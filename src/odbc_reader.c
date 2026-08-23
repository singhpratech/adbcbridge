// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// Result-set reader: ODBC rowsets -> Arrow record batches (ArrowArrayStream).

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// ---------------------------------------------------------------------------
// Handle refcounting

struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt) {
  struct OdbcHandleRef* ref = calloc(1, sizeof(struct OdbcHandleRef));
  if (!ref) return NULL;
  ref->hstmt = hstmt;
  ref->refcount = 1;
  return ref;
}

void OdbcHandleRefRelease(struct OdbcHandleRef* ref) {
  if (!ref) return;
  if (--ref->refcount == 0) {
    if (ref->hstmt) SQLFreeHandle(SQL_HANDLE_STMT, ref->hstmt);
    free(ref);
  }
}

// ---------------------------------------------------------------------------
// 32-bit-SQLLEN driver quirk wrappers (see OdbcReaderOptions::sqllen_32bit)

SQLLEN OdbcRowCount(SQLHSTMT hstmt, bool sqllen_32bit) {
  SQLLEN count = 0;  // zeroed so the driver's low 32 bits are the whole value
  if (!SQL_SUCCEEDED(SQLRowCount(hstmt, &count))) return -1;
  return OdbcReadLen(&count, sqllen_32bit);
}

SQLRETURN OdbcGetData(SQLHSTMT hstmt, SQLUSMALLINT col, SQLSMALLINT c_type, SQLPOINTER buf,
                      SQLLEN buf_len, SQLLEN* indicator, bool sqllen_32bit) {
  SQLLEN ind = 0;
  SQLRETURN ret = SQLGetData(hstmt, col, c_type, buf, buf_len, &ind);
  if (indicator) *indicator = OdbcReadLen(&ind, sqllen_32bit);
  return ret;
}

// ---------------------------------------------------------------------------
// Errors

static AdbcStatusCode SqlStateToStatus(const char* s) {
  if (!s || strlen(s) < 2) return ADBC_STATUS_UNKNOWN;
  if (strncmp(s, "08", 2) == 0) return ADBC_STATUS_IO;
  if (strncmp(s, "28", 2) == 0) return ADBC_STATUS_UNAUTHENTICATED;
  if (strncmp(s, "23", 2) == 0) return ADBC_STATUS_INTEGRITY;
  if (strncmp(s, "42S02", 5) == 0 || strncmp(s, "42S12", 5) == 0 ||
      strncmp(s, "42S22", 5) == 0)
    return ADBC_STATUS_NOT_FOUND;
  if (strncmp(s, "42S01", 5) == 0 || strncmp(s, "42S11", 5) == 0 ||
      strncmp(s, "42S21", 5) == 0)
    return ADBC_STATUS_ALREADY_EXISTS;
  if (strncmp(s, "42", 2) == 0 || strncmp(s, "22", 2) == 0 || strncmp(s, "07", 2) == 0)
    return ADBC_STATUS_INVALID_ARGUMENT;
  if (strncmp(s, "HYT0", 4) == 0) return ADBC_STATUS_TIMEOUT;
  if (strcmp(s, "HY008") == 0) return ADBC_STATUS_CANCELLED;
  if (strcmp(s, "HYC00") == 0 || strcmp(s, "IM001") == 0) return ADBC_STATUS_NOT_IMPLEMENTED;
  if (strcmp(s, "HY010") == 0) return ADBC_STATUS_INVALID_STATE;
  if (strncmp(s, "IM", 2) == 0) return ADBC_STATUS_INVALID_ARGUMENT;
  return ADBC_STATUS_UNKNOWN;
}

AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error) {
  SQLCHAR sqlstate[6] = {0};
  SQLINTEGER native = 0;
  SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
  SQLSMALLINT msg_len = 0;
  AdbcStatusCode status = ADBC_STATUS_UNKNOWN;
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 256);
  InternalAdbcStringBuilderAppend(&sb, "[ODBC] %s failed", context ? context : "call");

  SQLSMALLINT rec = 1;
  bool first = true;
  while (SQL_SUCCEEDED(SQLGetDiagRec(handle_type, handle, rec, sqlstate, &native, msg,
                                     sizeof(msg), &msg_len))) {
    if (first) {
      status = SqlStateToStatus((const char*)sqlstate);
      if (error) {
        memcpy(error->sqlstate, sqlstate, 5);
        error->vendor_code = (int32_t)native;
      }
      first = false;
    }
    InternalAdbcStringBuilderAppend(&sb, "\n  [%s] (%d) %.*s", (const char*)sqlstate,
                                    (int)native, (int)msg_len, (const char*)msg);
    if (error) {
      InternalAdbcAppendErrorDetail(error, "odbc.sqlstate", (const uint8_t*)sqlstate, 5);
    }
    rec++;
  }
  if (error) InternalAdbcSetError(error, "%s", sb.buffer ? sb.buffer : "[ODBC] unknown error");
  InternalAdbcStringBuilderReset(&sb);
  return status;
}

// ---------------------------------------------------------------------------
// Column description and type mapping

enum OdbcFetchKind {
  FETCH_BOOL,
  FETCH_I8,
  FETCH_I16,
  FETCH_I32,
  FETCH_I64,
  FETCH_U8,
  FETCH_U16,
  FETCH_U32,
  FETCH_U64,
  FETCH_F32,
  FETCH_F64,
  FETCH_CHAR,     // SQL_C_CHAR -> utf8 string
  FETCH_WCHAR,    // SQL_C_WCHAR -> utf8 string (converted)
  FETCH_BINARY,   // SQL_C_BINARY -> binary
  FETCH_DATE,     // DATE_STRUCT -> date32
  FETCH_TIME,     // TIME_STRUCT -> time32[s]
  FETCH_TIME64,   // SQL_C_CHAR "HH:MM:SS[.frac]" -> time64[us]
  FETCH_TIMESTAMP,// TIMESTAMP_STRUCT -> timestamp[us|ns]
  FETCH_TIMESTAMP_TZ,  // SQL_C_CHAR ISO-8601 with offset -> timestamp[us, UTC]
  FETCH_DECIMAL,  // SQL_C_CHAR -> decimal128
  FETCH_BOOL_STR, // SQL_C_CHAR ('t'/'1'/'true') -> bool (PostgreSQL, DuckDB report bool as char)
};

struct OdbcColumn {
  char* name;
  SQLSMALLINT sql_type;
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLSMALLINT nullable;
  enum OdbcFetchKind kind;
  SQLSMALLINT c_type;
  SQLLEN elem_size;  // bytes per row in the bound buffer
  bool bound;        // false => SQLGetData
  void* buffer;      // elem_size * rows
  SQLLEN* indicators;
  enum ArrowTimeUnit unit;
  int32_t precision;
  int32_t scale;
};

static bool IsUnsigned(SQLHSTMT hstmt, SQLUSMALLINT col, bool sqllen_32bit) {
  SQLLEN v = 0;  // zeroed: a 32-bit-SQLLEN driver only writes the low half
  if (!hstmt) return false;
  if (SQL_SUCCEEDED(SQLColAttribute(hstmt, col, SQL_DESC_UNSIGNED, NULL, 0, NULL, &v))) {
    return OdbcReadLen(&v, sqllen_32bit) == SQL_TRUE;
  }
  return false;
}

static bool TypeNameIsBool(SQLHSTMT hstmt, SQLUSMALLINT col) {
  SQLCHAR name[64] = {0};
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLColAttribute(hstmt, col, SQL_DESC_TYPE_NAME, name, sizeof(name), &len, NULL))) {
    return false;
  }
  for (SQLSMALLINT i = 0; i < len && i < (SQLSMALLINT)sizeof(name); i++) {
    if (name[i] >= 'A' && name[i] <= 'Z') name[i] = (SQLCHAR)(name[i] - 'A' + 'a');
  }
  return strcmp((const char*)name, "bool") == 0 || strcmp((const char*)name, "boolean") == 0;
}

// Case-insensitive substring search (needle must be lowercase).
static bool ContainsFold(const char* hay, const char* needle) {
  size_t nl = strlen(needle);
  if (nl == 0) return true;
  for (const char* p = hay; *p; p++) {
    size_t i = 0;
    while (i < nl) {
      char ch = p[i];
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      if (ch != needle[i]) break;
      i++;
    }
    if (i == nl) return true;
  }
  return false;
}

// Some drivers report a timezone-aware timestamp as a plain SQL_TYPE_TIMESTAMP
// (PostgreSQL's "timestamptz", Oracle's "TIMESTAMP WITH TIME ZONE", ...).  The
// only portable hint is the driver's own type name.
static bool IsTimestampWithTimezone(SQLHSTMT hstmt, SQLUSMALLINT col) {
  SQLCHAR name[128] = {0};
  SQLSMALLINT len = 0;
  if (!hstmt) return false;
  if (!SQL_SUCCEEDED(SQLColAttribute(hstmt, col, SQL_DESC_TYPE_NAME, name, sizeof(name), &len,
                                     NULL))) {
    return false;
  }
  name[sizeof(name) - 1] = '\0';
  return ContainsFold((const char*)name, "with time zone") ||
         ContainsFold((const char*)name, "timestamptz") ||
         ContainsFold((const char*)name, "timestampoffset");
}

// Fetch a column as SQL_C_CHAR, with a buffer big enough for `column_size`
// characters but never smaller than `minimum` (drivers report column_size 0 for
// types whose length they don't track).  Falls back to SQLGetData if that would
// exceed the caller's binding budget.
static void UseTextBuffer(struct OdbcColumn* c, const struct OdbcReaderOptions* opts,
                          SQLLEN minimum) {
  c->c_type = SQL_C_CHAR;
  c->elem_size = (SQLLEN)c->column_size + 8;
  if (c->elem_size < minimum) c->elem_size = minimum;
  if (c->elem_size > opts->max_bind_bytes) c->bound = false;
}

static void ClassifyColumn(SQLHSTMT hstmt, SQLUSMALLINT icol, struct OdbcColumn* c,
                           const struct OdbcReaderOptions* opts) {
  c->bound = true;
  if ((c->sql_type == SQL_CHAR || c->sql_type == SQL_VARCHAR || c->sql_type == SQL_WCHAR ||
       c->sql_type == SQL_WVARCHAR) &&
      c->column_size <= 8 && TypeNameIsBool(hstmt, icol)) {
    c->kind = FETCH_BOOL_STR; c->c_type = SQL_C_CHAR; c->elem_size = 16;
    return;
  }
  switch (c->sql_type) {
    case SQL_BIT:
      c->kind = FETCH_BOOL; c->c_type = SQL_C_BIT; c->elem_size = sizeof(unsigned char);
      break;
    case SQL_TINYINT:
      if (IsUnsigned(hstmt, icol, opts->sqllen_32bit)) { c->kind = FETCH_U8; c->c_type = SQL_C_UTINYINT; }
      else { c->kind = FETCH_I8; c->c_type = SQL_C_STINYINT; }
      c->elem_size = 1;
      break;
    case SQL_SMALLINT:
      if (IsUnsigned(hstmt, icol, opts->sqllen_32bit)) { c->kind = FETCH_U16; c->c_type = SQL_C_USHORT; }
      else { c->kind = FETCH_I16; c->c_type = SQL_C_SSHORT; }
      c->elem_size = sizeof(SQLSMALLINT);
      break;
    case SQL_INTEGER:
      if (IsUnsigned(hstmt, icol, opts->sqllen_32bit)) { c->kind = FETCH_U32; c->c_type = SQL_C_ULONG; }
      else { c->kind = FETCH_I32; c->c_type = SQL_C_SLONG; }
      c->elem_size = sizeof(SQLINTEGER);
      break;
    case SQL_BIGINT:
      if (IsUnsigned(hstmt, icol, opts->sqllen_32bit)) { c->kind = FETCH_U64; c->c_type = SQL_C_UBIGINT; }
      else { c->kind = FETCH_I64; c->c_type = SQL_C_SBIGINT; }
      c->elem_size = sizeof(SQLBIGINT);
      break;
    case SQL_REAL:
      c->kind = FETCH_F32; c->c_type = SQL_C_FLOAT; c->elem_size = sizeof(SQLREAL);
      break;
    case SQL_FLOAT:
    case SQL_DOUBLE:
      c->kind = FETCH_F64; c->c_type = SQL_C_DOUBLE; c->elem_size = sizeof(SQLDOUBLE);
      break;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      c->precision = (int32_t)c->column_size;
      c->scale = (int32_t)c->decimal_digits;
      if (!opts->decimal_as_string && c->precision > 0 && c->precision <= 38 &&
          c->scale >= 0 && c->scale <= c->precision) {
        c->kind = FETCH_DECIMAL;
      } else {
        c->kind = FETCH_CHAR;
      }
      c->c_type = SQL_C_CHAR;
      c->elem_size = (SQLLEN)c->column_size + 4;  // sign, point, terminator
      if (c->elem_size < 48) c->elem_size = 48;
      break;
    case SQL_TYPE_DATE:
    case SQL_DATE:
      c->kind = FETCH_DATE; c->c_type = SQL_C_TYPE_DATE; c->elem_size = sizeof(DATE_STRUCT);
      break;
    case SQL_TYPE_TIME:
    case SQL_TIME:
    case SQL_SS_TIME2:
      // TIME_STRUCT has no sub-second field, so a column with fractional
      // seconds has to come across as text.
      if (c->decimal_digits > 0) {
        c->kind = FETCH_TIME64;
        UseTextBuffer(c, opts, 40);
      } else {
        c->kind = FETCH_TIME; c->c_type = SQL_C_TYPE_TIME; c->elem_size = sizeof(TIME_STRUCT);
      }
      break;
    case SQL_TYPE_TIME_WITH_TIMEZONE:
      // Arrow has no time-with-timezone type; keep the driver's text form.
      c->kind = FETCH_CHAR;
      UseTextBuffer(c, opts, 40);
      break;
    case SQL_SS_TIMESTAMPOFFSET:
    case SQL_TYPE_TIMESTAMP_WITH_TIMEZONE:
      c->kind = FETCH_TIMESTAMP_TZ;
      c->unit = NANOARROW_TIME_UNIT_MICRO;
      UseTextBuffer(c, opts, 80);
      break;
    case SQL_TYPE_TIMESTAMP:
    case SQL_TIMESTAMP:
      if (IsTimestampWithTimezone(hstmt, icol)) {
        c->kind = FETCH_TIMESTAMP_TZ;
        c->unit = NANOARROW_TIME_UNIT_MICRO;
        UseTextBuffer(c, opts, 80);
        break;
      }
      c->kind = FETCH_TIMESTAMP; c->c_type = SQL_C_TYPE_TIMESTAMP;
      c->elem_size = sizeof(TIMESTAMP_STRUCT);
      c->unit = c->decimal_digits > 6 ? NANOARROW_TIME_UNIT_NANO : NANOARROW_TIME_UNIT_MICRO;
      break;
    case SQL_GUID:
      // 36 characters, or 38 with the braces some drivers add.
      c->kind = FETCH_CHAR;
      UseTextBuffer(c, opts, 64);
      break;
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      // Arrow's interval types cannot represent every ODBC interval qualifier
      // (day-to-second with a leading precision, year-to-month, ...), so the
      // driver's textual rendering is the lossless choice.
      c->kind = FETCH_CHAR;
      UseTextBuffer(c, opts, 64);
      break;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      c->kind = FETCH_BINARY; c->c_type = SQL_C_BINARY;
      c->elem_size = (SQLLEN)c->column_size;
      if (c->column_size == 0 || c->elem_size > opts->max_bind_bytes ||
          c->sql_type == SQL_LONGVARBINARY) {
        c->bound = false;
      }
      break;
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      c->kind = FETCH_WCHAR; c->c_type = SQL_C_WCHAR;
      c->elem_size = ((SQLLEN)c->column_size + 1) * (SQLLEN)sizeof(SQLWCHAR);
      if (c->column_size == 0 || c->elem_size > opts->max_bind_bytes ||
          c->sql_type == SQL_WLONGVARCHAR) {
        c->bound = false;
      }
      break;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    default:
      // Anything unknown: ask the driver for a string representation.
      c->kind = FETCH_CHAR; c->c_type = SQL_C_CHAR;
      // column_size is in characters; UTF-8 may need up to 4 bytes each.
      c->elem_size = (SQLLEN)c->column_size * 4 + 1;
      if (c->column_size == 0 || c->elem_size > opts->max_bind_bytes ||
          c->sql_type == SQL_LONGVARCHAR) {
        c->bound = false;
      }
      break;
  }
}

static AdbcStatusCode DescribeColumns(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                      struct OdbcColumn** out_cols, SQLSMALLINT* out_n,
                                      struct AdbcError* error) {
  SQLSMALLINT n = 0;
  ODBC_CHECK(SQLNumResultCols(hstmt, &n), SQL_HANDLE_STMT, hstmt, "SQLNumResultCols", error);
  struct OdbcColumn* cols = calloc(n > 0 ? n : 1, sizeof(struct OdbcColumn));
  if (!cols) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  for (SQLSMALLINT i = 0; i < n; i++) {
    SQLCHAR name[1024];
    SQLSMALLINT name_len = 0;
    struct OdbcColumn* c = &cols[i];
    // c->column_size is a SQLULEN out-parameter: calloc has zeroed it, so a driver that
    // writes only its low four bytes still leaves a well-defined value behind.
    SQLRETURN ret = SQLDescribeCol(hstmt, (SQLUSMALLINT)(i + 1), name, sizeof(name), &name_len,
                                   &c->sql_type, &c->column_size, &c->decimal_digits,
                                   &c->nullable);
    c->column_size = OdbcReadULen(&c->column_size, opts->sqllen_32bit);
    if (!SQL_SUCCEEDED(ret)) {
      AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLDescribeCol", error);
      for (SQLSMALLINT j = 0; j < i; j++) free(cols[j].name);
      free(cols);
      return s;
    }
    if (name_len <= 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "column%d", (int)i);
      c->name = strdup(buf);
    } else {
      if ((size_t)name_len >= sizeof(name)) name_len = sizeof(name) - 1;
      c->name = strndup((const char*)name, (size_t)name_len);
    }
    ClassifyColumn(hstmt, (SQLUSMALLINT)(i + 1), c, opts);
  }
  // ODBC (and SQL Server strictly) requires SQLGetData columns to come after all bound
  // columns and to be read in increasing order: unbind everything after the first
  // unbound column.
  bool seen_unbound = false;
  for (SQLSMALLINT i = 0; i < n; i++) {
    if (!cols[i].bound) seen_unbound = true;
    else if (seen_unbound) cols[i].bound = false;
  }
  *out_cols = cols;
  *out_n = n;
  return ADBC_STATUS_OK;
}

static void FreeColumns(struct OdbcColumn* cols, SQLSMALLINT n) {
  if (!cols) return;
  for (SQLSMALLINT i = 0; i < n; i++) {
    free(cols[i].name);
    free(cols[i].buffer);
    free(cols[i].indicators);
  }
  free(cols);
}

static AdbcStatusCode BuildSchema(const struct OdbcColumn* cols, SQLSMALLINT n,
                                  struct ArrowSchema* out, struct AdbcError* error) {
  ArrowSchemaInit(out);
  CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(out, n), error);
  for (SQLSMALLINT i = 0; i < n; i++) {
    const struct OdbcColumn* c = &cols[i];
    struct ArrowSchema* f = out->children[i];
    enum ArrowType t = NANOARROW_TYPE_STRING;
    switch (c->kind) {
      case FETCH_BOOL:
      case FETCH_BOOL_STR: t = NANOARROW_TYPE_BOOL; break;
      case FETCH_I8: t = NANOARROW_TYPE_INT8; break;
      case FETCH_I16: t = NANOARROW_TYPE_INT16; break;
      case FETCH_I32: t = NANOARROW_TYPE_INT32; break;
      case FETCH_I64: t = NANOARROW_TYPE_INT64; break;
      case FETCH_U8: t = NANOARROW_TYPE_UINT8; break;
      case FETCH_U16: t = NANOARROW_TYPE_UINT16; break;
      case FETCH_U32: t = NANOARROW_TYPE_UINT32; break;
      case FETCH_U64: t = NANOARROW_TYPE_UINT64; break;
      case FETCH_F32: t = NANOARROW_TYPE_FLOAT; break;
      case FETCH_F64: t = NANOARROW_TYPE_DOUBLE; break;
      case FETCH_CHAR:
      case FETCH_WCHAR: t = NANOARROW_TYPE_STRING; break;
      case FETCH_BINARY: t = NANOARROW_TYPE_BINARY; break;
      case FETCH_DATE: t = NANOARROW_TYPE_DATE32; break;
      case FETCH_TIME:
        CHECK_NA(INTERNAL,
                 ArrowSchemaSetTypeDateTime(f, NANOARROW_TYPE_TIME32,
                                            NANOARROW_TIME_UNIT_SECOND, NULL),
                 error);
        goto named;
      case FETCH_TIME64:
        CHECK_NA(INTERNAL,
                 ArrowSchemaSetTypeDateTime(f, NANOARROW_TYPE_TIME64,
                                            NANOARROW_TIME_UNIT_MICRO, NULL),
                 error);
        goto named;
      case FETCH_TIMESTAMP:
        CHECK_NA(INTERNAL,
                 ArrowSchemaSetTypeDateTime(f, NANOARROW_TYPE_TIMESTAMP, c->unit, NULL),
                 error);
        goto named;
      case FETCH_TIMESTAMP_TZ:
        CHECK_NA(INTERNAL,
                 ArrowSchemaSetTypeDateTime(f, NANOARROW_TYPE_TIMESTAMP,
                                            NANOARROW_TIME_UNIT_MICRO, "UTC"),
                 error);
        goto named;
      case FETCH_DECIMAL:
        CHECK_NA(INTERNAL,
                 ArrowSchemaSetTypeDecimal(f, NANOARROW_TYPE_DECIMAL128, c->precision,
                                           c->scale),
                 error);
        goto named;
    }
    CHECK_NA(INTERNAL, ArrowSchemaSetType(f, t), error);
  named:
    CHECK_NA(INTERNAL, ArrowSchemaSetName(f, c->name), error);
    if (c->nullable == SQL_NO_NULLS) {
      f->flags &= ~ARROW_FLAG_NULLABLE;
    }
  }
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcDescribeResultSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                        struct ArrowSchema* out, struct AdbcError* error) {
  struct OdbcColumn* cols = NULL;
  SQLSMALLINT n = 0;
  RAISE_ADBC(DescribeColumns(hstmt, opts, &cols, &n, error));
  AdbcStatusCode s = BuildSchema(cols, n, out, error);
  FreeColumns(cols, n);
  return s;
}

// ---------------------------------------------------------------------------
// Value conversion helpers

// Howard Hinnant's days_from_civil.
static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// Scan up to `max_digits` ASCII digits at s[*pos]; false if there are none.
static bool ScanUInt(const char* s, size_t len, size_t* pos, int max_digits, int64_t* out) {
  int64_t v = 0;
  int n = 0;
  while (*pos < len && n < max_digits && s[*pos] >= '0' && s[*pos] <= '9') {
    v = v * 10 + (s[*pos] - '0');
    (*pos)++;
    n++;
  }
  if (n == 0) return false;
  *out = v;
  return true;
}

// Scan ".ffffff" (or ",ffffff") into microseconds, truncating extra digits.
static void ScanFraction(const char* s, size_t len, size_t* pos, int64_t* out_micros) {
  *out_micros = 0;
  if (*pos >= len || (s[*pos] != '.' && s[*pos] != ',')) return;
  (*pos)++;
  int digits = 0;
  while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
    if (digits < 6) {
      *out_micros = *out_micros * 10 + (s[*pos] - '0');
      digits++;
    }
    (*pos)++;
  }
  while (digits < 6) {
    *out_micros *= 10;
    digits++;
  }
}

static void SkipBlanks(const char* s, size_t len, size_t* pos) {
  while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\0')) (*pos)++;
}

// "HH:MM[:SS[.frac]]" -> microseconds since midnight.
static bool ParseTimeMicros(const char* s, size_t len, int64_t* out) {
  size_t p = 0;
  int64_t h = 0, m = 0, sec = 0, frac = 0;
  SkipBlanks(s, len, &p);
  if (!ScanUInt(s, len, &p, 2, &h)) return false;
  if (p >= len || s[p] != ':') return false;
  p++;
  if (!ScanUInt(s, len, &p, 2, &m)) return false;
  if (p < len && s[p] == ':') {
    p++;
    if (!ScanUInt(s, len, &p, 2, &sec)) return false;
  }
  ScanFraction(s, len, &p, &frac);
  SkipBlanks(s, len, &p);
  if (p != len) return false;
  if (h > 23 || m > 59 || sec > 59) return false;
  *out = ((h * 60 + m) * 60 + sec) * 1000000LL + frac;
  return true;
}

// "YYYY-MM-DD[ T]HH:MM[:SS[.frac]][Z|(+|-)HH[:]MM]" -> microseconds since the
// Unix epoch in UTC.  A missing offset is taken as UTC.
static bool ParseTimestampUtcMicros(const char* s, size_t len, int64_t* out) {
  size_t p = 0;
  int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, frac = 0, offset_secs = 0;
  SkipBlanks(s, len, &p);
  if (!ScanUInt(s, len, &p, 4, &y)) return false;
  if (p >= len || s[p] != '-') return false;
  p++;
  if (!ScanUInt(s, len, &p, 2, &mo)) return false;
  if (p >= len || s[p] != '-') return false;
  p++;
  if (!ScanUInt(s, len, &p, 2, &d)) return false;
  if (p < len && (s[p] == ' ' || s[p] == 'T' || s[p] == 't')) {
    p++;
    SkipBlanks(s, len, &p);
    if (!ScanUInt(s, len, &p, 2, &h)) return false;
    if (p >= len || s[p] != ':') return false;
    p++;
    if (!ScanUInt(s, len, &p, 2, &mi)) return false;
    if (p < len && s[p] == ':') {
      p++;
      if (!ScanUInt(s, len, &p, 2, &sec)) return false;
    }
    ScanFraction(s, len, &p, &frac);
  }
  SkipBlanks(s, len, &p);
  if (p < len && (s[p] == 'Z' || s[p] == 'z')) {
    p++;
  } else if (p < len && (s[p] == '+' || s[p] == '-')) {
    int sign = s[p] == '-' ? -1 : 1;
    int64_t oh = 0, om = 0;
    p++;
    if (!ScanUInt(s, len, &p, 2, &oh)) return false;
    if (p < len && s[p] == ':') {
      p++;
      if (!ScanUInt(s, len, &p, 2, &om)) return false;
    } else if (p < len && s[p] >= '0' && s[p] <= '9') {
      if (!ScanUInt(s, len, &p, 2, &om)) return false;
    }
    if (oh > 23 || om > 59) return false;
    offset_secs = sign * (oh * 3600 + om * 60);
  }
  SkipBlanks(s, len, &p);
  if (p != len) return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) return false;
  int64_t secs = DaysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + sec;
  *out = (secs - offset_secs) * 1000000LL + frac;
  return true;
}

// Append a UTF-16 buffer (n units) to a utf8 string array.  Surrogate pairs
// become 4-byte sequences (non-BMP characters such as emoji); an unpaired
// surrogate becomes U+FFFD so the output is always valid UTF-8.
static ArrowErrorCode AppendUtf16(struct ArrowArray* arr, const SQLWCHAR* w, size_t n,
                                  struct ArrowBuffer* scratch) {
  scratch->size_bytes = 0;
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(scratch, (int64_t)n * 4 + 1));
  uint8_t* o = scratch->data;
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = (uint16_t)w[i];
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      uint32_t lo = (i + 1 < n) ? (uint16_t)w[i + 1] : 0;
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      } else {
        cp = 0xFFFD;  // high surrogate with no low surrogate following
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;  // low surrogate with no high surrogate preceding
    }
    if (cp < 0x80) {
      o[k++] = (uint8_t)cp;
    } else if (cp < 0x800) {
      o[k++] = (uint8_t)(0xC0 | (cp >> 6));
      o[k++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      o[k++] = (uint8_t)(0xE0 | (cp >> 12));
      o[k++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      o[k++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
      o[k++] = (uint8_t)(0xF0 | (cp >> 18));
      o[k++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
      o[k++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
      o[k++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
  }
  struct ArrowStringView sv = {(const char*)o, (int64_t)k};
  return ArrowArrayAppendString(arr, sv);
}

// Parse a decimal string like "-123.45" into a decimal128 with the column's scale.
static ArrowErrorCode AppendDecimalString(struct ArrowArray* arr, const char* s, size_t len,
                                          int32_t precision, int32_t scale) {
  char digits[64];
  size_t nd = 0;
  bool neg = false;
  int32_t frac_seen = -1;  // number of fractional digits collected, -1 before '.'
  size_t i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '+')) i++;
  if (i < len && s[i] == '-') { neg = true; i++; }
  for (; i < len; i++) {
    char ch = s[i];
    if (ch >= '0' && ch <= '9') {
      if (frac_seen >= 0) {
        if (frac_seen >= scale) continue;  // truncate extra fractional digits
        frac_seen++;
      }
      if (nd < sizeof(digits) - 1) digits[nd++] = ch;
    } else if (ch == '.' && frac_seen < 0) {
      frac_seen = 0;
    } else if (ch == '\0' || ch == ' ') {
      break;
    } else {
      return EINVAL;
    }
  }
  if (frac_seen < 0) frac_seen = 0;
  while (frac_seen < scale && nd < sizeof(digits) - 1) { digits[nd++] = '0'; frac_seen++; }
  if (nd == 0) digits[nd++] = '0';
  digits[nd] = '\0';
  struct ArrowDecimal dec;
  ArrowDecimalInit(&dec, 128, precision, scale);
  struct ArrowStringView sv = {digits, (int64_t)nd};
  NANOARROW_RETURN_NOT_OK(ArrowDecimalSetDigits(&dec, sv));
  if (neg) ArrowDecimalNegate(&dec);
  return ArrowArrayAppendDecimal(arr, &dec);
}

// ---------------------------------------------------------------------------
// Reader

struct OdbcReader {
  struct OdbcHandleRef* ref;
  struct OdbcReaderOptions opts;
  struct OdbcColumn* cols;
  SQLSMALLINT ncols;
  struct ArrowSchema schema;
  SQLULEN rows_per_fetch;
  SQLULEN rows_fetched;
  SQLUSMALLINT* row_status;
  bool done;
  bool bound;
  struct ArrowBuffer scratch;  // for SQLGetData chunks / utf16 conversion
  struct AdbcError error;
  char error_message[1024];
};

static AdbcStatusCode ReaderBind(struct OdbcReader* r, struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  bool all_bound = true;
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    if (!r->cols[i].bound) all_bound = false;
  }
  r->rows_per_fetch = all_bound ? (SQLULEN)r->opts.batch_size : 1;
  if (r->rows_per_fetch < 1) r->rows_per_fetch = 1;
  if (r->opts.min_buffer_rows > 0 && all_bound) {
    // Round up to a multiple of the driver's internal chunk so rows stay aligned.
    SQLULEN m = (SQLULEN)r->opts.min_buffer_rows;
    r->rows_per_fetch = ((r->rows_per_fetch + m - 1) / m) * m;
  }
  SQLULEN capacity = r->rows_per_fetch;
  if (r->opts.min_buffer_rows > 0 && capacity < (SQLULEN)r->opts.min_buffer_rows) {
    capacity = (SQLULEN)r->opts.min_buffer_rows;
  }

  // Column-wise binding is the ODBC default; some drivers (DuckDB) reject setting it
  // explicitly, so this is best-effort.
  SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0);
  SQLRETURN ret = SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)r->rows_per_fetch, 0);
  if (!SQL_SUCCEEDED(ret)) {
    // Driver doesn't support block cursors: fall back to one row at a time.
    r->rows_per_fetch = 1;
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0);
  }
  r->row_status = calloc(capacity, sizeof(SQLUSMALLINT));
  SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_STATUS_PTR, r->row_status, 0);
  SQLSetStmtAttr(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &r->rows_fetched, 0);

  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    struct OdbcColumn* c = &r->cols[i];
    // A 32-bit-SQLLEN driver fills this as int32[capacity] (stride 4), which fits inside
    // the same allocation; OdbcIndicatorGet() reads it back with the right stride.
    c->indicators = calloc(capacity, sizeof(SQLLEN));
    if (!c->indicators) {
      InternalAdbcSetError(error, "out of memory");
      return ADBC_STATUS_INTERNAL;
    }
    if (!c->bound) continue;
    c->buffer = calloc(capacity, (size_t)c->elem_size);
    if (!c->buffer) {
      InternalAdbcSetError(error, "out of memory binding column %s", c->name);
      return ADBC_STATUS_INTERNAL;
    }
    ODBC_CHECK(SQLBindCol(hstmt, (SQLUSMALLINT)(i + 1), c->c_type, c->buffer, c->elem_size,
                          c->indicators),
               SQL_HANDLE_STMT, hstmt, "SQLBindCol", error);
  }
  r->bound = true;
  return ADBC_STATUS_OK;
}

// Fetch one unbound column value for the current row via SQLGetData into scratch.
// Returns status; sets *is_null; data is in r->scratch (size_bytes).
static AdbcStatusCode GetDataLong(struct OdbcReader* r, SQLSMALLINT i, bool* is_null,
                                  struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  struct OdbcColumn* c = &r->cols[i];
  r->scratch.size_bytes = 0;
  *is_null = false;
  const size_t chunk = 65536;
  size_t term = 0;
  if (c->c_type == SQL_C_CHAR) term = 1;
  else if (c->c_type == SQL_C_WCHAR) term = sizeof(SQLWCHAR);

  for (;;) {
    CHECK_NA(INTERNAL, ArrowBufferReserve(&r->scratch, (int64_t)chunk), error);
    uint8_t* dst = r->scratch.data + r->scratch.size_bytes;
    SQLLEN ind = 0;
    SQLRETURN ret = OdbcGetData(hstmt, (SQLUSMALLINT)(i + 1), c->c_type, dst, (SQLLEN)chunk, &ind,
                                r->opts.sqllen_32bit);
    if (ret == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(ret)) {
      return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLGetData", error);
    }
    if (ind == SQL_NULL_DATA) {
      *is_null = true;
      return ADBC_STATUS_OK;
    }
    size_t got;
    if (ind == SQL_NO_TOTAL || (size_t)ind >= chunk) {
      got = chunk - term;
    } else {
      got = (size_t)ind;
    }
    r->scratch.size_bytes += (int64_t)got;
    if (ret == SQL_SUCCESS) break;  // SQL_SUCCESS_WITH_INFO => more data (01004)
    // Check whether the "with info" was truncation; if not, we're done.
    SQLCHAR state[6] = {0};
    SQLINTEGER native;
    SQLSMALLINT len;
    if (SQL_SUCCEEDED(SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, &native, NULL, 0, &len)) &&
        strcmp((const char*)state, "01004") == 0) {
      continue;
    }
    break;
  }
  return ADBC_STATUS_OK;
}

static AdbcStatusCode AppendValue(struct OdbcReader* r, SQLSMALLINT i, SQLULEN row,
                                  struct ArrowArray* arr, struct AdbcError* error) {
  struct OdbcColumn* c = &r->cols[i];
  const uint8_t* data;
  size_t len;
  bool is_null;

  if (c->bound) {
    SQLLEN ind = OdbcIndicatorGet(c->indicators, (size_t)row, r->opts.sqllen_32bit);
    is_null = (ind == SQL_NULL_DATA);
    data = (const uint8_t*)c->buffer + (size_t)row * (size_t)c->elem_size;
    if (ind == SQL_NO_TOTAL || ind < 0) {
      len = 0;
    } else {
      len = (size_t)ind;
      // Truncated value (data longer than bound buffer) - clamp.
      size_t cap = (size_t)c->elem_size - (c->c_type == SQL_C_CHAR ? 1
                                          : c->c_type == SQL_C_WCHAR ? sizeof(SQLWCHAR) : 0);
      if (len > cap) len = cap;
    }
  } else {
    RAISE_ADBC(GetDataLong(r, i, &is_null, error));
    data = r->scratch.data;
    len = (size_t)r->scratch.size_bytes;
  }

  if (is_null) {
    CHECK_NA(INTERNAL, ArrowArrayAppendNull(arr, 1), error);
    return ADBC_STATUS_OK;
  }

  switch (c->kind) {
    case FETCH_BOOL:
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, *(const unsigned char*)data ? 1 : 0), error);
      break;
    case FETCH_I8: CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, *(const int8_t*)data), error); break;
    case FETCH_I16: CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, *(const SQLSMALLINT*)data), error); break;
    case FETCH_I32: CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, *(const SQLINTEGER*)data), error); break;
    case FETCH_I64: CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, *(const SQLBIGINT*)data), error); break;
    case FETCH_U8: CHECK_NA(INTERNAL, ArrowArrayAppendUInt(arr, *(const uint8_t*)data), error); break;
    case FETCH_U16: CHECK_NA(INTERNAL, ArrowArrayAppendUInt(arr, *(const SQLUSMALLINT*)data), error); break;
    case FETCH_U32: CHECK_NA(INTERNAL, ArrowArrayAppendUInt(arr, *(const SQLUINTEGER*)data), error); break;
    case FETCH_U64: CHECK_NA(INTERNAL, ArrowArrayAppendUInt(arr, *(const SQLUBIGINT*)data), error); break;
    // NaN and +/-Inf are passed through verbatim: they are valid Arrow float
    // values, so they must not be turned into nulls or rejected here.
    case FETCH_F32: CHECK_NA(INTERNAL, ArrowArrayAppendDouble(arr, *(const SQLREAL*)data), error); break;
    case FETCH_F64: CHECK_NA(INTERNAL, ArrowArrayAppendDouble(arr, *(const SQLDOUBLE*)data), error); break;
    case FETCH_CHAR: {
      struct ArrowStringView sv = {(const char*)data, (int64_t)len};
      CHECK_NA(INTERNAL, ArrowArrayAppendString(arr, sv), error);
      break;
    }
    case FETCH_WCHAR: {
      // Need a separate scratch when data already lives in r->scratch.
      struct ArrowBuffer* tmp = &r->scratch;
      struct ArrowBuffer local;
      if (!c->bound) { ArrowBufferInit(&local); tmp = &local; }
      ArrowErrorCode ec = AppendUtf16(arr, (const SQLWCHAR*)data, len / sizeof(SQLWCHAR), tmp);
      if (!c->bound) ArrowBufferReset(&local);
      CHECK_NA(INTERNAL, ec, error);
      break;
    }
    case FETCH_BINARY: {
      struct ArrowBufferView bv;
      bv.data.as_uint8 = data;
      bv.size_bytes = (int64_t)len;
      CHECK_NA(INTERNAL, ArrowArrayAppendBytes(arr, bv), error);
      break;
    }
    case FETCH_DATE: {
      const DATE_STRUCT* d = (const DATE_STRUCT*)data;
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, DaysFromCivil(d->year, d->month, d->day)), error);
      break;
    }
    case FETCH_TIME: {
      const TIME_STRUCT* t = (const TIME_STRUCT*)data;
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, t->hour * 3600 + t->minute * 60 + t->second), error);
      break;
    }
    case FETCH_TIME64: {
      int64_t v = 0;
      if (!ParseTimeMicros((const char*)data, len, &v)) {
        InternalAdbcSetError(error, "Could not parse time value '%.*s' for column %s", (int)len,
                             (const char*)data, c->name);
        return ADBC_STATUS_INVALID_DATA;
      }
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, v), error);
      break;
    }
    case FETCH_TIMESTAMP_TZ: {
      int64_t v = 0;
      if (!ParseTimestampUtcMicros((const char*)data, len, &v)) {
        InternalAdbcSetError(error, "Could not parse timestamp value '%.*s' for column %s",
                             (int)len, (const char*)data, c->name);
        return ADBC_STATUS_INVALID_DATA;
      }
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, v), error);
      break;
    }
    case FETCH_TIMESTAMP: {
      const TIMESTAMP_STRUCT* t = (const TIMESTAMP_STRUCT*)data;
      int64_t secs = DaysFromCivil(t->year, t->month, t->day) * 86400 + t->hour * 3600 +
                     t->minute * 60 + t->second;
      int64_t v = (c->unit == NANOARROW_TIME_UNIT_NANO)
                      ? secs * 1000000000LL + (int64_t)t->fraction
                      : secs * 1000000LL + (int64_t)t->fraction / 1000;
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, v), error);
      break;
    }
    case FETCH_BOOL_STR: {
      char ch = len > 0 ? (char)data[0] : '0';
      bool v = (ch == 't' || ch == 'T' || ch == '1' || ch == 'y' || ch == 'Y');
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, v ? 1 : 0), error);
      break;
    }
    case FETCH_DECIMAL: {
      ArrowErrorCode ec = AppendDecimalString(arr, (const char*)data, len, c->precision, c->scale);
      if (ec != NANOARROW_OK) {
        InternalAdbcSetError(error, "Could not parse decimal value '%.*s' for column %s",
                             (int)len, (const char*)data, c->name);
        return ADBC_STATUS_INVALID_DATA;
      }
      break;
    }
  }
  return ADBC_STATUS_OK;
}

static AdbcStatusCode ReaderNextBatch(struct OdbcReader* r, struct ArrowArray* out,
                                      struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  if (!r->bound) RAISE_ADBC(ReaderBind(r, error));

  struct ArrowArray batch;
  batch.release = NULL;
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&batch, &r->schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&batch), error);

  int64_t total = 0;
  AdbcStatusCode status = ADBC_STATUS_OK;
  while (total < r->opts.batch_size && !r->done) {
    r->rows_fetched = 0;
    SQLRETURN ret = SQLFetch(hstmt);
    if (ret == SQL_NO_DATA) {
      r->done = true;
      break;
    }
    if (!SQL_SUCCEEDED(ret)) {
      status = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLFetch", error);
      break;
    }
    // SQL_ATTR_ROWS_FETCHED_PTR is a SQLULEN the driver writes; it was zeroed above so a
    // 32-bit-SQLLEN driver's low half is the whole count.
    const SQLULEN fetched = OdbcReadULen(&r->rows_fetched, r->opts.sqllen_32bit);
    for (SQLULEN row = 0; row < fetched && status == ADBC_STATUS_OK; row++) {
      if (r->row_status[row] == SQL_ROW_NOROW) continue;
      if (r->row_status[row] == SQL_ROW_ERROR) {
        InternalAdbcSetError(error, "[ODBC] row %lu reported SQL_ROW_ERROR",
                             (unsigned long)row);
        status = ADBC_STATUS_IO;
        break;
      }
      for (SQLSMALLINT i = 0; i < r->ncols; i++) {
        status = AppendValue(r, i, row, batch.children[i], error);
        if (status != ADBC_STATUS_OK) break;
      }
      total++;
    }
    if (status != ADBC_STATUS_OK) break;
    if (r->rows_per_fetch == 1 && total >= r->opts.batch_size) break;
  }
  if (status != ADBC_STATUS_OK) {
    ArrowArrayRelease(&batch);
    return status;
  }
  if (total == 0) {
    ArrowArrayRelease(&batch);
    out->release = NULL;  // end of stream
    return ADBC_STATUS_OK;
  }
  batch.length = total;
  struct ArrowError na_error;
  ArrowErrorCode ec = ArrowArrayFinishBuildingDefault(&batch, &na_error);
  if (ec != NANOARROW_OK) {
    ArrowArrayRelease(&batch);
    InternalAdbcSetError(error, "Failed to finish batch: %s", na_error.message);
    return ADBC_STATUS_INTERNAL;
  }
  ArrowArrayMove(&batch, out);
  return ADBC_STATUS_OK;
}

static void ReaderRelease(struct ArrowArrayStream* stream) {
  struct OdbcReader* r = (struct OdbcReader*)stream->private_data;
  if (r) {
    if (r->ref && r->ref->hstmt) {
      SQLCloseCursor(r->ref->hstmt);
      SQLFreeStmt(r->ref->hstmt, SQL_UNBIND);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROW_STATUS_PTR, NULL, 0);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROWS_FETCHED_PTR, NULL, 0);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0);
    }
    OdbcHandleRefRelease(r->ref);
    FreeColumns(r->cols, r->ncols);
    free(r->row_status);
    if (r->schema.release) r->schema.release(&r->schema);
    ArrowBufferReset(&r->scratch);
    if (r->error.release) r->error.release(&r->error);
    free(r);
  }
  stream->private_data = NULL;
  stream->release = NULL;
}

static int ReaderGetSchema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
  struct OdbcReader* r = (struct OdbcReader*)stream->private_data;
  if (!r) return EINVAL;
  return ArrowSchemaDeepCopy(&r->schema, out);
}

static int ReaderGetNext(struct ArrowArrayStream* stream, struct ArrowArray* out) {
  struct OdbcReader* r = (struct OdbcReader*)stream->private_data;
  if (!r) return EINVAL;
  if (r->error.release) {
    r->error.release(&r->error);
    memset(&r->error, 0, sizeof(r->error));
  }
  out->release = NULL;
  if (r->done) return 0;
  AdbcStatusCode status = ReaderNextBatch(r, out, &r->error);
  if (status != ADBC_STATUS_OK) {
    snprintf(r->error_message, sizeof(r->error_message), "%s",
             r->error.message ? r->error.message : "unknown error");
    r->done = true;
    return InternalAdbcStatusCodeToErrno(status);
  }
  return 0;
}

static const char* ReaderGetLastError(struct ArrowArrayStream* stream) {
  struct OdbcReader* r = (struct OdbcReader*)stream->private_data;
  if (!r) return NULL;
  return r->error_message[0] ? r->error_message : NULL;
}

AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error) {
  struct OdbcReader* r = calloc(1, sizeof(struct OdbcReader));
  if (!r) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  r->ref = ref;
  ref->refcount++;
  r->opts = *opts;
  if (r->opts.batch_size <= 0) r->opts.batch_size = ADBC_ODBC_DEFAULT_BATCH_SIZE;
  ArrowBufferInit(&r->scratch);

  AdbcStatusCode status = DescribeColumns(ref->hstmt, &r->opts, &r->cols, &r->ncols, error);
  if (status == ADBC_STATUS_OK) status = BuildSchema(r->cols, r->ncols, &r->schema, error);
  if (status != ADBC_STATUS_OK) {
    struct ArrowArrayStream tmp = {0};
    tmp.private_data = r;
    tmp.release = ReaderRelease;
    ReaderRelease(&tmp);
    return status;
  }
  out->private_data = r;
  out->get_schema = ReaderGetSchema;
  out->get_next = ReaderGetNext;
  out->get_last_error = ReaderGetLastError;
  out->release = ReaderRelease;
  return ADBC_STATUS_OK;
}
