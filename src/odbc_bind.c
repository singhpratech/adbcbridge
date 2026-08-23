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

// Parameter binding (Arrow -> SQLBindParameter) and bulk ingest.

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// Per-parameter scratch storage for one row.
struct ParamSlot {
  SQLSMALLINT c_type;
  SQLSMALLINT sql_type;
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLLEN indicator;
  // What SQLBindParameter is actually pointed at: `indicator` re-encoded for a driver
  // that reads StrLen_or_IndPtr as a 32-bit SQLLEN (see OdbcIndicatorSet).
  SQLLEN bound_indicator;
  SQLLEN buffer_length;
  const void* data;  // points into fixed or into Arrow buffers
  struct ArrowBuffer wbuf;  // UTF-16 conversion of string parameters
  // Value schema of a dictionary-encoded column, parsed once for the whole bind.
  struct ArrowSchemaView dict_sv;
  bool dict_ready;
  union {
    unsigned char bit;
    SQLINTEGER i32;
    SQLBIGINT i64;
    SQLUBIGINT u64;
    SQLDOUBLE f64;
    DATE_STRUCT date;
    TIME_STRUCT time;
    TIMESTAMP_STRUCT ts;
    char text[64];
  } fixed;
};

static void CivilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (*m <= 2));
}

// UTF-8 -> UTF-16 into `o`, which must hold at least Utf16Units(s, n) + 1 units.
// NUL-terminates and returns the number of units written.
static int64_t Utf8ToUtf16Into(SQLWCHAR* o, const char* s, int64_t n) {
  int64_t k = 0;
  for (int64_t i = 0; i < n;) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
    else { cp = 0xFFFD; len = 1; }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      o[k++] = (SQLWCHAR)(0xD800 + (cp >> 10));
      o[k++] = (SQLWCHAR)(0xDC00 + (cp & 0x3FF));
    } else {
      o[k++] = (SQLWCHAR)cp;
    }
  }
  o[k] = 0;
  return k;
}

// How many UTF-16 units Utf8ToUtf16Into would write for this UTF-8 string.
static int64_t Utf16Units(const char* s, int64_t n) {
  int64_t k = 0;
  for (int64_t i = 0; i < n;) {
    unsigned char c = (unsigned char)s[i];
    int len;
    bool pair = false;
    if (c < 0x80) { len = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { len = 4; pair = true; }
    else { len = 1; }
    i += len;
    k += pair ? 2 : 1;
  }
  return k;
}

// UTF-8 -> UTF-16 (SQLWCHAR units) into buf; returns number of units.
static ArrowErrorCode Utf8ToUtf16(struct ArrowBuffer* buf, const char* s, int64_t n, int64_t* units) {
  buf->size_bytes = 0;
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buf, (n + 1) * (int64_t)sizeof(SQLWCHAR)));
  int64_t k = Utf8ToUtf16Into((SQLWCHAR*)buf->data, s, n);
  buf->size_bytes = k * (int64_t)sizeof(SQLWCHAR);
  *units = k;
  return NANOARROW_OK;
}

// Fractional-second digits an Arrow time/timestamp unit needs.  Nanoseconds are capped
// at 7 because SQL Server's TIME and DATETIME2 top out at scale 7 and reject anything
// larger with HY104; the same cap keeps parameter scale and created column scale equal.
static int FractionalDigits(enum ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND: return 0;
    case NANOARROW_TIME_UNIT_MILLI: return 3;
    case NANOARROW_TIME_UNIT_MICRO: return 6;
    default: return 7;
  }
}

// Column size / fractional digits for a timestamp parameter of `unit`.
static void TimestampParamSize(enum ArrowTimeUnit unit, SQLULEN* column_size,
                               SQLSMALLINT* decimal_digits) {
  const int digits = FractionalDigits(unit);
  *decimal_digits = (SQLSMALLINT)digits;
  *column_size = (SQLULEN)(digits ? 20 + digits : 19);
}

// Ticks per second of an Arrow date/time unit.
static int64_t TicksPerSecond(enum ArrowTimeUnit unit) {
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND: return 1;
    case NANOARROW_TIME_UNIT_MILLI: return 1000;
    case NANOARROW_TIME_UNIT_MICRO: return 1000000;
    default: return 1000000000;
  }
}

// Split an Arrow time-of-day value in `unit` into whole seconds since midnight
// and the leftover ticks.  Values outside a day wrap, so a corrupt or unread
// (null) slot can never produce an out-of-range hour.
static void TimeOfDayFromArrow(int64_t v, enum ArrowTimeUnit unit, int64_t* secs,
                               int64_t* frac) {
  const int64_t per_sec = TicksPerSecond(unit);
  int64_t s = v / per_sec, f = v % per_sec;
  if (f < 0) { f += per_sec; s -= 1; }
  s %= 86400;
  if (s < 0) s += 86400;
  *secs = s;
  *frac = f;
}

// Convert an Arrow time32[s]/time32[ms] value to an ODBC TIME_STRUCT (which has
// no fractional field, so sub-second ticks are dropped).
static void TimeStructFromArrow(int64_t v, enum ArrowTimeUnit unit, TIME_STRUCT* t) {
  int64_t secs, frac;
  TimeOfDayFromArrow(v, unit, &secs, &frac);
  t->hour = (SQLUSMALLINT)(secs / 3600);
  t->minute = (SQLUSMALLINT)((secs % 3600) / 60);
  t->second = (SQLUSMALLINT)(secs % 60);
}

// Render an Arrow time value in `unit` as "HH:MM:SS.ffffff"; returns its length.
// TIME_STRUCT cannot carry fractional seconds, so sub-second times are bound as
// this text instead.
static int TimeTextFromArrow(int64_t v, enum ArrowTimeUnit unit, char* out, size_t out_size) {
  int64_t secs, frac;
  TimeOfDayFromArrow(v, unit, &secs, &frac);
  const int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
  const int digits = FractionalDigits(unit);
  if (digits == 0) return snprintf(out, out_size, "%02d:%02d:%02d", hh, mm, ss);
  int64_t scale = 1;
  for (int i = 0; i < digits; i++) scale *= 10;
  // frac < 10^9 and scale <= 10^7, so the product stays well inside int64.
  const long long f = (long long)(frac * scale / TicksPerSecond(unit));
  return snprintf(out, out_size, "%02d:%02d:%02d.%0*lld", hh, mm, ss, digits, f);
}

// Column size (characters) of the textual form TimeTextFromArrow produces.
static SQLULEN TimeParamColumnSize(enum ArrowTimeUnit unit) {
  const int digits = FractionalDigits(unit);
  return (SQLULEN)(digits ? 9 + digits : 8);
}

// Convert an Arrow timestamp value in `unit` to an ODBC TIMESTAMP_STRUCT.
static void TimestampFromArrow(int64_t v, enum ArrowTimeUnit unit, TIMESTAMP_STRUCT* ts) {
  int64_t per_sec = 1, frac_mul = 1;
  switch (unit) {
    case NANOARROW_TIME_UNIT_SECOND: per_sec = 1; frac_mul = 1000000000; break;
    case NANOARROW_TIME_UNIT_MILLI: per_sec = 1000; frac_mul = 1000000; break;
    case NANOARROW_TIME_UNIT_MICRO: per_sec = 1000000; frac_mul = 1000; break;
    case NANOARROW_TIME_UNIT_NANO: per_sec = 1000000000; frac_mul = 1; break;
  }
  int64_t secs = v / per_sec, frac = v % per_sec;
  if (frac < 0) { frac += per_sec; secs -= 1; }
  int64_t days = secs / 86400, sod = secs % 86400;
  if (sod < 0) { sod += 86400; days -= 1; }
  int y; unsigned m, d;
  CivilFromDays(days, &y, &m, &d);
  ts->year = (SQLSMALLINT)y; ts->month = (SQLUSMALLINT)m; ts->day = (SQLUSMALLINT)d;
  ts->hour = (SQLUSMALLINT)(sod / 3600); ts->minute = (SQLUSMALLINT)((sod % 3600) / 60);
  ts->second = (SQLUSMALLINT)(sod % 60); ts->fraction = (SQLUINTEGER)(frac * frac_mul);
}

static AdbcStatusCode SlotFromArrowValue(struct ParamSlot* p, const struct ArrowSchemaView* sv,
                                         const struct ArrowArrayView* av, int64_t row,
                                         bool is_null, const struct OdbcReaderOptions* opts,
                                         struct AdbcError* error) {
  if (is_null) {
    p->indicator = SQL_NULL_DATA;
    p->data = &p->fixed;
    p->buffer_length = 0;
  } else {
    p->indicator = 0;
  }
  if (p->indicator == SQL_NULL_DATA && opts->null_param_as_varchar) {
    p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 1;
    p->decimal_digits = 0; p->data = p->fixed.text; p->buffer_length = 0;
    return ADBC_STATUS_OK;
  }
  switch (sv->type) {
    case NANOARROW_TYPE_NA:
      // Untyped NULL (e.g. Python None): send as a NULL varchar.
      p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 1;
      p->indicator = SQL_NULL_DATA;
      p->data = p->fixed.text; p->buffer_length = 0;
      break;
    case NANOARROW_TYPE_BOOL:
      if (opts->bool_param_as_varchar) {
        // QuestDB reads a boolean parameter only from the words "true"/"false".
        const char* word = ArrowArrayViewGetIntUnsafe(av, row) ? "true" : "false";
        const int n = snprintf(p->fixed.text, sizeof(p->fixed.text), "%s", word);
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 5;
        p->data = p->fixed.text; p->buffer_length = n + 1;
        if (p->indicator != SQL_NULL_DATA) p->indicator = n;
      } else if (opts->bool_param_as_int) {
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_INTEGER;
        p->fixed.i64 = ArrowArrayViewGetIntUnsafe(av, row) ? 1 : 0;
        p->data = &p->fixed.i64; p->buffer_length = sizeof(SQLBIGINT);
      } else {
        p->c_type = SQL_C_BIT; p->sql_type = SQL_BIT;
        p->fixed.bit = (unsigned char)ArrowArrayViewGetIntUnsafe(av, row);
        p->data = &p->fixed.bit; p->buffer_length = 1;
      }
      break;
    case NANOARROW_TYPE_INT8: case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32: case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8: case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32: case NANOARROW_TYPE_UINT64: {
      bool is_unsigned = sv->type == NANOARROW_TYPE_UINT8 || sv->type == NANOARROW_TYPE_UINT16 ||
                         sv->type == NANOARROW_TYPE_UINT32 || sv->type == NANOARROW_TYPE_UINT64;
      uint64_t u = 0; int64_t v = 0;
      if (p->indicator != SQL_NULL_DATA) {
        if (is_unsigned) u = ArrowArrayViewGetUIntUnsafe(av, row);
        else v = ArrowArrayViewGetIntUnsafe(av, row);
      }
      bool fits32 = is_unsigned ? (u <= INT32_MAX) : (v >= INT32_MIN && v <= INT32_MAX);
      if (fits32) {
        // SQL_C_SLONG is the most universally supported integer binding.
        p->c_type = SQL_C_SLONG; p->sql_type = SQL_INTEGER;
        p->fixed.i32 = is_unsigned ? (SQLINTEGER)u : (SQLINTEGER)v;
        p->data = &p->fixed.i32; p->buffer_length = sizeof(SQLINTEGER);
      } else if (opts->bigint_param_as_string) {
        int n = is_unsigned ? snprintf(p->fixed.text, sizeof(p->fixed.text), "%llu", (unsigned long long)u)
                            : snprintf(p->fixed.text, sizeof(p->fixed.text), "%lld", (long long)v);
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_NUMERIC; p->column_size = 20; p->decimal_digits = 0;
        p->data = p->fixed.text; p->buffer_length = n + 1;
        if (p->indicator != SQL_NULL_DATA) p->indicator = n;
      } else if (is_unsigned) {
        p->c_type = SQL_C_UBIGINT; p->sql_type = SQL_BIGINT;
        p->fixed.u64 = u; p->data = &p->fixed.u64; p->buffer_length = sizeof(SQLUBIGINT);
      } else {
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_BIGINT;
        p->fixed.i64 = v; p->data = &p->fixed.i64; p->buffer_length = sizeof(SQLBIGINT);
      }
      break;
    }
    case NANOARROW_TYPE_HALF_FLOAT: case NANOARROW_TYPE_FLOAT: case NANOARROW_TYPE_DOUBLE:
      p->c_type = SQL_C_DOUBLE; p->sql_type = SQL_DOUBLE;
      p->fixed.f64 = ArrowArrayViewGetDoubleUnsafe(av, row);
      p->data = &p->fixed.f64; p->buffer_length = sizeof(SQLDOUBLE);
      break;
    case NANOARROW_TYPE_STRING: case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW: {
      int64_t units = 0;
      if (opts->wchar_as_utf8) {  // see OdbcReaderOptions::wchar_as_utf8
        struct ArrowStringView s = {NULL, 0};
        p->c_type = SQL_C_CHAR;
        if (p->indicator != SQL_NULL_DATA) {
          s = ArrowArrayViewGetStringUnsafe(av, row);
          p->data = (void*)s.data;
          p->buffer_length = s.size_bytes;
          p->indicator = s.size_bytes;
        }
        p->sql_type = s.size_bytes > 4000 ? SQL_LONGVARCHAR : SQL_VARCHAR;
        p->column_size = (SQLULEN)(s.size_bytes > 0 ? s.size_bytes : 1);
        break;
      }
      if (p->indicator != SQL_NULL_DATA) {
        struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(av, row);
        CHECK_NA(INTERNAL, Utf8ToUtf16(&p->wbuf, s.data, s.size_bytes, &units), error);
      }
      p->c_type = SQL_C_WCHAR;
      p->sql_type = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
      p->column_size = (SQLULEN)(units > 0 ? units : 1);
      p->data = p->wbuf.data; p->buffer_length = units * (int64_t)sizeof(SQLWCHAR);
      if (p->indicator != SQL_NULL_DATA) p->indicator = p->buffer_length;
      break;
    }
    case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY: case NANOARROW_TYPE_BINARY_VIEW: {
      struct ArrowBufferView b = {{NULL}, 0};
      if (p->indicator != SQL_NULL_DATA) b = ArrowArrayViewGetBytesUnsafe(av, row);
      p->c_type = SQL_C_BINARY;
      p->sql_type = b.size_bytes > 4000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
      p->column_size = (SQLULEN)(b.size_bytes > 0 ? b.size_bytes : 1);
      p->data = b.data.as_uint8; p->buffer_length = b.size_bytes;
      if (p->indicator != SQL_NULL_DATA) p->indicator = b.size_bytes;
      break;
    }
    case NANOARROW_TYPE_DATE32: {
      int y; unsigned m, d;
      CivilFromDays(ArrowArrayViewGetIntUnsafe(av, row), &y, &m, &d);
      p->fixed.date.year = (SQLSMALLINT)y; p->fixed.date.month = (SQLUSMALLINT)m;
      p->fixed.date.day = (SQLUSMALLINT)d;
      p->c_type = SQL_C_TYPE_DATE; p->sql_type = SQL_TYPE_DATE;
      p->data = &p->fixed.date; p->buffer_length = sizeof(DATE_STRUCT);
      break;
    }
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64: {
      const int64_t v = ArrowArrayViewGetIntUnsafe(av, row);
      if (sv->time_unit == NANOARROW_TIME_UNIT_SECOND) {
        TimeStructFromArrow(v, sv->time_unit, &p->fixed.time);
        p->c_type = SQL_C_TYPE_TIME; p->sql_type = SQL_TYPE_TIME;
        p->column_size = TimeParamColumnSize(sv->time_unit); p->decimal_digits = 0;
        p->data = &p->fixed.time; p->buffer_length = sizeof(TIME_STRUCT);
      } else {
        // TIME_STRUCT has no fractional field, so sub-second times go across as
        // "HH:MM:SS.ffffff" text.  It is bound as SQL_VARCHAR and left to the
        // server's own literal parsing rather than as SQL_TYPE_TIME: DuckDB's
        // ODBC driver aborts the process on an SQL_C_CHAR -> SQL_TYPE_TIME
        // parameter ("Invalid unicode ... in value construction") and MariaDB's
        // rejects it with 22008 "Datetime field overflow", while every driver
        // tested accepts the same string as a VARCHAR parameter.
        int n = TimeTextFromArrow(v, sv->time_unit, p->fixed.text, sizeof(p->fixed.text));
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR;
        p->column_size = TimeParamColumnSize(sv->time_unit); p->decimal_digits = 0;
        p->data = p->fixed.text; p->buffer_length = n + 1;
        if (p->indicator != SQL_NULL_DATA) p->indicator = n;
      }
      break;
    }
    case NANOARROW_TYPE_TIMESTAMP: {
      TimestampFromArrow(ArrowArrayViewGetIntUnsafe(av, row), sv->time_unit, &p->fixed.ts);
      p->c_type = SQL_C_TYPE_TIMESTAMP; p->sql_type = SQL_TYPE_TIMESTAMP;
      TimestampParamSize(sv->time_unit, &p->column_size, &p->decimal_digits);
      p->data = &p->fixed.ts; p->buffer_length = sizeof(TIMESTAMP_STRUCT);
      break;
    }
    case NANOARROW_TYPE_DECIMAL128: case NANOARROW_TYPE_DECIMAL256: {
      if (p->indicator != SQL_NULL_DATA) {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, sv->type == NANOARROW_TYPE_DECIMAL128 ? 128 : 256,
                         sv->decimal_precision, sv->decimal_scale);
        ArrowArrayViewGetDecimalUnsafe(av, row, &dec);
        struct ArrowBuffer buf;
        ArrowBufferInit(&buf);
        CHECK_NA(INTERNAL, ArrowDecimalAppendStringToBuffer(&dec, &buf), error);
        size_t n = (size_t)buf.size_bytes < sizeof(p->fixed.text) - 1 ? (size_t)buf.size_bytes
                                                                        : sizeof(p->fixed.text) - 1;
        memcpy(p->fixed.text, buf.data, n);
        p->fixed.text[n] = '\0';
        ArrowBufferReset(&buf);
        p->indicator = (SQLLEN)n;
      }
      p->c_type = SQL_C_CHAR;
      if (opts->decimal_param_as_varchar) {
        p->sql_type = SQL_VARCHAR; p->column_size = sizeof(p->fixed.text); p->decimal_digits = 0;
      } else {
        p->sql_type = SQL_DECIMAL;
        p->column_size = (SQLULEN)sv->decimal_precision; p->decimal_digits = (SQLSMALLINT)sv->decimal_scale;
      }
      p->data = p->fixed.text; p->buffer_length = (SQLLEN)sizeof(p->fixed.text);
      break;
    }
    default:
      InternalAdbcSetError(error, "Unsupported Arrow type for parameter binding: %s",
                           ArrowTypeString(sv->type));
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  return ADBC_STATUS_OK;
}

// Bind one parameter from row `row` of an Arrow column.  Dictionary-encoded
// columns are decoded here: the index is resolved against the dictionary and
// the value it points at is bound in its place, so a pandas categorical binds
// exactly like the plain column it encodes.
static AdbcStatusCode SlotFromArrow(struct ParamSlot* p, const struct ArrowSchemaView* sv,
                                    const struct ArrowArrayView* av, int64_t row,
                                    const struct OdbcReaderOptions* opts,
                                    struct AdbcError* error) {
  bool is_null = ArrowArrayViewIsNull(av, row);
  if (sv->type != NANOARROW_TYPE_DICTIONARY) {
    return SlotFromArrowValue(p, sv, av, row, is_null, opts, error);
  }
  if (!sv->schema || !sv->schema->dictionary || !av->dictionary) {
    InternalAdbcSetError(error, "Dictionary-encoded parameter has no dictionary");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (!p->dict_ready) {
    struct ArrowError na_error;
    CHECK_NA_DETAIL(INTERNAL, ArrowSchemaViewInit(&p->dict_sv, sv->schema->dictionary, &na_error),
                    &na_error, error);
    p->dict_ready = true;
  }
  const struct ArrowArrayView* values = av->dictionary;
  int64_t idx = is_null ? 0 : ArrowArrayViewGetIntUnsafe(av, row);
  if (idx < 0 || idx >= values->length) {
    if (!is_null) {
      InternalAdbcSetError(error, "Dictionary index %lld is out of range (dictionary has %lld values)",
                           (long long)idx, (long long)values->length);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    // A null index and nothing to look it up in: bind an untyped NULL.
    p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 1;
    p->decimal_digits = 0; p->indicator = SQL_NULL_DATA;
    p->data = p->fixed.text; p->buffer_length = 0;
    return ADBC_STATUS_OK;
  }
  if (!is_null) is_null = ArrowArrayViewIsNull(values, idx);
  return SlotFromArrowValue(p, &p->dict_sv, values, idx, is_null, opts, error);
}

// ---------------------------------------------------------------------------
// Column-wise array parameter binding
//
// Rather than executing the statement once per row, bind whole Arrow columns as
// ODBC parameter arrays (SQL_ATTR_PARAM_BIND_TYPE = SQL_PARAM_BIND_BY_COLUMN
// together with SQL_ATTR_PARAMSET_SIZE) so that a single SQLExecute applies a
// whole Arrow batch.  Fixed-width Arrow columns are bound directly on top of
// the Arrow data buffers (no copy); variable-length columns are staged into a
// per-column buffer sized to the batch's longest value.  Drivers that do not
// honour the parameter-array attributes are detected and the caller falls back
// to the row-at-a-time path automatically.

// Values longer than this are not worth staging; fall back for that batch.
#define ARRAY_BIND_MAX_VARLEN 32768
// Bound the staging allocation by splitting a batch into parameter-set chunks.
#define ARRAY_BIND_MAX_CHUNK_BYTES (16 * 1024 * 1024)
#define ARRAY_BIND_MAX_CHUNK_ROWS 65536
#define ARRAY_BIND_DECIMAL_CHARS 64
// "HH:MM:SS.fffffff" plus a NUL, rounded up.
#define ARRAY_BIND_TIME_CHARS 24
// Room for a 64-bit integer in decimal text plus sign and NUL.
#define ARRAY_BIND_INT_CHARS 24
// "true"/"false" plus a NUL (bool_param_as_varchar).
#define ARRAY_BIND_BOOL_CHARS 6

struct ArrayParam {
  SQLSMALLINT c_type;
  SQLSMALLINT sql_type;
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLLEN elem_size;       // bytes per parameter set
  const uint8_t* direct;  // Arrow data buffer, when binding without a copy
  uint8_t* buffer;        // staging buffer (elem_size * chunk rows), else NULL
  SQLLEN* indicators;     // NULL when fixed-width and free of nulls
  bool needs_buffer;
  bool needs_indicators;
  // Dictionary-encoded column: the plan describes the dictionary's value type
  // and ArrayParamFill resolves each index before staging the value.
  bool dictionary;
  struct ArrowSchemaView dict_sv;
};

// Longest value in a variable-length column, measured in the unit it will be
// bound in -- bytes for binary and for narrow strings, UTF-16 units for wide
// ones -- or -1 if it exceeds the staging cap.
static int64_t ArrayParamVarLenMax(const struct ArrowArrayView* av, bool binary, bool wide,
                                   int64_t nrows) {
  int64_t max = 0;
  for (int64_t i = 0; i < nrows; i++) {
    if (ArrowArrayViewIsNull(av, i)) continue;
    int64_t len;
    if (binary) {
      len = ArrowArrayViewGetBytesUnsafe(av, i).size_bytes;
    } else {
      struct ArrowStringView v = ArrowArrayViewGetStringUnsafe(av, i);
      if (v.size_bytes > ARRAY_BIND_MAX_VARLEN) return -1;
      len = wide ? Utf16Units(v.data, v.size_bytes) : v.size_bytes;
    }
    if (len > max) {
      max = len;
      if (max > ARRAY_BIND_MAX_VARLEN) return -1;
    }
  }
  return max;
}

// Whether every non-null value of an integer column fits in SQLINTEGER.  The
// row-at-a-time path prefers SQL_C_SLONG whenever it can because it is the most
// widely supported integer binding; matching that choice keeps the two paths
// sending byte-identical parameters.
static bool ArrayParamIntFits32(const struct ArrowArrayView* av, bool is_unsigned, int64_t nrows) {
  for (int64_t i = 0; i < nrows; i++) {
    if (ArrowArrayViewIsNull(av, i)) continue;
    if (is_unsigned) {
      if (ArrowArrayViewGetUIntUnsafe(av, i) > (uint64_t)INT32_MAX) return false;
    } else {
      int64_t v = ArrowArrayViewGetIntUnsafe(av, i);
      if (v < INT32_MIN || v > INT32_MAX) return false;
    }
  }
  return true;
}

static bool ArrowTypeIsUnsignedInt(enum ArrowType type) {
  return type == NANOARROW_TYPE_UINT8 || type == NANOARROW_TYPE_UINT16 ||
         type == NANOARROW_TYPE_UINT32 || type == NANOARROW_TYPE_UINT64;
}

// Decide how one column will be bound.  Clears *supported (without raising an
// error) when this batch cannot use array binding for this column, in which
// case the caller replays it row-at-a-time.  The ODBC C/SQL type chosen here is
// the one SlotFromArrowValue would choose for the same values, driver quirks
// included, so the two paths send identical data.
static void ArrayParamPlan(struct ArrayParam* p, const struct ArrowSchemaView* sv,
                           const struct ArrowArrayView* av, int64_t nrows,
                           const struct OdbcReaderOptions* opts, bool* supported) {
  memset(p, 0, sizeof(*p));
  if (sv->type == NANOARROW_TYPE_DICTIONARY) {
    // Plan against the dictionary's values; ArrayParamFill decodes per row.
    struct ArrowSchemaView dsv;
    if (!sv->schema || !sv->schema->dictionary || !av->dictionary ||
        ArrowSchemaViewInit(&dsv, sv->schema->dictionary, NULL) != NANOARROW_OK ||
        dsv.type == NANOARROW_TYPE_DICTIONARY) {
      *supported = false;
      return;
    }
    const bool index_nulls = ArrowArrayViewComputeNullCount(av) > 0;
    ArrayParamPlan(p, &dsv, av->dictionary, av->dictionary->length, opts, supported);
    if (!*supported) return;
    p->dictionary = true;
    p->dict_sv = dsv;
    p->direct = NULL;       // indices have to be decoded, so never bind in place
    p->needs_buffer = true;
    if (index_nulls) p->needs_indicators = true;
    return;
  }
  const uint8_t* data = av->buffer_views[1].data.as_uint8;
  const bool has_nulls = ArrowArrayViewComputeNullCount(av) > 0;
  if (has_nulls && opts->null_param_as_varchar) {
    // The driver only encodes NULL as a NULL VARCHAR whatever the column's type
    // (clickhouse-odbc); that is a per-value switch only the row path can make.
    *supported = false;
    return;
  }
  switch (sv->type) {
    // Integers wider than the value they hold are narrowed into a staging
    // buffer: SQL_C_SLONG is the most widely supported integer binding, and it
    // is what the row path picks whenever every value fits.
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64: {
      const bool is_unsigned = ArrowTypeIsUnsignedInt(sv->type);
      if (ArrayParamIntFits32(av, is_unsigned, nrows)) {
        p->c_type = SQL_C_SLONG; p->sql_type = SQL_INTEGER; p->elem_size = sizeof(SQLINTEGER);
        p->needs_buffer = sv->type != NANOARROW_TYPE_INT32;
      } else if (opts->bigint_param_as_string) {
        // Oracle's driver rejects SQL_C_SBIGINT; send wide integers as numeric text.
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_NUMERIC;
        p->column_size = 20; p->decimal_digits = 0;
        p->elem_size = ARRAY_BIND_INT_CHARS;
        p->needs_buffer = true; p->needs_indicators = true;
      } else if (is_unsigned) {
        p->c_type = SQL_C_UBIGINT; p->sql_type = SQL_BIGINT; p->elem_size = sizeof(SQLUBIGINT);
        p->needs_buffer = sv->type != NANOARROW_TYPE_UINT64;
      } else {
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_BIGINT; p->elem_size = sizeof(SQLBIGINT);
        p->needs_buffer = sv->type != NANOARROW_TYPE_INT64;
      }
      break;
    }
    case NANOARROW_TYPE_FLOAT:
      p->c_type = SQL_C_FLOAT; p->sql_type = SQL_REAL; p->elem_size = 4; break;
    case NANOARROW_TYPE_DOUBLE:
      p->c_type = SQL_C_DOUBLE; p->sql_type = SQL_DOUBLE; p->elem_size = 8; break;
    case NANOARROW_TYPE_BOOL:  // Arrow stores bits; ODBC wants one value each
      if (opts->bool_param_as_varchar) {  // QuestDB parses only "true"/"false"
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 5;
        p->elem_size = ARRAY_BIND_BOOL_CHARS;
        p->needs_indicators = true;
      } else if (opts->bool_param_as_int) {  // DuckDB rejects SQL_BIT parameters
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_INTEGER; p->elem_size = sizeof(SQLBIGINT);
      } else {
        p->c_type = SQL_C_BIT; p->sql_type = SQL_BIT; p->elem_size = 1;
      }
      p->needs_buffer = true; break;
    case NANOARROW_TYPE_HALF_FLOAT:
      p->c_type = SQL_C_DOUBLE; p->sql_type = SQL_DOUBLE; p->elem_size = sizeof(SQLDOUBLE);
      p->needs_buffer = true; break;
    case NANOARROW_TYPE_DATE32:
      p->c_type = SQL_C_TYPE_DATE; p->sql_type = SQL_TYPE_DATE;
      p->elem_size = sizeof(DATE_STRUCT); p->needs_buffer = true; break;
    case NANOARROW_TYPE_TIMESTAMP:
      p->c_type = SQL_C_TYPE_TIMESTAMP; p->sql_type = SQL_TYPE_TIMESTAMP;
      p->elem_size = sizeof(TIMESTAMP_STRUCT);
      TimestampParamSize(sv->time_unit, &p->column_size, &p->decimal_digits);
      p->needs_buffer = true; break;
    case NANOARROW_TYPE_TIME32:
    case NANOARROW_TYPE_TIME64:
      p->column_size = TimeParamColumnSize(sv->time_unit);
      if (sv->time_unit == NANOARROW_TIME_UNIT_SECOND) {
        p->c_type = SQL_C_TYPE_TIME; p->sql_type = SQL_TYPE_TIME;
        p->elem_size = sizeof(TIME_STRUCT);
      } else {
        // Sub-second times go across as VARCHAR text; see SlotFromArrowValue.
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR;
        p->elem_size = ARRAY_BIND_TIME_CHARS;
        p->needs_indicators = true;
      }
      p->needs_buffer = true; break;
    case NANOARROW_TYPE_DECIMAL128:
    case NANOARROW_TYPE_DECIMAL256:
      p->c_type = SQL_C_CHAR;
      p->elem_size = ARRAY_BIND_DECIMAL_CHARS;
      if (opts->decimal_param_as_varchar) {  // DuckDB mis-scales SQL_DECIMAL parameters
        p->sql_type = SQL_VARCHAR; p->column_size = ARRAY_BIND_DECIMAL_CHARS;
        p->decimal_digits = 0;
      } else {
        p->sql_type = SQL_DECIMAL;
        p->column_size = (SQLULEN)sv->decimal_precision;
        p->decimal_digits = (SQLSMALLINT)sv->decimal_scale;
      }
      p->needs_buffer = true; p->needs_indicators = true; break;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW: {
      const bool binary = sv->type != NANOARROW_TYPE_STRING &&
                          sv->type != NANOARROW_TYPE_LARGE_STRING &&
                          sv->type != NANOARROW_TYPE_STRING_VIEW;
      // Strings go out as UTF-16, exactly as the row path sends them: a
      // SQL_C_CHAR array is transcoded from the driver's narrow charset and
      // mangles anything outside it (SQL Server stored "hello ?" for an emoji).
      // Drivers whose SQLWCHAR is not UTF-16 keep the narrow, UTF-8 path.
      const bool wide = !binary && !opts->wchar_as_utf8;
      int64_t max = ArrayParamVarLenMax(av, binary, wide, nrows);
      if (max < 0) {  // too wide to stage: this batch goes row-at-a-time
        *supported = false;
        return;
      }
      if (max < 1) max = 1;
      p->column_size = (SQLULEN)max;
      if (binary) {
        p->c_type = SQL_C_BINARY;
        p->sql_type = max > 4000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
        p->elem_size = max;
      } else if (wide) {
        p->c_type = SQL_C_WCHAR;
        p->sql_type = max > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
        p->elem_size = (max + 1) * (int64_t)sizeof(SQLWCHAR);  // room for a NUL
      } else {
        p->c_type = SQL_C_CHAR;
        p->sql_type = max > 4000 ? SQL_LONGVARCHAR : SQL_VARCHAR;
        p->elem_size = max + 1;  // room for a NUL terminator
      }
      p->needs_buffer = true; p->needs_indicators = true;
      break;
    }
    default:
      // Leave the diagnostic to the row-at-a-time path, which raises it with
      // the offending row in hand.
      *supported = false;
      return;
  }
  if (has_nulls) p->needs_indicators = true;
  if (!p->needs_buffer) {
    if (!data) {  // no data buffer to point at (e.g. an all-null array)
      *supported = false;
      return;
    }
    p->direct = data + (size_t)av->offset * (size_t)p->elem_size;
  }
}

// Stage rows [start, start + n) of one column into its buffer/indicator arrays.
static AdbcStatusCode ArrayParamFill(struct ArrayParam* p, const struct ArrowSchemaView* sv,
                                     const struct ArrowArrayView* av, int64_t start, int64_t n,
                                     bool q, struct AdbcError* error) {
  // `q`: the driver reads indicator arrays as int32[] with stride 4 (see OdbcIndicatorSet).
  SQLLEN* ind = p->indicators;
  if (!p->buffer) {
    if (ind) {
      for (int64_t i = 0; i < n; i++) {
        OdbcIndicatorSet(ind, (size_t)i, ArrowArrayViewIsNull(av, start + i) ? SQL_NULL_DATA : 0, q);
      }
    }
    return ADBC_STATUS_OK;
  }
  const size_t stride = (size_t)p->elem_size;
  // A dictionary column stages the dictionary's values, looked up per row.
  const struct ArrowSchemaView* vsv = p->dictionary ? &p->dict_sv : sv;
  const struct ArrowArrayView* values = p->dictionary ? av->dictionary : av;
  for (int64_t i = 0; i < n; i++) {
    int64_t row = start + i;
    uint8_t* slot = p->buffer + (size_t)i * stride;
    bool is_null = ArrowArrayViewIsNull(av, row);
    if (p->dictionary && !is_null) {
      const int64_t idx = ArrowArrayViewGetIntUnsafe(av, row);
      if (idx < 0 || idx >= values->length) {
        InternalAdbcSetError(error,
                             "Dictionary index %lld is out of range (dictionary has %lld values)",
                             (long long)idx, (long long)values->length);
        return ADBC_STATUS_INVALID_ARGUMENT;
      }
      row = idx;
      is_null = ArrowArrayViewIsNull(values, idx);
    }
    if (is_null) {
      if (ind) OdbcIndicatorSet(ind, (size_t)i, SQL_NULL_DATA, q);
      memset(slot, 0, stride);
      continue;
    }
    if (ind) OdbcIndicatorSet(ind, (size_t)i, 0, q);
    switch (vsv->type) {
      case NANOARROW_TYPE_BOOL: {
        const int64_t b = ArrowArrayViewGetIntUnsafe(values, row) != 0;
        if (p->c_type == SQL_C_BIT) {
          *slot = (uint8_t)b;
        } else if (p->c_type == SQL_C_CHAR) {  // bool_param_as_varchar
          const char* word = b ? "true" : "false";
          const size_t len = strlen(word);
          memcpy(slot, word, len + 1);
          if (ind) OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)len, q);
        } else {  // bool_param_as_int
          SQLBIGINT v = (SQLBIGINT)b;
          memcpy(slot, &v, sizeof(v));
        }
        break;
      }
      case NANOARROW_TYPE_FLOAT: {
        SQLREAL v = (SQLREAL)ArrowArrayViewGetDoubleUnsafe(values, row);
        memcpy(slot, &v, sizeof(v));
        break;
      }
      case NANOARROW_TYPE_DOUBLE: {
        SQLDOUBLE v = (SQLDOUBLE)ArrowArrayViewGetDoubleUnsafe(values, row);
        memcpy(slot, &v, sizeof(v));
        break;
      }
      // Every integer width lands here; the plan chose the C type from the
      // values, and widths that the plan binds in place still have to be staged
      // when they arrive dictionary-encoded.
      case NANOARROW_TYPE_INT8:
      case NANOARROW_TYPE_INT16:
      case NANOARROW_TYPE_INT32:
      case NANOARROW_TYPE_INT64:
      case NANOARROW_TYPE_UINT8:
      case NANOARROW_TYPE_UINT16:
      case NANOARROW_TYPE_UINT32:
      case NANOARROW_TYPE_UINT64: {
        const bool is_unsigned = ArrowTypeIsUnsignedInt(vsv->type);
        uint64_t u = 0;
        int64_t v = 0;
        if (is_unsigned) u = ArrowArrayViewGetUIntUnsafe(values, row);
        else v = ArrowArrayViewGetIntUnsafe(values, row);
        if (p->c_type == SQL_C_SLONG) {
          SQLINTEGER x = is_unsigned ? (SQLINTEGER)u : (SQLINTEGER)v;
          memcpy(slot, &x, sizeof(x));
        } else if (p->c_type == SQL_C_UBIGINT) {
          SQLUBIGINT x = is_unsigned ? (SQLUBIGINT)u : (SQLUBIGINT)v;
          memcpy(slot, &x, sizeof(x));
        } else if (p->c_type == SQL_C_SBIGINT) {
          SQLBIGINT x = is_unsigned ? (SQLBIGINT)u : (SQLBIGINT)v;
          memcpy(slot, &x, sizeof(x));
        } else {  // bigint_param_as_string: numeric text
          int len = is_unsigned
                        ? snprintf((char*)slot, stride, "%llu", (unsigned long long)u)
                        : snprintf((char*)slot, stride, "%lld", (long long)v);
          if (len < 0) len = 0;
          if ((size_t)len >= stride) len = (int)stride - 1;
          if (ind) OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)len, q);
        }
        break;
      }
      case NANOARROW_TYPE_HALF_FLOAT: {
        SQLDOUBLE v = (SQLDOUBLE)ArrowArrayViewGetDoubleUnsafe(values, row);
        memcpy(slot, &v, sizeof(v));
        break;
      }
      case NANOARROW_TYPE_DATE32: {
        DATE_STRUCT d;
        int y; unsigned m, dd;
        CivilFromDays(ArrowArrayViewGetIntUnsafe(values, row), &y, &m, &dd);
        d.year = (SQLSMALLINT)y; d.month = (SQLUSMALLINT)m; d.day = (SQLUSMALLINT)dd;
        memcpy(slot, &d, sizeof(d));
        break;
      }
      case NANOARROW_TYPE_TIMESTAMP: {
        TIMESTAMP_STRUCT ts;
        TimestampFromArrow(ArrowArrayViewGetIntUnsafe(values, row), vsv->time_unit, &ts);
        memcpy(slot, &ts, sizeof(ts));
        break;
      }
      case NANOARROW_TYPE_TIME32:
      case NANOARROW_TYPE_TIME64: {
        const int64_t v = ArrowArrayViewGetIntUnsafe(values, row);
        if (p->c_type == SQL_C_CHAR) {
          int len = TimeTextFromArrow(v, vsv->time_unit, (char*)slot, stride);
          if (ind) ind[i] = (SQLLEN)len;
        } else {
          TIME_STRUCT t;
          TimeStructFromArrow(v, vsv->time_unit, &t);
          memcpy(slot, &t, sizeof(t));
        }
        break;
      }
      case NANOARROW_TYPE_STRING:
      case NANOARROW_TYPE_LARGE_STRING:
      case NANOARROW_TYPE_STRING_VIEW: {
        struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(values, row);
        if (p->c_type == SQL_C_WCHAR) {
          int64_t units = Utf8ToUtf16Into((SQLWCHAR*)slot, s.data, s.size_bytes);
          if (ind) {
            OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)(units * (int64_t)sizeof(SQLWCHAR)), q);
          }
        } else {  // wchar_as_utf8
          if (s.size_bytes > 0) memcpy(slot, s.data, (size_t)s.size_bytes);
          slot[s.size_bytes] = '\0';
          if (ind) OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)s.size_bytes, q);
        }
        break;
      }
      case NANOARROW_TYPE_BINARY:
      case NANOARROW_TYPE_LARGE_BINARY:
      case NANOARROW_TYPE_FIXED_SIZE_BINARY:
      case NANOARROW_TYPE_BINARY_VIEW: {
        struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(values, row);
        if (b.size_bytes > 0) memcpy(slot, b.data.as_uint8, (size_t)b.size_bytes);
        if (ind) OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)b.size_bytes, q);
        break;
      }
      case NANOARROW_TYPE_DECIMAL128:
      case NANOARROW_TYPE_DECIMAL256: {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, vsv->type == NANOARROW_TYPE_DECIMAL128 ? 128 : 256,
                         vsv->decimal_precision, vsv->decimal_scale);
        ArrowArrayViewGetDecimalUnsafe(values, row, &dec);
        struct ArrowBuffer buf;
        ArrowBufferInit(&buf);
        if (ArrowDecimalAppendStringToBuffer(&dec, &buf) != NANOARROW_OK) {
          ArrowBufferReset(&buf);
          InternalAdbcSetError(error, "Failed to format decimal parameter");
          return ADBC_STATUS_INTERNAL;
        }
        size_t len = (size_t)buf.size_bytes < stride - 1 ? (size_t)buf.size_bytes : stride - 1;
        memcpy(slot, buf.data, len);
        slot[len] = '\0';
        ArrowBufferReset(&buf);
        if (ind) OdbcIndicatorSet(ind, (size_t)i, (SQLLEN)len, q);
        break;
      }
      default:
        InternalAdbcSetError(error, "Unsupported Arrow type for parameter binding: %s",
                             ArrowTypeString(vsv->type));
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
  }
  return ADBC_STATUS_OK;
}

// Undo the parameter-array statement attributes so the row-at-a-time path (and
// any later use of the handle) sees a plain single-row statement again.
static void ArrayParamsResetStmt(SQLHSTMT hstmt) {
  SQLSetStmtAttr(hstmt, SQL_ATTR_PARAM_STATUS_PTR, (SQLPOINTER)NULL, 0);
  SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, (SQLPOINTER)NULL, 0);
  SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)(SQLULEN)1, 0);
  SQLFreeStmt(hstmt, SQL_RESET_PARAMS);
}

// Rows affected by the parameter-array execute that just finished.
//
// SQL_PARAM_ARRAY_ROW_COUNTS says how the driver reports them.  SQL_PARC_BATCH
// means one row count per parameter set, reachable with SQLMoreResults, which is
// what psqlodbc does -- SQLRowCount alone would report the first set's count and
// nothing else.  SQL_PARC_NO_BATCH means SQLRowCount already holds the total for
// the whole array and there is nothing further to walk.  Summing what
// SQLMoreResults hands back is right for both, and gives DB-API the number it
// wants: an UPDATE whose parameter sets match nothing reports 0, not the number
// of sets submitted.  Returns false when the driver declines to answer.
static bool ArrayParamsRowCount(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                int64_t nsets, int64_t* affected) {
  SQLLEN count = OdbcRowCount(hstmt, opts->sqllen_32bit);
  bool answered = count >= 0;
  int64_t total = answered ? (int64_t)count : 0;
  if (opts->param_array_row_counts != SQL_PARC_NO_BATCH) {
    // At most one result per parameter set; the bound also stops a driver that
    // never says SQL_NO_DATA from spinning here forever.
    for (int64_t i = 1; i < nsets && SQL_SUCCEEDED(SQLMoreResults(hstmt)); i++) {
      count = OdbcRowCount(hstmt, opts->sqllen_32bit);
      if (count >= 0) {
        total += (int64_t)count;
        answered = true;
      }
    }
  }
  *affected = total;
  return answered;
}

/// Execute one Arrow batch using column-wise parameter arrays.
///
/// On return *rows_done holds how many leading rows of the batch were applied;
/// the caller replays the remainder row-at-a-time.  *use_array is cleared when
/// the driver turns out not to support parameter arrays, or when it stops
/// accounting for every parameter set it was handed.
static AdbcStatusCode ExecuteBatchArray(struct OdbcStatement* stmt,
                                        const struct ArrowSchemaView* svs,
                                        const struct ArrowArrayView* view, int64_t ncols,
                                        int64_t nrows, bool* use_array, int64_t* rows_done,
                                        int64_t* total, struct AdbcError* error) {
  SQLHSTMT hstmt = stmt->ref->hstmt;
  const struct OdbcReaderOptions* opts = &stmt->reader_opts;
  AdbcStatusCode status = ADBC_STATUS_OK;
  bool supported = true;
  *rows_done = 0;

  struct ArrayParam* params = calloc((size_t)ncols, sizeof(*params));
  if (!params) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  int64_t per_row = 0;
  for (int64_t i = 0; i < ncols; i++) {
    ArrayParamPlan(&params[i], &svs[i], view->children[i], nrows, opts, &supported);
    if (!supported) break;
    if (params[i].needs_buffer) per_row += params[i].elem_size;
    if (params[i].needs_indicators) per_row += (int64_t)sizeof(SQLLEN);
  }
  if (!supported) {
    free(params);
    return ADBC_STATUS_OK;  // not an error: the caller replays the batch
  }

  int64_t chunk = nrows;
  if (per_row > 0 && chunk > ARRAY_BIND_MAX_CHUNK_BYTES / per_row) {
    chunk = ARRAY_BIND_MAX_CHUNK_BYTES / per_row;
  }
  if (chunk > ARRAY_BIND_MAX_CHUNK_ROWS) chunk = ARRAY_BIND_MAX_CHUNK_ROWS;
  // A one-set parameter array buys nothing and is actively dangerous: MariaDB's
  // Connector/ODBC takes a non-array path for SQL_ATTR_PARAMSET_SIZE = 1 and then
  // segfaults on the next, larger execute of the same prepared statement.  Never
  // submit one; single leftover rows go to the row-at-a-time path instead.
  if (chunk < 2) {
    free(params);
    return ADBC_STATUS_OK;
  }

  SQLUSMALLINT* param_status = malloc(sizeof(SQLUSMALLINT) * (size_t)chunk);
  if (!param_status) {
    free(params);
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  for (int64_t i = 0; i < ncols && status == ADBC_STATUS_OK; i++) {
    if (params[i].needs_buffer) {
      params[i].buffer = malloc((size_t)params[i].elem_size * (size_t)chunk);
      if (!params[i].buffer) status = ADBC_STATUS_INTERNAL;
    }
    if (params[i].needs_indicators) {
      params[i].indicators = malloc(sizeof(SQLLEN) * (size_t)chunk);
      if (!params[i].indicators) status = ADBC_STATUS_INTERNAL;
    }
  }
  if (status != ADBC_STATUS_OK) {
    InternalAdbcSetError(error, "out of memory");
    goto cleanup;
  }

  SQLULEN processed = 0;
  SQLFreeStmt(hstmt, SQL_RESET_PARAMS);
  if (!SQL_SUCCEEDED(SQLSetStmtAttr(hstmt, SQL_ATTR_PARAM_BIND_TYPE,
                                    (SQLPOINTER)(SQLULEN)SQL_PARAM_BIND_BY_COLUMN, 0)) ||
      !SQL_SUCCEEDED(SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0))) {
    *use_array = false;  // nothing has executed yet, so the caller replays it all
    goto cleanup;
  }
  SQLSetStmtAttr(hstmt, SQL_ATTR_PARAM_STATUS_PTR, param_status, 0);

  for (int64_t row = 0; row < nrows;) {
    int64_t n = nrows - row < chunk ? nrows - row : chunk;
    if (n < 2) break;  // trailing single row: leave it to the row-at-a-time path

    if (!SQL_SUCCEEDED(SQLSetStmtAttr(hstmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)(SQLULEN)n, 0))) {
      *use_array = false;  // this chunk has not run yet
      break;
    }
    SQLFreeStmt(hstmt, SQL_CLOSE);
    for (int64_t i = 0; i < ncols; i++) {
      struct ArrayParam* p = &params[i];
      status = ArrayParamFill(p, &svs[i], view->children[i], row, n, opts->sqllen_32bit, error);
      if (status != ADBC_STATUS_OK) break;
      SQLPOINTER data =
          p->buffer ? (SQLPOINTER)p->buffer
                    : (SQLPOINTER)(uintptr_t)(p->direct + (size_t)row * (size_t)p->elem_size);
      if (!SQL_SUCCEEDED(SQLBindParameter(hstmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT, p->c_type,
                                          p->sql_type, p->column_size, p->decimal_digits, data,
                                          p->elem_size, p->indicators))) {
        status = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLBindParameter", error);
        break;
      }
    }
    if (status != ADBC_STATUS_OK) break;

    processed = 0;
    for (int64_t i = 0; i < n; i++) param_status[i] = SQL_PARAM_UNUSED;
    SQLRETURN r = stmt->prepared ? SQLExecute(hstmt)
                                 : SQLExecDirect(hstmt, (SQLCHAR*)stmt->query, SQL_NTS);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA) {
      if (OdbcReadULen(&processed, opts->sqllen_32bit) == 0 && row == 0) {
        // Nothing was applied.  Let the row-at-a-time path run: it either
        // succeeds (the driver simply dislikes parameter arrays) or reports the
        // genuine data error with the offending row's own diagnostics.
        *use_array = false;
        break;
      }
      status = OdbcSetError(SQL_HANDLE_STMT, hstmt,
                            stmt->prepared ? "SQLExecute" : "SQLExecDirect", error);
      break;
    }

    int64_t affected = 0;
    bool have_row_count = true;
    if (r == SQL_NO_DATA) {
      affected = 0;  // the statement affected no rows at all
    } else {
      have_row_count = ArrayParamsRowCount(hstmt, opts, n, &affected);
    }
    SQLSMALLINT nres = 0;
    SQLNumResultCols(hstmt, &nres);
    if (nres > 0) SQLFreeStmt(hstmt, SQL_CLOSE);

    int64_t applied = 0;
    bool status_filled = false;
    for (int64_t i = 0; i < n; i++) {
      if (param_status[i] == SQL_PARAM_UNUSED) continue;
      status_filled = true;
      if (param_status[i] == SQL_PARAM_SUCCESS || param_status[i] == SQL_PARAM_SUCCESS_WITH_INFO) {
        applied++;
      }
    }

    // How many parameter sets the driver owns up to having run.  ODBC requires
    // SQL_ATTR_PARAMS_PROCESSED_PTR to be written; the parameter-status array is
    // the fallback for drivers that fill only that.  Fewer than the whole chunk
    // means the rest did not go in, so the remainder is replayed one row at a
    // time and this statement stops using arrays.  `processed` was zeroed just
    // before the execute, so a 32-bit-SQLLEN driver's low half is the count.
    int64_t done = (int64_t)OdbcReadULen(&processed, opts->sqllen_32bit);
    if (done > n) done = n;
    if (done <= 0 && status_filled) done = applied;
    if (done <= 0) {
      // Neither counter was written, so a successful-looking execute tells us
      // nothing about what actually landed.  Replaying the chunk could duplicate
      // rows the driver did apply, so stop here instead and say what to turn off.
      InternalAdbcSetError(
          error,
          "ODBC driver accepted a parameter array of %lld sets but reported neither "
          "SQL_ATTR_PARAMS_PROCESSED_PTR nor SQL_ATTR_PARAM_STATUS_PTR, so the rows it "
          "applied cannot be determined; set \"" ADBC_ODBC_OPTION_ARRAY_BINDING
          "\" to \"false\" on the statement to bind one row at a time",
          (long long)n);
      status = ADBC_STATUS_INTERNAL;
      break;
    }

    if (have_row_count) {
      *total += affected;  // authoritative: rows affected by the chunk
    } else if (status_filled) {
      *total += applied;
    } else {
      *total += done;
    }
    row += done;
    *rows_done = row;
    if (done < n) {
      *use_array = false;
      break;
    }
  }

cleanup:
  ArrayParamsResetStmt(hstmt);
  for (int64_t i = 0; i < ncols; i++) {
    free(params[i].buffer);
    free(params[i].indicators);
  }
  free(params);
  free(param_status);
  return status;
}

// Bind row `row` of `view` as the statement's parameters and execute once.
// *out_result_cols receives the number of result columns the execution produced.
static AdbcStatusCode BindAndExecuteRow(SQLHSTMT hstmt, bool prepared, const char* query,
                                        struct ParamSlot* slots, const struct ArrowSchemaView* svs,
                                        const struct ArrowArrayView* view, int64_t ncols,
                                        int64_t row, const struct OdbcReaderOptions* opts,
                                        SQLSMALLINT* out_result_cols, struct AdbcError* error) {
  SQLFreeStmt(hstmt, SQL_CLOSE);
  for (int64_t i = 0; i < ncols; i++) {
    struct ParamSlot* p = &slots[i];
    RAISE_ADBC(SlotFromArrow(p, &svs[i], view->children[i], row, opts, error));
    if (p->indicator == SQL_NULL_DATA) {
      // Bind NULLs with the driver's own idea of the parameter type when it can
      // tell us (SQLDescribeParam), a NULL value pointer and SQL_C_DEFAULT -- the
      // combination every driver we have met encodes correctly (pyodbc does the same).
      SQLSMALLINT dtype = 0, ddigits = 0, dnullable = 0;
      SQLULEN dsize = 0;  // zeroed: a 32-bit-SQLLEN driver writes only the low half
      if (!opts->no_describe_param &&
          SQL_SUCCEEDED(SQLDescribeParam(hstmt, (SQLUSMALLINT)(i + 1), &dtype, &dsize, &ddigits,
                                         &dnullable)) &&
          dtype != 0 && dtype != SQL_UNKNOWN_TYPE) {
        dsize = OdbcReadULen(&dsize, opts->sqllen_32bit);
        p->sql_type = dtype;
        p->column_size = dsize ? dsize : 1;
        p->decimal_digits = ddigits;
      }
      p->c_type = SQL_C_DEFAULT;
      p->data = NULL;
      p->buffer_length = 0;
    }
    p->bound_indicator = 0;
    OdbcIndicatorSet(&p->bound_indicator, 0, p->indicator, opts->sqllen_32bit);
    SQLRETURN r = SQLBindParameter(hstmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT, p->c_type,
                                   p->sql_type, p->column_size, p->decimal_digits,
                                   (SQLPOINTER)p->data, p->buffer_length, &p->bound_indicator);
    if (!SQL_SUCCEEDED(r)) return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLBindParameter", error);
  }
  SQLRETURN r = prepared ? SQLExecute(hstmt) : SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS);
  if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA) {
    return OdbcSetError(SQL_HANDLE_STMT, hstmt, prepared ? "SQLExecute" : "SQLExecDirect", error);
  }
  *out_result_cols = 0;
  SQLNumResultCols(hstmt, out_result_cols);
  return ADBC_STATUS_OK;
}

// Execute hstmt once per row of the bound stream, returning total rows affected.
// Any result set an execution happens to produce is discarded; the caller that wants
// one goes through BoundReaderInit instead.
// ---------------------------------------------------------------------------
// Automatic transaction batching
//
// Executing a bound stream row by row with the connection in autocommit costs a
// commit -- for most engines a round trip and an fsync -- per row, which is what
// made bulk ingest two orders of magnitude slower than the fetch path.  When the
// caller has not opened a transaction of their own, turn autocommit off for the
// duration of the execute and commit once at the end.

struct OdbcAutoTxn {
  struct OdbcConnection* conn;
  bool active;
};

static void OdbcAutoTxnInit(struct OdbcAutoTxn* txn, struct OdbcConnection* conn) {
  txn->conn = conn;
  txn->active = false;
}

// Turn autocommit off, once.  A connection the caller has already taken out of
// autocommit is left alone: that transaction is theirs to commit.  A driver that
// refuses is simply left autocommitting -- slower, but correct.
static void OdbcAutoTxnBegin(struct OdbcAutoTxn* txn) {
  struct OdbcConnection* conn = txn->conn;
  if (txn->active || !conn || !conn->connected) return;
  if (!conn->autocommit || !conn->reader_opts.txn_capable) return;
  if (!SQL_SUCCEEDED(SQLSetConnectAttr(conn->hdbc, SQL_ATTR_AUTOCOMMIT,
                                       (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0))) {
    return;
  }
  txn->active = true;
}

// Commit (or, when the execute failed, roll back) and restore autocommit.
static AdbcStatusCode OdbcAutoTxnEnd(struct OdbcAutoTxn* txn, bool commit,
                                     struct AdbcError* error) {
  if (!txn->active) return ADBC_STATUS_OK;
  txn->active = false;
  struct OdbcConnection* conn = txn->conn;
  AdbcStatusCode status = ADBC_STATUS_OK;
  SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, commit ? SQL_COMMIT : SQL_ROLLBACK);
  if (commit && !SQL_SUCCEEDED(r)) {
    status = OdbcSetError(SQL_HANDLE_DBC, conn->hdbc, "SQLEndTran(SQL_COMMIT)", error);
  }
  SQLSetConnectAttr(conn->hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_ON, 0);
  return status;
}

// ---------------------------------------------------------------------------
// Multi-row INSERT batching (bulk ingest only)
//
// Parameter arrays collapse a whole Arrow batch into one SQLExecute, but five of the
// ODBC drivers in the compatibility matrix mishandle them (see
// OdbcReaderOptions::no_param_arrays) and one -- MySQL Connector/ODBC -- accepts them
// and then walks them row by row inside the driver.  On those, ingest costs one
// SQLExecute, and for a client/server database one network round trip, per row:
// clickhouse-odbc sends one HTTP request per row and manages sixteen rows a second.
//
// The rewrite here needs no array support at all.  Instead of executing
// `INSERT INTO t VALUES (?,?,?,?)` N times, it prepares
// `INSERT INTO t VALUES (?,?,?,?),(?,?,?,?),...` with K row-groups and binds K rows'
// worth of ordinary scalar parameters per execute, over exactly the same
// SQLBindParameter machinery (and the same per-driver parameter quirks) as the
// row-at-a-time path.  Round trips drop by a factor of K.
//
// Only the INSERT that bulk ingest generates is ever rewritten: it is reached through
// OdbcStatement::ingest_into, which nothing but OdbcStatementIngest sets.  A query the
// caller wrote is theirs and is executed as written.

// A prepared INSERT carrying a fixed number of row-groups.
struct MultiRowGroup {
  SQLHSTMT hstmt;
  int64_t rows;
};

struct MultiRowInsert {
  struct OdbcStatement* stmt;
  bool enabled;  // the ingest path and the option allow the rewrite
  bool ready;    // the setup below has run
  bool active;   // ... and produced a usable prepared statement
  bool insert_all;
  int64_t ncols;
  int64_t rows;               // K: row-groups in the `full` statement
  struct MultiRowGroup full;  // K row-groups; used for every whole group of a batch
  struct MultiRowGroup tail;  // the last partial group, prepared on demand
  struct ParamSlot* slots;    // K * ncols, reused by every execute
  int64_t nslots;
  // SQLDescribeParam of the first row-group's parameters, so a NULL is bound with the
  // type the driver expects.  Parameter i+1 of the K-row statement is column i, and the
  // answer is the same for every row-group, so it is asked for once per column.
  SQLSMALLINT* null_type;
  SQLULEN* null_size;
  SQLSMALLINT* null_digits;
  signed char* null_described;  // 0 unknown, 1 answered, -1 refused
};

// `INSERT INTO t (a, b) VALUES (?, ?), (?, ?)`, or Oracle's
// `INSERT ALL INTO t (a, b) VALUES (?, ?) INTO t (a, b) VALUES (?, ?) SELECT 1 FROM dual`.
// Returns a malloc'd string, or NULL on allocation failure.
static char* MultiRowSql(const char* into, int64_t ncols, int64_t rows, bool insert_all) {
  struct InternalAdbcStringBuilder sb;
  if (InternalAdbcStringBuilderInit(&sb, 256) != 0) return NULL;
  if (insert_all) {
    InternalAdbcStringBuilderAppend(&sb, "INSERT ALL");
  } else {
    InternalAdbcStringBuilderAppend(&sb, "INSERT INTO %s VALUES ", into);
  }
  for (int64_t r = 0; r < rows; r++) {
    if (insert_all) {
      InternalAdbcStringBuilderAppend(&sb, " INTO %s VALUES (", into);
    } else {
      InternalAdbcStringBuilderAppend(&sb, r ? ", (" : "(");
    }
    for (int64_t c = 0; c < ncols; c++) InternalAdbcStringBuilderAppend(&sb, c ? ", ?" : "?");
    InternalAdbcStringBuilderAppend(&sb, ")");
  }
  if (insert_all) InternalAdbcStringBuilderAppend(&sb, " SELECT 1 FROM dual");
  char* out = sb.buffer ? strdup(sb.buffer) : NULL;
  InternalAdbcStringBuilderReset(&sb);
  return out;
}

// Allocate a statement handle and SQLPrepare the `rows`-group INSERT on it.  NULL when
// the driver or the server refuses the statement -- which is the probe: too many
// parameters, or a server with no multi-row VALUES at all.
static SQLHSTMT MultiRowPrepare(struct OdbcConnection* conn, const char* into, int64_t ncols,
                                int64_t rows, bool insert_all) {
  char* sql = MultiRowSql(into, ncols, rows, insert_all);
  if (!sql) return NULL;
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt))) {
    free(sql);
    return NULL;
  }
  if (!SQL_SUCCEEDED(SQLPrepare(hstmt, (SQLCHAR*)sql, SQL_NTS))) {
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    hstmt = NULL;
  }
  free(sql);
  return hstmt;
}

static void MultiRowInit(struct MultiRowInsert* mr, struct OdbcStatement* stmt, int64_t ncols) {
  memset(mr, 0, sizeof(*mr));
  mr->stmt = stmt;
  mr->ncols = ncols;
  // Only bulk ingest, only with something to batch, and only when the option allows it.
  mr->enabled = stmt->ingest_into != NULL && ncols > 0 && stmt->rows_per_insert != 1 &&
                stmt->conn != NULL && stmt->conn->connected;
}

static void MultiRowReset(struct MultiRowInsert* mr) {
  if (mr->full.hstmt) SQLFreeHandle(SQL_HANDLE_STMT, mr->full.hstmt);
  if (mr->tail.hstmt) SQLFreeHandle(SQL_HANDLE_STMT, mr->tail.hstmt);
  mr->full.hstmt = NULL;
  mr->tail.hstmt = NULL;
  for (int64_t i = 0; i < mr->nslots; i++) ArrowBufferReset(&mr->slots[i].wbuf);
  free(mr->slots);
  mr->slots = NULL;
  mr->nslots = 0;
  free(mr->null_type);
  free(mr->null_size);
  free(mr->null_digits);
  free(mr->null_described);
  mr->null_type = NULL;
  mr->null_size = NULL;
  mr->null_digits = NULL;
  mr->null_described = NULL;
  mr->active = false;
}

// Decide K and prepare the K-row statement.  Runs once, on the first batch worth
// batching, before the ingest transaction opens -- a refused SQLPrepare must not be able
// to poison a transaction on a server that aborts one on any error.
static void MultiRowSetup(struct MultiRowInsert* mr) {
  mr->ready = true;
  struct OdbcStatement* stmt = mr->stmt;
  struct OdbcConnection* conn = stmt->conn;
  const struct OdbcReaderOptions* opts = &stmt->reader_opts;
  const char* into = stmt->ingest_into;
  const int64_t ncols = mr->ncols;
  if (conn->multirow_unsupported) return;

  // Does this server take a multi-row INSERT at all?  Two row-groups is the cheapest
  // question that answers it, and it separates "the form is not supported" from "that
  // many parameters is too many", which the search below handles instead.
  if (!conn->multirow_probed) {
    bool insert_all = false;
    SQLHSTMT probe = MultiRowPrepare(conn, into, ncols, 2, false);
    if (!probe && opts->multirow_insert_all) {
      // Oracle has no multi-row VALUES; INSERT ALL is its spelling.
      probe = MultiRowPrepare(conn, into, ncols, 2, true);
      insert_all = probe != NULL;
    }
    conn->multirow_probed = true;
    if (!probe) {
      conn->multirow_unsupported = true;
      return;
    }
    SQLFreeHandle(SQL_HANDLE_STMT, probe);
    conn->multirow_insert_all = insert_all;
  }
  mr->insert_all = conn->multirow_insert_all;

  // A row count the caller asked for is taken at its word, subject only to what this
  // connection has actually been refused and to the hard budgets below; the default
  // parameter budget is a guess about an unknown backend, and the caller may know
  // better.  Nothing asked for means the guess.
  int64_t k = stmt->rows_per_insert > 0 ? stmt->rows_per_insert
                                        : ADBC_ODBC_MULTIROW_MAX_PARAMS / ncols;
  if (conn->multirow_max_params > 0 && k > conn->multirow_max_params / ncols) {
    k = conn->multirow_max_params / ncols;
  }
  if (k > ADBC_ODBC_MULTIROW_MAX_ROWS) k = ADBC_ODBC_MULTIROW_MAX_ROWS;
  // SQL text budget: `(?, ?, ?, ?), ` plus, for INSERT ALL, another `INTO <table> VALUES `.
  {
    int64_t sql_max = (opts->max_statement_len > 0 &&
                       opts->max_statement_len < ADBC_ODBC_MULTIROW_MAX_SQL_BYTES)
                          ? opts->max_statement_len
                          : ADBC_ODBC_MULTIROW_MAX_SQL_BYTES;
    int64_t into_len = (int64_t)strlen(into);
    int64_t per_group = ncols * 3 + 4 + (mr->insert_all ? into_len + 20 : 0);
    int64_t budget = (sql_max - into_len - 64) / per_group;
    if (budget < k) k = budget;
  }
  // Parameter scratch budget.
  {
    int64_t per_group = (int64_t)sizeof(struct ParamSlot) * ncols;
    int64_t budget = ADBC_ODBC_MULTIROW_MAX_SLOT_BYTES / (per_group > 0 ? per_group : 1);
    if (budget < k) k = budget;
  }
  if (k < 2) return;  // a one-group "batch" is the row-at-a-time path with extra steps

  // Prepare, halving on refusal: the parameter ceiling is not something ODBC lets a
  // driver report (there is no SQL_MAX_PARAMETERS), so it has to be found by asking.
  bool narrowed = false;
  SQLHSTMT hstmt = NULL;
  while (k >= 2) {
    hstmt = MultiRowPrepare(conn, into, ncols, k, mr->insert_all);
    if (hstmt) break;
    k /= 2;
    narrowed = true;
  }
  if (!hstmt) {
    // Two row-groups prepared a moment ago and K >= 2 will not: nothing about this
    // statement is going to work.  Leave the ingest on the paths that already do.
    conn->multirow_unsupported = true;
    return;
  }
  // Remember a ceiling only when one was actually hit; a small K the caller asked for
  // says nothing about what the server would have taken.
  if (narrowed) conn->multirow_max_params = k * ncols;

  mr->slots = calloc((size_t)(k * ncols), sizeof(*mr->slots));
  mr->null_type = calloc((size_t)ncols, sizeof(*mr->null_type));
  mr->null_size = calloc((size_t)ncols, sizeof(*mr->null_size));
  mr->null_digits = calloc((size_t)ncols, sizeof(*mr->null_digits));
  mr->null_described = calloc((size_t)ncols, sizeof(*mr->null_described));
  if (!mr->slots || !mr->null_type || !mr->null_size || !mr->null_digits || !mr->null_described) {
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    MultiRowReset(mr);
    return;
  }
  mr->nslots = k * ncols;
  for (int64_t i = 0; i < mr->nslots; i++) ArrowBufferInit(&mr->slots[i].wbuf);
  mr->rows = k;
  mr->full.hstmt = hstmt;
  mr->full.rows = k;
  mr->active = true;
}

// The prepared statement for a group of exactly `n` rows, or NULL if it cannot be had.
// The tail statement is cached because consecutive batches of the same shape leave the
// same remainder.
static SQLHSTMT MultiRowGroupFor(struct MultiRowInsert* mr, int64_t n) {
  if (n == mr->full.rows) return mr->full.hstmt;
  if (mr->tail.hstmt && mr->tail.rows == n) return mr->tail.hstmt;
  if (mr->tail.hstmt) {
    SQLFreeHandle(SQL_HANDLE_STMT, mr->tail.hstmt);
    mr->tail.hstmt = NULL;
    mr->tail.rows = 0;
  }
  SQLHSTMT hstmt =
      MultiRowPrepare(mr->stmt->conn, mr->stmt->ingest_into, mr->ncols, n, mr->insert_all);
  if (!hstmt) return NULL;
  mr->tail.hstmt = hstmt;
  mr->tail.rows = n;
  return hstmt;
}

// Halve the row-group count and re-prepare, for a driver that takes the K-row statement
// at SQLPrepare and then refuses it at SQLExecute.  clickhouse-odbc does exactly that
// above a few dozen row-groups.  Returns false when there is nothing smaller left to try.
static bool MultiRowNarrow(struct MultiRowInsert* mr) {
  struct OdbcConnection* conn = mr->stmt->conn;
  int64_t k = mr->rows / 2;
  SQLHSTMT hstmt = NULL;
  while (k >= 2) {
    hstmt = MultiRowPrepare(conn, mr->stmt->ingest_into, mr->ncols, k, mr->insert_all);
    if (hstmt) break;
    k /= 2;
  }
  if (!hstmt) return false;
  if (mr->full.hstmt) SQLFreeHandle(SQL_HANDLE_STMT, mr->full.hstmt);
  if (mr->tail.hstmt) SQLFreeHandle(SQL_HANDLE_STMT, mr->tail.hstmt);
  mr->tail.hstmt = NULL;
  mr->tail.rows = 0;
  mr->full.hstmt = hstmt;
  mr->full.rows = k;
  mr->rows = k;  // the slot array was sized for the larger K, so it still fits
  conn->multirow_max_params = k * mr->ncols;
  // The describe cache belongs to the handle that is gone.
  memset(mr->null_described, 0, (size_t)mr->ncols * sizeof(*mr->null_described));
  return true;
}

// Type a NULL parameter of column `col` the way BindAndExecuteRow does, but asking the
// driver only once per column instead of once per NULL.
static void MultiRowNullType(struct MultiRowInsert* mr, SQLHSTMT hstmt, int64_t col,
                             struct ParamSlot* p) {
  const struct OdbcReaderOptions* opts = &mr->stmt->reader_opts;
  if (!opts->no_describe_param && mr->null_described[col] == 0) {
    SQLSMALLINT dtype = 0, ddigits = 0, dnullable = 0;
    SQLULEN dsize = 0;  // zeroed: a 32-bit-SQLLEN driver writes only the low half
    mr->null_described[col] = -1;
    if (SQL_SUCCEEDED(SQLDescribeParam(hstmt, (SQLUSMALLINT)(col + 1), &dtype, &dsize, &ddigits,
                                       &dnullable)) &&
        dtype != 0 && dtype != SQL_UNKNOWN_TYPE) {
      dsize = OdbcReadULen(&dsize, opts->sqllen_32bit);
      mr->null_type[col] = dtype;
      mr->null_size[col] = dsize ? dsize : 1;
      mr->null_digits[col] = ddigits;
      mr->null_described[col] = 1;
    }
  }
  if (mr->null_described[col] == 1) {
    p->sql_type = mr->null_type[col];
    p->column_size = mr->null_size[col];
    p->decimal_digits = mr->null_digits[col];
  }
  p->c_type = SQL_C_DEFAULT;
  p->data = NULL;
  p->buffer_length = 0;
}

// Bind rows [row0, row0 + n) into `hstmt`'s n row-groups and execute once.
static AdbcStatusCode MultiRowExecGroup(struct MultiRowInsert* mr, SQLHSTMT hstmt,
                                        const struct ArrowSchemaView* svs,
                                        const struct ArrowArrayView* view, int64_t row0, int64_t n,
                                        int64_t* total, struct AdbcError* error) {
  const struct OdbcReaderOptions* opts = &mr->stmt->reader_opts;
  const int64_t ncols = mr->ncols;
  SQLFreeStmt(hstmt, SQL_CLOSE);
  for (int64_t r = 0; r < n; r++) {
    for (int64_t c = 0; c < ncols; c++) {
      struct ParamSlot* p = &mr->slots[r * ncols + c];
      RAISE_ADBC(SlotFromArrow(p, &svs[c], view->children[c], row0 + r, opts, error));
      if (p->indicator == SQL_NULL_DATA) MultiRowNullType(mr, hstmt, c, p);
      p->bound_indicator = 0;
      OdbcIndicatorSet(&p->bound_indicator, 0, p->indicator, opts->sqllen_32bit);
      if (!SQL_SUCCEEDED(SQLBindParameter(hstmt, (SQLUSMALLINT)(r * ncols + c + 1),
                                          SQL_PARAM_INPUT, p->c_type, p->sql_type, p->column_size,
                                          p->decimal_digits, (SQLPOINTER)p->data, p->buffer_length,
                                          &p->bound_indicator))) {
        return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLBindParameter", error);
      }
    }
  }
  SQLRETURN ret = SQLExecute(hstmt);
  if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA) {
    return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLExecute", error);
  }
  // An INSERT that did not raise has inserted every row-group it was given, so the group
  // size is the row count -- and it is a better one than the driver's: DuckDB answers
  // SQLRowCount with 1 for a multi-row INSERT however many row-groups it carried, and
  // clickhouse-odbc answers -1.  (Only the INSERT that bulk ingest generates reaches
  // here; there is no WHERE clause or conflict rule that could apply fewer.)
  SQLSMALLINT nres = 0;
  *total += n;
  SQLNumResultCols(hstmt, &nres);
  if (nres > 0) SQLFreeStmt(hstmt, SQL_CLOSE);
  return ADBC_STATUS_OK;
}

/// Ingest as much of one Arrow batch as whole row-groups can carry.
///
/// Rows [row0, row0 + nrows) are the ones on offer.  *rows_done receives how many
/// leading rows of those went in; the caller applies the rest (a single trailing row, or
/// everything if the setup did not take) by its other paths.
/// *fell_back is set when the server turned out to reject the multi-row form at execute
/// time and nothing at all had been applied yet, so the caller may safely replay the
/// whole batch; the connection remembers the refusal and never asks again.
static AdbcStatusCode MultiRowExecuteBatch(struct MultiRowInsert* mr,
                                           const struct ArrowSchemaView* svs,
                                           const struct ArrowArrayView* view, int64_t row0,
                                           int64_t nrows, bool virgin, int64_t* rows_done,
                                           int64_t* total, bool* fell_back,
                                           struct AdbcError* error) {
  int64_t row = 0;
  while (nrows - row >= 2) {
    int64_t n = nrows - row;
    if (n > mr->rows) n = mr->rows;
    SQLHSTMT hstmt = MultiRowGroupFor(mr, n);
    if (!hstmt) break;  // no statement for this remainder: the caller finishes the batch
    AdbcStatusCode status = MultiRowExecGroup(mr, hstmt, svs, view, row0 + row, n, total, error);
    if (status != ADBC_STATUS_OK) {
      if (!virgin || row != 0) return status;
      // Nothing has been applied anywhere yet, so this is still a probe.  A driver can
      // accept the K-row statement at SQLPrepare and refuse it at SQLExecute -- most
      // often because K parameters is more than it will carry -- so halve K and ask
      // again before giving the form up.
      if (error && error->release) error->release(error);
      if (MultiRowNarrow(mr)) continue;
      // Nothing smaller works either: drop to the paths that already do, and let the
      // caller replay the batch.  If the refusal was a data error rather than a size
      // one, the row-at-a-time path reports it with the offending row's diagnostics.
      mr->stmt->conn->multirow_unsupported = true;
      mr->active = false;
      *fell_back = true;
      return ADBC_STATUS_OK;
    }
    row += n;
  }
  *rows_done = row;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode ExecuteRows(struct OdbcStatement* stmt, int64_t* rows_affected,
                                  struct AdbcError* error) {
  SQLHSTMT hstmt = stmt->ref->hstmt;
  struct ArrowArrayStream* stream = &stmt->bind_stream;
  struct ArrowSchema schema;
  schema.release = NULL;
  int rc = stream->get_schema(stream, &schema);
  if (rc != 0) {
    InternalAdbcSetError(error, "Bound stream get_schema failed: %s", stream->get_last_error(stream));
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  struct ArrowError na_error;
  struct ArrowArrayView view;
  AdbcStatusCode status = ADBC_STATUS_OK;
  int64_t total = 0;

  int64_t ncols = schema.n_children;
  struct ArrowSchemaView* svs = calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(*svs));
  struct ParamSlot* slots = calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(*slots));
  for (int64_t i = 0; i < ncols; i++) ArrowBufferInit(&slots[i].wbuf);
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayViewInitFromSchema(&view, &schema, &na_error), &na_error, error);
  for (int64_t i = 0; i < ncols; i++) {
    CHECK_NA_DETAIL(INTERNAL, ArrowSchemaViewInit(&svs[i], schema.children[i], &na_error), &na_error, error);
  }

  // Array binding only helps for multi-row, non-result-producing executions.
  bool use_array = stmt->array_binding && ncols > 0;
  // Bulk ingest can instead pack K rows into one INSERT ... VALUES (...),(...), which
  // needs nothing of the driver beyond ordinary parameters -- so it works on the five
  // drivers whose parameter arrays are unusable, and is faster than arrays on the ones
  // where they work.
  struct MultiRowInsert mr;
  MultiRowInit(&mr, stmt, ncols);
  // Ahead of parameter arrays, not behind them: on every server measured, one INSERT
  // carrying K row-groups beats the same rows submitted as an array (PostgreSQL 221k
  // rows/s against 97k, SQL Server 157k against 85k, Oracle 40k against 1.8k, MariaDB
  // 224k against 211k).  A driver whose arrays really are faster opts out with
  // prefer_param_arrays; arrays also remain the path for anything the probe rules out.
  const bool multirow_first = mr.enabled && !(use_array && stmt->reader_opts.prefer_param_arrays);
  // This function is only reached when the statement returns no rows, so there is
  // never an open result set to keep alive across the commit.
  struct OdbcAutoTxn txn;
  OdbcAutoTxnInit(&txn, stmt->conn);
  int64_t seen = 0;
  bool applied = false;  // has anything at all reached the server yet?

  for (;;) {
    struct ArrowArray batch;
    batch.release = NULL;
    rc = stream->get_next(stream, &batch);
    if (rc != 0) {
      InternalAdbcSetError(error, "Bound stream get_next failed: %s", stream->get_last_error(stream));
      status = ADBC_STATUS_INVALID_ARGUMENT;
      break;
    }
    if (!batch.release) break;
    if (ArrowArrayViewSetArray(&view, &batch, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Invalid bound batch: %s", na_error.message);
      status = ADBC_STATUS_INVALID_ARGUMENT;
      batch.release(&batch);
      break;
    }
    // Prepare the multi-row INSERT before the transaction opens: its SQLPrepare is the
    // probe that decides whether this server has the form at all, and a statement
    // refused inside a transaction aborts the transaction on some servers.
    if (multirow_first && !mr.ready && batch.length > 1) MultiRowSetup(&mr);
    // Once more than one row is in play, one commit for the lot beats one per row.
    seen += batch.length;
    if (seen > 1) OdbcAutoTxnBegin(&txn);
    int64_t row = 0;
    if (multirow_first && mr.active && batch.length > 1) {
      int64_t done = 0;
      bool fell_back = false;
      status = MultiRowExecuteBatch(&mr, svs, &view, 0, batch.length, !applied, &done, &total,
                                    &fell_back, error);
      row = done;
      if (fell_back) {
        // The probe applied nothing.  Start the transaction over so the replay below
        // runs on a clean one (PostgreSQL aborts a transaction on any error).
        OdbcAutoTxnEnd(&txn, false, NULL);
        if (seen > 1) OdbcAutoTxnBegin(&txn);
      }
    }
    if (use_array && row == 0 && batch.length > 1 && status == ADBC_STATUS_OK) {
      int64_t done = 0;
      status = ExecuteBatchArray(stmt, svs, &view, ncols, batch.length, &use_array, &done, &total,
                                 error);
      row = done;
    }
    // Parameter arrays gave up part way through the batch (or turned out not to work at
    // all): a multi-row INSERT still beats one execute per row for what is left.
    if (mr.enabled && !mr.ready && batch.length - row > 1 && status == ADBC_STATUS_OK) {
      MultiRowSetup(&mr);
    }
    if (mr.active && batch.length - row > 1 && status == ADBC_STATUS_OK) {
      int64_t done = 0;
      bool fell_back = false;
      status = MultiRowExecuteBatch(&mr, svs, &view, row, batch.length - row,
                                    !applied && row == 0, &done, &total, &fell_back, error);
      row += done;
    }
    if (row > 0) applied = true;
    for (; row < batch.length && status == ADBC_STATUS_OK; row++) {
      SQLSMALLINT nres = 0;
      status = BindAndExecuteRow(hstmt, stmt->prepared, stmt->query, slots, svs, &view, ncols, row,
                                 &stmt->reader_opts, &nres, error);
      if (status != ADBC_STATUS_OK) break;
      applied = true;
      SQLLEN count = OdbcRowCount(hstmt, stmt->reader_opts.sqllen_32bit);
      if (count > 0) total += count;
      if (nres > 0) SQLCloseCursor(hstmt);
    }
    batch.release(&batch);
    if (status != ADBC_STATUS_OK) break;
  }
  {
    // Commit everything the stream produced, or roll it back so a failed ingest
    // does not leave half a table behind.
    AdbcStatusCode txn_status = OdbcAutoTxnEnd(&txn, status == ADBC_STATUS_OK, error);
    if (status == ADBC_STATUS_OK) status = txn_status;
  }
  MultiRowReset(&mr);
  ArrowArrayViewReset(&view);
  free(svs);
  if (schema.release) schema.release(&schema);
  SQLFreeStmt(hstmt, SQL_RESET_PARAMS);
  for (int64_t i = 0; i < ncols; i++) ArrowBufferReset(&slots[i].wbuf);
  // The stream is consumed; drop it.
  stream->release(stream);
  memset(stream, 0, sizeof(*stream));
  stmt->has_bind = false;
  free(slots);
  if (status != ADBC_STATUS_OK) return status;
  if (rows_affected) *rows_affected = total;
  return status;
}

// ---------------------------------------------------------------------------
// Lazy re-execution reader
//
// ADBC requires a result-returning query bound to an N-row parameter batch to be
// executed once per row, with the concatenation of the N result sets exposed as a
// single stream.  This reader owns the parameter stream and the statement handle,
// drains one result set at a time through OdbcReaderInit, and only re-executes when
// the consumer has finished the previous one.  rows_affected is -1 for this path
// because the row counts are not known until the whole stream has been consumed.

struct BoundReader {
  struct OdbcHandleRef* ref;
  struct OdbcReaderOptions opts;
  bool prepared;
  char* query;  // copy: the AdbcStatement may be released before the stream is
  // Parameters
  struct ArrowArrayStream params;
  struct ArrowSchema param_schema;
  struct ArrowArrayView view;
  bool view_ready;
  struct ArrowSchemaView* svs;
  struct ParamSlot* slots;
  int64_t ncols;
  struct ArrowArray batch;  // current parameter batch; release == NULL when none
  int64_t row;              // next row to execute within `batch`
  bool params_done;
  // Results
  struct ArrowSchema schema;      // schema of the first result set
  struct ArrowArrayStream inner;  // reader over the result set being drained
  bool done;
  struct AdbcError error;
  char error_message[1024];
};

// Two result sets can be concatenated when their column types line up; column
// names are allowed to differ (drivers name expression columns inconsistently).
static bool ResultSchemaMatches(const struct ArrowSchema* a, const struct ArrowSchema* b) {
  if (a->n_children != b->n_children) return false;
  for (int64_t i = 0; i < a->n_children; i++) {
    const char* fa = a->children[i]->format;
    const char* fb = b->children[i]->format;
    if (!fa || !fb || strcmp(fa, fb) != 0) return false;
  }
  return true;
}

static void BoundReaderFree(struct BoundReader* r) {
  if (!r) return;
  if (r->inner.release) r->inner.release(&r->inner);
  if (r->batch.release) r->batch.release(&r->batch);
  if (r->view_ready) ArrowArrayViewReset(&r->view);
  if (r->params.release) r->params.release(&r->params);
  if (r->param_schema.release) r->param_schema.release(&r->param_schema);
  if (r->schema.release) r->schema.release(&r->schema);
  if (r->slots) {
    for (int64_t i = 0; i < r->ncols; i++) ArrowBufferReset(&r->slots[i].wbuf);
    free(r->slots);
  }
  free(r->svs);
  if (r->ref && r->ref->hstmt) {
    SQLFreeStmt(r->ref->hstmt, SQL_RESET_PARAMS);
    SQLFreeStmt(r->ref->hstmt, SQL_CLOSE);
  }
  OdbcHandleRefRelease(r->ref);
  free(r->query);
  if (r->error.release) r->error.release(&r->error);
  free(r);
}

// Execute the next parameter row.  Sets *exhausted when the parameter stream is
// spent; otherwise *out_result_cols is that execution's result column count.
static AdbcStatusCode BoundReaderExecuteNextRow(struct BoundReader* r, bool* exhausted,
                                                SQLSMALLINT* out_result_cols,
                                                struct AdbcError* error) {
  struct ArrowError na_error;
  *exhausted = false;
  for (;;) {
    if (r->batch.release && r->row < r->batch.length) break;
    if (r->batch.release) {
      r->batch.release(&r->batch);
      r->batch.release = NULL;
    }
    if (r->params_done) {
      *exhausted = true;
      return ADBC_STATUS_OK;
    }
    r->batch.release = NULL;
    int rc = r->params.get_next(&r->params, &r->batch);
    if (rc != 0) {
      InternalAdbcSetError(error, "Bound stream get_next failed: %s",
                           r->params.get_last_error(&r->params));
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    if (!r->batch.release) {
      r->params_done = true;
      *exhausted = true;
      return ADBC_STATUS_OK;
    }
    if (ArrowArrayViewSetArray(&r->view, &r->batch, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Invalid bound batch: %s", na_error.message);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    r->row = 0;
  }
  int64_t row = r->row++;
  return BindAndExecuteRow(r->ref->hstmt, r->prepared, r->query, r->slots, r->svs, &r->view,
                           r->ncols, row, &r->opts, out_result_cols, error);
}

// Re-execute until an execution yields a result set, and open a reader over it.
// Sets *exhausted when the parameter rows run out first.
static AdbcStatusCode BoundReaderOpenNextResult(struct BoundReader* r, bool* exhausted,
                                                struct AdbcError* error) {
  for (;;) {
    SQLSMALLINT nres = 0;
    RAISE_ADBC(BoundReaderExecuteNextRow(r, exhausted, &nres, error));
    if (*exhausted) return ADBC_STATUS_OK;
    if (nres <= 0) continue;  // e.g. an INSERT row mixed into the batch
    RAISE_ADBC(OdbcReaderInit(r->ref, &r->opts, &r->inner, error));
    if (!r->schema.release) {
      int rc = r->inner.get_schema(&r->inner, &r->schema);
      if (rc != 0) {
        InternalAdbcSetError(error, "Failed to read result schema");
        return ADBC_STATUS_INTERNAL;
      }
      return ADBC_STATUS_OK;
    }
    struct ArrowSchema next;
    next.release = NULL;
    if (r->inner.get_schema(&r->inner, &next) != 0) {
      InternalAdbcSetError(error, "Failed to read result schema");
      return ADBC_STATUS_INTERNAL;
    }
    bool ok = ResultSchemaMatches(&r->schema, &next);
    next.release(&next);
    if (!ok) {
      InternalAdbcSetError(error,
                           "Bound parameter row %" PRId64
                           " produced a result set with a different schema than the first row",
                           r->row - 1);
      return ADBC_STATUS_INVALID_STATE;
    }
    return ADBC_STATUS_OK;
  }
}

static int BoundReaderGetSchema(struct ArrowArrayStream* stream, struct ArrowSchema* out) {
  struct BoundReader* r = (struct BoundReader*)stream->private_data;
  if (!r) return EINVAL;
  return ArrowSchemaDeepCopy(&r->schema, out);
}

static int BoundReaderGetNext(struct ArrowArrayStream* stream, struct ArrowArray* out) {
  struct BoundReader* r = (struct BoundReader*)stream->private_data;
  if (!r) return EINVAL;
  out->release = NULL;
  if (r->done) return 0;
  if (r->error.release) {
    r->error.release(&r->error);
    memset(&r->error, 0, sizeof(r->error));
  }
  for (;;) {
    if (r->inner.release) {
      int rc = r->inner.get_next(&r->inner, out);
      if (rc != 0) {
        const char* msg = r->inner.get_last_error(&r->inner);
        snprintf(r->error_message, sizeof(r->error_message), "%s", msg ? msg : "unknown error");
        r->done = true;
        return rc;
      }
      if (out->release) return 0;
      r->inner.release(&r->inner);
      memset(&r->inner, 0, sizeof(r->inner));
    }
    bool exhausted = false;
    AdbcStatusCode status = BoundReaderOpenNextResult(r, &exhausted, &r->error);
    if (status != ADBC_STATUS_OK) {
      snprintf(r->error_message, sizeof(r->error_message), "%s",
               r->error.message ? r->error.message : "unknown error");
      r->done = true;
      return InternalAdbcStatusCodeToErrno(status);
    }
    if (exhausted) {
      r->done = true;
      return 0;
    }
  }
}

static const char* BoundReaderGetLastError(struct ArrowArrayStream* stream) {
  struct BoundReader* r = (struct BoundReader*)stream->private_data;
  if (!r) return NULL;
  return r->error_message[0] ? r->error_message : NULL;
}

static void BoundReaderRelease(struct ArrowArrayStream* stream) {
  BoundReaderFree((struct BoundReader*)stream->private_data);
  stream->private_data = NULL;
  stream->release = NULL;
}

// Execute the bound statement for a caller that wants the result set.  Rows are
// executed eagerly until the first result set appears; the rest are executed lazily
// as the consumer drains the stream.  If no row produces a result set at all this
// degrades to the ExecuteRows behaviour: an empty stream and a real row count.
static AdbcStatusCode BoundReaderInit(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                      int64_t* rows_affected, struct AdbcError* error) {
  struct BoundReader* r = calloc(1, sizeof(struct BoundReader));
  if (!r) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  r->ref = stmt->ref;
  stmt->ref->refcount++;
  r->opts = stmt->reader_opts;
  r->prepared = stmt->prepared;
  r->query = stmt->query ? strdup(stmt->query) : NULL;
  // Take ownership of the parameter stream: it is consumed lazily from here on.
  memcpy(&r->params, &stmt->bind_stream, sizeof(r->params));
  memset(&stmt->bind_stream, 0, sizeof(stmt->bind_stream));
  stmt->has_bind = false;

  AdbcStatusCode status = ADBC_STATUS_OK;
  if (r->params.get_schema(&r->params, &r->param_schema) != 0) {
    InternalAdbcSetError(error, "Bound stream get_schema failed: %s",
                         r->params.get_last_error(&r->params));
    status = ADBC_STATUS_INVALID_ARGUMENT;
  }
  struct ArrowError na_error;
  if (status == ADBC_STATUS_OK) {
    r->ncols = r->param_schema.n_children;
    r->svs = calloc((size_t)(r->ncols > 0 ? r->ncols : 1), sizeof(*r->svs));
    r->slots = calloc((size_t)(r->ncols > 0 ? r->ncols : 1), sizeof(*r->slots));
    if (!r->svs || !r->slots) {
      InternalAdbcSetError(error, "out of memory");
      status = ADBC_STATUS_INTERNAL;
    }
  }
  if (status == ADBC_STATUS_OK) {
    for (int64_t i = 0; i < r->ncols; i++) ArrowBufferInit(&r->slots[i].wbuf);
    if (ArrowArrayViewInitFromSchema(&r->view, &r->param_schema, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Invalid bound schema: %s", na_error.message);
      status = ADBC_STATUS_INVALID_ARGUMENT;
    } else {
      r->view_ready = true;
    }
  }
  for (int64_t i = 0; status == ADBC_STATUS_OK && i < r->ncols; i++) {
    if (ArrowSchemaViewInit(&r->svs[i], r->param_schema.children[i], &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Invalid bound schema: %s", na_error.message);
      status = ADBC_STATUS_INVALID_ARGUMENT;
    }
  }

  // Execute rows until one returns a result set.
  int64_t total = 0;
  bool exhausted = false;
  while (status == ADBC_STATUS_OK) {
    SQLSMALLINT nres = 0;
    status = BoundReaderExecuteNextRow(r, &exhausted, &nres, error);
    if (status != ADBC_STATUS_OK || exhausted) break;
    if (nres > 0) {
      status = OdbcReaderInit(r->ref, &r->opts, &r->inner, error);
      if (status == ADBC_STATUS_OK && r->inner.get_schema(&r->inner, &r->schema) != 0) {
        InternalAdbcSetError(error, "Failed to read result schema");
        status = ADBC_STATUS_INTERNAL;
      }
      break;
    }
    SQLLEN count = 0;
    if (SQL_SUCCEEDED(SQLRowCount(r->ref->hstmt, &count)) && count > 0) total += count;
  }
  if (status != ADBC_STATUS_OK) {
    BoundReaderFree(r);
    return status;
  }
  if (!r->schema.release) {
    // Nothing in the batch produced a result set; report the row count instead.
    BoundReaderFree(r);
    if (rows_affected) *rows_affected = total;
    struct ArrowSchema empty;
    ArrowSchemaInit(&empty);
    CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&empty, 0), error);
    CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &empty, 0), error);
    return ADBC_STATUS_OK;
  }
  if (rows_affected) *rows_affected = -1;
  out->private_data = r;
  out->get_schema = BoundReaderGetSchema;
  out->get_next = BoundReaderGetNext;
  out->get_last_error = BoundReaderGetLastError;
  out->release = BoundReaderRelease;
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error);

AdbcStatusCode OdbcStatementExecuteBound(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                         int64_t* rows_affected, struct AdbcError* error) {
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  if (!stmt->prepared) {
    ODBC_CHECK(SQLPrepare(stmt->ref->hstmt, (SQLCHAR*)stmt->query, SQL_NTS), SQL_HANDLE_STMT,
               stmt->ref->hstmt, "SQLPrepare", error);
    stmt->prepared = true;
  }
  if (out) return BoundReaderInit(stmt, out, rows_affected, error);
  return ExecuteRows(stmt, rows_affected, error);
}

// ---------------------------------------------------------------------------
// Bulk ingest

// Parameters to apply to a type name from SQLGetTypeInfo, e.g. VARCHAR(`length`),
// DECIMAL(`precision`,`scale`), TIME(`frac_digits`).  Only the ones the driver says the
// type takes (its CREATE_PARAMS) are used.
struct TypeParams {
  int64_t length;
  int32_t precision;
  int32_t scale;
  // Fractional-second digits for a datetime type; 0 means "no fractional seconds".
  int frac_digits;
};

// Ask the driver for its name of one SQL type via SQLGetTypeInfo. Returns false if the
// driver has no such type -- or if ansi_ddl_type_names says the driver's names are not
// the ones the server accepts, in which case every candidate is refused and the caller
// falls through to its portable SQL fallback name.
static bool TypeNameOne(SQLHDBC hdbc, const struct OdbcReaderOptions* opts, SQLSMALLINT sql_type,
                        const struct TypeParams* tp, bool q, char* out, size_t out_size) {
  SQLHSTMT hstmt = NULL;
  bool done = false;
  if (opts->ansi_ddl_type_names) return false;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  if (SQL_SUCCEEDED(SQLGetTypeInfo(hstmt, sql_type)) && SQL_SUCCEEDED(SQLFetch(hstmt))) {
    char name[256] = {0}, params[256] = {0};
    SQLLEN ind1 = 0, ind2 = 0;
    OdbcGetData(hstmt, 1, SQL_C_CHAR, name, sizeof(name), &ind1, q);
    OdbcGetData(hstmt, 6, SQL_C_CHAR, params, sizeof(params), &ind2, q);  // CREATE_PARAMS
    if (ind1 > 0) {
      // Some drivers return names with embedded parameters, e.g. "NUMBER(19,0)" or
      // "decimal(p,s)"; strip anything from '(' so we can apply our own parameters.
      char* paren = strchr(name, '(');
      if (paren) *paren = '\0';
      bool has_params = ind2 > 0;
      // A datetime type whose only CREATE_PARAMS entry is the fractional-seconds
      // precision (msodbcsql reports "scale" for time/datetime2, clickhouse-odbc for
      // DateTime64).  Without it the type is created at its default scale, which is
      // whole seconds on several backends, and sub-second parameters are silently
      // truncated (or, on ClickHouse, stored as NULL).
      if (has_params && tp->frac_digits > 0 && !strchr(params, ',') &&
          (strstr(params, "scale") || strstr(params, "precision"))) {
        int digits = tp->frac_digits;
        SQLSMALLINT max_scale = 0;
        SQLLEN ind3 = 0;  // MAXIMUM_SCALE; not every driver can return it
        if (SQL_SUCCEEDED(OdbcGetData(hstmt, 15, SQL_C_SSHORT, &max_scale, 0, &ind3, q)) &&
            ind3 != SQL_NULL_DATA && max_scale > 0 && digits > (int)max_scale) {
          digits = (int)max_scale;
        }
        snprintf(out, out_size, "%s(%d)", name, digits);
      } else if (has_params && strstr(params, "precision") && tp->precision > 0) {
        snprintf(out, out_size, "%s(%d,%d)", name, tp->precision, tp->scale);
      } else if (has_params && strstr(params, "length") && tp->length > 0) {
        snprintf(out, out_size, "%s(%lld)", name, (long long)tp->length);
      } else {
        snprintf(out, out_size, "%s", name);
      }
      done = true;
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return done;
}

// Try a chain of candidate SQL types (e.g. BIGINT, then NUMERIC(19,0) for Oracle).
static void TypeNameFor(SQLHDBC hdbc, const struct OdbcReaderOptions* opts,
                        const SQLSMALLINT* candidates, int n, const struct TypeParams* tp, bool q,
                        const char* fallback, char* out, size_t out_size) {
  for (int i = 0; i < n; i++) {
    if (TypeNameOne(hdbc, opts, candidates[i], tp, q, out, out_size)) return;
  }
  snprintf(out, out_size, "%s", fallback);
}

static AdbcStatusCode ColumnTypeSql(SQLHDBC hdbc, const struct OdbcReaderOptions* opts,
                                    const struct ArrowSchemaView* sv, bool q, char* out,
                                    size_t out_size, struct AdbcError* error) {
  if (sv->type == NANOARROW_TYPE_DICTIONARY) {
    // The values are bound decoded, so the created column takes the
    // dictionary's value type rather than its index type.
    struct ArrowSchemaView dsv;
    struct ArrowError na_error;
    if (!sv->schema || !sv->schema->dictionary ||
        ArrowSchemaViewInit(&dsv, sv->schema->dictionary, &na_error) != NANOARROW_OK ||
        dsv.type == NANOARROW_TYPE_DICTIONARY) {
      InternalAdbcSetError(error, "Dictionary-encoded column has no usable value type");
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    return ColumnTypeSql(hdbc, opts, &dsv, q, out, out_size, error);
  }
#define TYPES(...) ((const SQLSMALLINT[]){__VA_ARGS__})
#define CHAIN_P(params, fallback, ...)                                                     \
  TypeNameFor(hdbc, opts, TYPES(__VA_ARGS__),                                              \
              (int)(sizeof(TYPES(__VA_ARGS__)) / sizeof(SQLSMALLINT)), params, q, fallback, \
              out, out_size)
#define CHAIN(fallback, ...) CHAIN_P(&(const struct TypeParams){0}, fallback, __VA_ARGS__)
  switch (sv->type) {
    case NANOARROW_TYPE_BOOL: CHAIN("BOOLEAN", SQL_BIT, SQL_TINYINT, SQL_SMALLINT); break;
    case NANOARROW_TYPE_INT8: case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_INT16: CHAIN("SMALLINT", SQL_SMALLINT, SQL_INTEGER); break;
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_INT32: CHAIN("INTEGER", SQL_INTEGER, SQL_BIGINT); break;
    case NANOARROW_TYPE_UINT32: case NANOARROW_TYPE_INT64: case NANOARROW_TYPE_UINT64:
      if (!TypeNameOne(hdbc, opts, SQL_BIGINT, &(const struct TypeParams){0}, q, out, out_size) &&
          !TypeNameOne(hdbc, opts, SQL_DECIMAL, &(const struct TypeParams){.precision = 19}, q, out,
                       out_size) &&
          !TypeNameOne(hdbc, opts, SQL_NUMERIC, &(const struct TypeParams){.precision = 19}, q, out,
                       out_size)) {
        snprintf(out, out_size, "BIGINT");
      }
      break;
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT: CHAIN("REAL", SQL_REAL, SQL_FLOAT, SQL_DOUBLE); break;
    case NANOARROW_TYPE_DOUBLE: CHAIN("DOUBLE PRECISION", SQL_DOUBLE, SQL_FLOAT); break;
    case NANOARROW_TYPE_STRING: case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_STRING_VIEW:
      CHAIN("TEXT", SQL_LONGVARCHAR, SQL_WLONGVARCHAR, SQL_VARCHAR); break;
    case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY: case NANOARROW_TYPE_FIXED_SIZE_BINARY:
    case NANOARROW_TYPE_BINARY_VIEW:
      CHAIN("BLOB", SQL_LONGVARBINARY, SQL_VARBINARY); break;
    case NANOARROW_TYPE_DATE32: CHAIN("DATE", SQL_TYPE_DATE, SQL_TYPE_TIMESTAMP); break;
    case NANOARROW_TYPE_TIME32: case NANOARROW_TYPE_TIME64: {
      const int digits = FractionalDigits(sv->time_unit);
      if (digits > 0 && opts->fractional_time_type_format) {
        int d = digits;
        if (opts->fractional_time_max_digits > 0 && d > opts->fractional_time_max_digits) {
          d = opts->fractional_time_max_digits;
        }
        snprintf(out, out_size, opts->fractional_time_type_format, d);
        break;
      }
      char fallback[32];
      if (digits > 0) {
        snprintf(fallback, sizeof(fallback), "TIME(%d)", digits);
      } else {
        snprintf(fallback, sizeof(fallback), "TIME");
      }
      CHAIN_P(&(const struct TypeParams){.frac_digits = digits}, fallback, SQL_TYPE_TIME,
              SQL_SS_TIME2);
      break;
    }
    case NANOARROW_TYPE_TIMESTAMP:
      CHAIN_P(&(const struct TypeParams){.frac_digits = FractionalDigits(sv->time_unit)},
              "TIMESTAMP", SQL_TYPE_TIMESTAMP);
      break;
    case NANOARROW_TYPE_DECIMAL128: case NANOARROW_TYPE_DECIMAL256: {
      const struct TypeParams dec = {.precision = sv->decimal_precision,
                                     .scale = sv->decimal_scale};
      if (!TypeNameOne(hdbc, opts, SQL_DECIMAL, &dec, q, out, out_size) &&
          !TypeNameOne(hdbc, opts, SQL_NUMERIC, &dec, q, out, out_size)) {
        snprintf(out, out_size, "DECIMAL(%d,%d)", sv->decimal_precision, sv->decimal_scale);
      }
      break;
    }
    default:
      InternalAdbcSetError(error, "Unsupported Arrow type for ingest: %s", ArrowTypeString(sv->type));
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }
#undef CHAIN
#undef CHAIN_P
#undef TYPES
  return ADBC_STATUS_OK;
}

static void AppendQualifiedName(struct InternalAdbcStringBuilder* sb, const char* q,
                                const char* catalog, const char* schema, const char* table) {
  if (catalog && *catalog) InternalAdbcStringBuilderAppend(sb, "%s%s%s.", q, catalog, q);
  if (schema && *schema) InternalAdbcStringBuilderAppend(sb, "%s%s%s.", q, schema, q);
  InternalAdbcStringBuilderAppend(sb, "%s%s%s", q, table, q);
}

static bool EqualsIgnoreCase(const char* a, const char* b) {
  for (;; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
    if (!x) return true;
  }
}

static SQLCHAR* IngestPat(const char* s) { return (s && *s) ? (SQLCHAR*)s : NULL; }
static SQLSMALLINT IngestPatLen(const char* s) { return (s && *s) ? SQL_NTS : 0; }

// SQL_ATTR_CURRENT_CATALOG, or NULL when the backend has no catalogs.
static char* IngestCurrentCatalog(struct OdbcConnection* conn) {
  SQLCHAR buf[1024] = {0};
  SQLINTEGER len = 0;
  if (!SQL_SUCCEEDED(
          SQLGetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, buf, sizeof(buf), &len)))
    return NULL;
  if (buf[0] == '\0') return NULL;
  return strdup((const char*)buf);
}

// Does the ingest target already exist?  SQLTables' table argument is a pattern,
// so the returned TABLE_NAME is compared exactly.  Best effort: a driver that
// cannot answer leaves *exists false, and the CREATE below reports the conflict
// through its own diagnostics instead.
static void IngestTableExists(struct OdbcConnection* conn, const char* catalog, const char* schema,
                              const char* table, bool* exists) {
  *exists = false;
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt))) return;
  if (SQL_SUCCEEDED(SQLTables(hstmt, IngestPat(catalog), IngestPatLen(catalog), IngestPat(schema),
                              IngestPatLen(schema), (SQLCHAR*)table, SQL_NTS, NULL, 0))) {
    char buf[512];
    SQLLEN ind = 0;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      if (SQL_SUCCEEDED(SQLGetData(hstmt, 3, SQL_C_CHAR, buf, sizeof(buf), &ind)) &&
          ind != SQL_NULL_DATA && EqualsIgnoreCase(buf, table)) {
        *exists = true;
        break;
      }
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
}

// create_append over an existing table can only append if every bound column is
// actually there.  When it is not, the table "already exists" as something other
// than what the caller asked to create, which is ADBC_STATUS_ALREADY_EXISTS --
// not the NOT_FOUND the backend would report for the unknown column at INSERT
// time.  Best effort: a driver that cannot list the columns yields OK and the
// INSERT surfaces whatever it surfaces.
static AdbcStatusCode IngestCheckAppendable(struct OdbcConnection* conn, const char* catalog,
                                            const char* schema, const char* table,
                                            const struct ArrowSchema* bound,
                                            struct AdbcError* error) {
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt))) return ADBC_STATUS_OK;
  char** names = NULL;
  size_t n = 0, cap = 0;
  if (SQL_SUCCEEDED(SQLColumns(hstmt, IngestPat(catalog), IngestPatLen(catalog), IngestPat(schema),
                               IngestPatLen(schema), (SQLCHAR*)table, SQL_NTS, NULL, 0))) {
    char buf[512];
    SQLLEN ind = 0;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      if (!SQL_SUCCEEDED(SQLGetData(hstmt, 4, SQL_C_CHAR, buf, sizeof(buf), &ind)) ||
          ind == SQL_NULL_DATA)
        continue;
      if (n == cap) {
        size_t next = cap ? cap * 2 : 16;
        char** p = realloc(names, next * sizeof(*p));
        if (!p) break;
        names = p;
        cap = next;
      }
      names[n] = strdup(buf);
      if (!names[n]) break;
      n++;
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);

  AdbcStatusCode status = ADBC_STATUS_OK;
  for (int64_t i = 0; n > 0 && i < bound->n_children; i++) {
    const char* want = bound->children[i]->name ? bound->children[i]->name : "";
    bool found = false;
    for (size_t j = 0; j < n; j++) {
      if (EqualsIgnoreCase(names[j], want)) { found = true; break; }
    }
    if (!found) {
      InternalAdbcSetError(error,
                           "Table \"%s\" already exists and has no column \"%s\"; "
                           "cannot append the bound data to it",
                           table, want);
      status = ADBC_STATUS_ALREADY_EXISTS;
      break;
    }
  }
  for (size_t j = 0; j < n; j++) free(names[j]);
  free(names);
  return status;
}

static AdbcStatusCode ExecSimple(struct OdbcConnection* conn, const char* sql, bool ignore_error,
                                 struct AdbcError* error) {
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS);
  AdbcStatusCode s = ADBC_STATUS_OK;
  if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA && !ignore_error) {
    s = OdbcSetError(SQL_HANDLE_STMT, hstmt, sql, error);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return s;
}

AdbcStatusCode OdbcStatementIngest(struct OdbcStatement* stmt, int64_t* rows_affected,
                                   struct AdbcError* error) {
  if (!stmt->has_bind) {
    InternalAdbcSetError(error, "Must bind data before bulk ingest");
    return ADBC_STATUS_INVALID_STATE;
  }
  struct OdbcConnection* conn = stmt->conn;
  const char* mode = stmt->ingest_mode ? stmt->ingest_mode : ADBC_INGEST_OPTION_MODE_CREATE;
  struct ArrowArrayStream* stream = &stmt->bind_stream;
  struct ArrowSchema schema;
  if (stream->get_schema(stream, &schema) != 0) {
    InternalAdbcSetError(error, "Bound stream get_schema failed: %s", stream->get_last_error(stream));
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  char q[8];
  OdbcQuoteChar(conn->hdbc, q);
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 256);
  AdbcStatusCode status = ADBC_STATUS_OK;

  bool do_create = strcmp(mode, ADBC_INGEST_OPTION_MODE_APPEND) != 0;
  if (do_create && stmt->ref) {
    // Release this statement's own ODBC handle before the DDL below: Firebird refuses a
    // metadata update while the connection still holds a cursor or prepared statement on
    // the table (the replace-mode DROP fails with -607 "object in use" and the following
    // CREATE then reports "table already exists"). The INSERT at the end allocates a
    // fresh handle anyway.
    OdbcHandleRefRelease(stmt->ref);
    stmt->ref = NULL;
    stmt->prepared = false;
  }
  bool create_append = strcmp(mode, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0;
  if (strcmp(mode, ADBC_INGEST_OPTION_MODE_REPLACE) == 0) {
    InternalAdbcStringBuilderAppend(&sb, "DROP TABLE ");
    AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
    ExecSimple(conn, sb.buffer, /*ignore_error=*/true, error);
    sb.size = 0;
  }
  // Catalog lookups below must be scoped to the database the CREATE targets, or a
  // same-named table in another database (MariaDB, SQL Server) answers for it.
  char* current_catalog = NULL;
  const char* probe_catalog = stmt->ingest_catalog;
  if (!probe_catalog || !*probe_catalog) {
    current_catalog = IngestCurrentCatalog(conn);
    probe_catalog = current_catalog;
  }

  if (do_create) {
    InternalAdbcStringBuilderAppend(&sb, "CREATE %sTABLE ", stmt->ingest_temporary ? "TEMPORARY " : "");
    AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
    InternalAdbcStringBuilderAppend(&sb, " (");
    struct ArrowError na_error;
    for (int64_t i = 0; i < schema.n_children && status == ADBC_STATUS_OK; i++) {
      struct ArrowSchemaView sv;
      if (ArrowSchemaViewInit(&sv, schema.children[i], &na_error) != NANOARROW_OK) {
        InternalAdbcSetError(error, "Bad schema: %s", na_error.message);
        status = ADBC_STATUS_INVALID_ARGUMENT;
        break;
      }
      char tname[300];
      status = ColumnTypeSql(conn->hdbc, &conn->reader_opts, &sv, stmt->reader_opts.sqllen_32bit,
                             tname, sizeof(tname), error);
      const char* name = schema.children[i]->name ? schema.children[i]->name : "";
      if (conn->reader_opts.nullable_type_format && (schema.children[i]->flags & ARROW_FLAG_NULLABLE)) {
        char wrapped[340];
        snprintf(wrapped, sizeof(wrapped), conn->reader_opts.nullable_type_format, tname);
        InternalAdbcStringBuilderAppend(&sb, "%s%s%s%s %s", i ? ", " : "", q, name, q, wrapped);
      } else {
        InternalAdbcStringBuilderAppend(&sb, "%s%s%s%s %s", i ? ", " : "", q, name, q, tname);
      }
    }
    InternalAdbcStringBuilderAppend(&sb, ")");
    if (status == ADBC_STATUS_OK) {
      status = ExecSimple(conn, sb.buffer, /*ignore_error=*/false, error);
      if (status != ADBC_STATUS_OK && status != ADBC_STATUS_ALREADY_EXISTS) {
        // OdbcSetError already maps SQLSTATE 42S01 and vendor "already exists"
        // messages.  For the backends that report neither, ask the catalog
        // directly rather than surfacing a generic failure -- but only after the
        // CREATE has actually failed, so a stale or over-broad catalog answer can
        // never make us skip DDL that was needed.
        bool exists = false;
        IngestTableExists(conn, probe_catalog, stmt->ingest_schema, stmt->ingest_table, &exists);
        if (exists) status = ADBC_STATUS_ALREADY_EXISTS;
      }
      if (create_append && status == ADBC_STATUS_ALREADY_EXISTS) {
        // create_append may append into what is already there -- but only if every
        // bound column exists.  Otherwise the table exists as something other than
        // what the caller asked to create, which stays ALREADY_EXISTS.
        status = IngestCheckAppendable(conn, probe_catalog, stmt->ingest_schema,
                                       stmt->ingest_table, &schema, error);
        // Recovered: drop the CREATE's diagnostic so we do not return a stale error.
        if (status == ADBC_STATUS_OK && error && error->release) error->release(error);
      }
    }
    sb.size = 0;
  }
  free(current_catalog);
  if (status != ADBC_STATUS_OK) {
    InternalAdbcStringBuilderReset(&sb);
    schema.release(&schema);
    return status;
  }

  // t ("a", "b") -- everything an INSERT repeats per row-group in the multi-row form.
  AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
  InternalAdbcStringBuilderAppend(&sb, " (");
  for (int64_t i = 0; i < schema.n_children; i++) {
    const char* name = schema.children[i]->name ? schema.children[i]->name : "";
    InternalAdbcStringBuilderAppend(&sb, "%s%s%s%s", i ? ", " : "", q, name, q);
  }
  InternalAdbcStringBuilderAppend(&sb, ")");
  free(stmt->ingest_into);
  stmt->ingest_into = sb.buffer ? strdup(sb.buffer) : NULL;

  // INSERT INTO t ("a", "b") VALUES (?, ?)
  sb.size = 0;
  InternalAdbcStringBuilderAppend(&sb, "INSERT INTO %s VALUES (",
                                  stmt->ingest_into ? stmt->ingest_into : "");
  for (int64_t i = 0; i < schema.n_children; i++) InternalAdbcStringBuilderAppend(&sb, i ? ", ?" : "?");
  InternalAdbcStringBuilderAppend(&sb, ")");
  schema.release(&schema);

  free(stmt->query);
  stmt->query = strdup(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  stmt->prepared = false;
  AdbcStatusCode ingest_status = OdbcStatementExecuteBound(stmt, NULL, rows_affected, error);
  // The multi-row rewrite is scoped to this one ingest: a later ExecuteQuery on the same
  // statement must never see it.
  free(stmt->ingest_into);
  stmt->ingest_into = NULL;
  return ingest_status;
}
