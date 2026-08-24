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

#if !defined(_WIN32)
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#define ADBC_ODBC_HAVE_PREFETCH 1
#endif

#if defined(_WIN32)
// strndup is POSIX and absent from the MSVC CRT.  Without a declaration the call
// would still compile under C's implicit-int rule and truncate the pointer, so
// give Windows a real one.
static char* strndup(const char* s, size_t n) {
  size_t len = 0;
  while (len < n && s[len]) len++;
  char* out = malloc(len + 1);
  if (!out) return NULL;
  memcpy(out, s, len);
  out[len] = '\0';
  return out;
}
#endif

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
  // Pre-filled with all-ones, which reads as -1 at either SQLLEN width: a driver
  // that returns success without writing the out-parameter then says "unknown"
  // rather than "no rows".  psqlodbc does exactly that for a statement executed
  // inside an explicit transaction against a server that sends no row count with
  // its command tag (CrateDB), and a silent 0 there is indistinguishable from a
  // statement that really affected nothing.
  SQLLEN count;
  memset(&count, 0xff, sizeof(count));
  if (!SQL_SUCCEEDED(SQLRowCount(hstmt, &count))) return -1;
  return OdbcReadLen(&count, sqllen_32bit);
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

// Case-insensitive substring search over a possibly non-NUL-terminated buffer.
// `needle` must already be lower case.  Returns SIZE_MAX when there is no match.
static size_t HaystackFind(const char* hay, size_t hay_len, const char* needle) {
  size_t nlen = strlen(needle);
  if (nlen == 0 || hay_len < nlen) return SIZE_MAX;
  for (size_t i = 0; i + nlen <= hay_len; i++) {
    size_t j = 0;
    while (j < nlen) {
      char a = hay[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (a != b) break;
      j++;
    }
    if (j == nlen) return i;
  }
  return SIZE_MAX;
}

static bool HaystackContains(const char* hay, size_t hay_len, const char* needle) {
  return HaystackFind(hay, hay_len, needle) != SIZE_MAX;
}

// The one diagnostic unixODBC has for a driver library that would not load.
static const char kCantOpenLib[] = "can't open lib '";

// unixODBC loads a driver library through libltdl, which tries a list of
// candidate names and, when none of them opens, reports "file not found" -- the
// dlerror() that says *why* the loader refused the file is thrown away before
// the driver manager ever sees it.  So a driver that is on disk and readable is
// reported as missing, which sends people looking for the wrong problem.
//
// Only reached once a connect attempt has already failed with that message.
// Open the same file here and say what the dynamic loader actually said.
static void OdbcExplainLoadFailure(struct InternalAdbcStringBuilder* sb, const char* path) {
#if defined(_WIN32)
  // The Windows driver manager surfaces the real LoadLibrary error itself, and
  // the static-TLS exhaustion below is a glibc/ELF condition.
  (void)sb;
  (void)path;
#else
  if (access(path, R_OK) != 0) {
    // The file really is missing or unreadable: say which, and stop.
    InternalAdbcStringBuilderAppend(sb, "\n  [adbcbridge] %s: %s", path, strerror(errno));
    return;
  }
  // libltdl opens with RTLD_LAZY; match it so the outcome is comparable.
  void* handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
  if (handle) {
    dlclose(handle);
    InternalAdbcStringBuilderAppend(
        sb,
        "\n  [adbcbridge] that file exists and dlopen()s here, so the driver manager refused "
        "it for another reason (it calls every load failure \"file not found\"): check its "
        "word size, and that odbcinst.ini names the library itself");
    return;
  }
  const char* err = dlerror();
  if (!err) err = "(no reason given)";
  // macOS's dyld lists every path it tried -- the file itself, then a
  // /System/Volumes/Preboot/Cryptexes mirror, then the file again -- each with
  // its own reason, and the list can outrun the 1 KiB the error message holds.
  // The first entry is the file the caller named and carries the reason that
  // matters ("incompatible architecture", "slice is not valid mach-o file"), so
  // keep that entry and drop the rest of the list.
  char* trimmed = NULL;
  const char* tail = strstr(err, "), '");
  if (tail) {
    size_t keep = (size_t)(tail - err) + 1;  // through the closing parenthesis
    trimmed = malloc(keep + 1);
    if (trimmed) {
      memcpy(trimmed, err, keep);
      trimmed[keep] = '\0';
      err = trimmed;
    }
  }
  InternalAdbcStringBuilderAppend(
      sb,
      "\n  [adbcbridge] the file is there and readable -- the driver manager says \"file not "
      "found\" for any load failure.  dlopen(): %s",
      err);
  free(trimmed);
  if (strstr(err, "static TLS")) {
    InternalAdbcStringBuilderAppend(
        sb,
        "\n  [adbcbridge] that library was pinned to dynamic TLS before this driver loaded -- "
        "importing pyarrow does that to libstdc++ -- and glibc cannot move it to static TLS "
        "afterwards.  Load the ODBC driver before pyarrow, or LD_PRELOAD it.  Raising "
        "glibc.rtld.optional_static_tls does not help.  See docs/TROUBLESHOOTING.md");
  }
#endif
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
  bool says_already_exists = false;
  char* unloadable = NULL;  // path from "Can't open lib '<path>'", if any
  while (SQL_SUCCEEDED(OdbcGetDiagRecUtf8(handle_type, handle, rec, sqlstate, &native, (char*)msg,
                                          sizeof(msg), &msg_len))) {
    // SQLGetDiagRec fills at most sizeof(msg)-1 bytes plus a NUL, but reports
    // the *untruncated* message length in msg_len.  Using that length against
    // `msg` reads past the end of the buffer, so a longer diagnostic (an MSSQL
    // RAISERROR, an Oracle error stack) is re-fetched into a buffer that fits.
    const char* text = (const char*)msg;
    size_t text_len = msg_len > 0 ? (size_t)msg_len : 0;
    char* heap = NULL;
    if (text_len > sizeof(msg) - 1) {
      if (text_len > 32000) text_len = 32000;  // SQLSMALLINT buffer length
      heap = calloc(text_len + 1, 1);
      SQLSMALLINT full_len = 0;
      if (heap && SQL_SUCCEEDED(OdbcGetDiagRecUtf8(handle_type, handle, rec, sqlstate, &native, heap,
                                                   (SQLSMALLINT)(text_len + 1), &full_len))) {
        text = heap;
        if (full_len > 0 && (size_t)full_len < text_len) text_len = (size_t)full_len;
      } else {
        // No second buffer: report only the bytes the first call really wrote.
        free(heap);
        heap = NULL;
        const char* nul = memchr(msg, '\0', sizeof(msg));
        text_len = nul ? (size_t)(nul - (const char*)msg) : sizeof(msg) - 1;
      }
    }
    if (HaystackContains(text, text_len, "already exists")) {
      says_already_exists = true;
    }
    if (!unloadable) {
      size_t at = HaystackFind(text, text_len, kCantOpenLib);
      if (at != SIZE_MAX) {
        size_t start = at + sizeof(kCantOpenLib) - 1;
        size_t end = start;
        while (end < text_len && text[end] != '\'') end++;
        if (end > start && end < text_len) {
          unloadable = malloc(end - start + 1);
          if (unloadable) {
            memcpy(unloadable, text + start, end - start);
            unloadable[end - start] = '\0';
          }
        }
      }
    }
    if (first) {
      status = SqlStateToStatus((const char*)sqlstate);
      if (error) {
        memcpy(error->sqlstate, sqlstate, 5);
        error->vendor_code = (int32_t)native;
      }
      first = false;
    }
    InternalAdbcStringBuilderAppend(&sb, "\n  [%s] (%d) %.*s", (const char*)sqlstate,
                                    (int)native, (int)text_len, text);
    if (error) {
      InternalAdbcAppendErrorDetail(error, "odbc.sqlstate", (const uint8_t*)sqlstate, 5);
    }
    free(heap);
    rec++;
  }
  // Not every backend reports "object already exists" with SQLSTATE 42S01:
  // SQLiteODBC uses HY000 with "table X already exists", and other drivers pass
  // a vendor message through unmapped.  Fall back to the message text when the
  // SQLSTATE carried no usable meaning.
  if (status == ADBC_STATUS_UNKNOWN && says_already_exists) status = ADBC_STATUS_ALREADY_EXISTS;
  if (unloadable) {
    OdbcExplainLoadFailure(&sb, unloadable);
    free(unloadable);
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
  FETCH_TIME64,   // SQL_C_CHAR "HH:MM:SS[.frac]" -> time64[us|ns]
  FETCH_TIMESTAMP,// TIMESTAMP_STRUCT -> timestamp[s|ms|us|ns]
  FETCH_TIMESTAMP_TZ,  // SQL_C_CHAR ISO-8601 with offset -> timestamp[us, UTC]
  FETCH_TIMESTAMP_TEXT,// SQL_C_CHAR "YYYY-MM-DD hh:mm:ss[.frac]" -> timestamp[us]
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
  // The bound width is narrower than what the driver said the column could hold, so a
  // value may come back truncated and need re-reading.  Only such a column can force
  // the repair path -- which is why prefetch, whose fetch thread has already moved the
  // cursor past the rowset being converted, engages only when no column is clipped.
  bool clipped;
  // Bytes that had to be re-read because the value outgrew the bound buffer, counted
  // over the adaptation window only.  See AdaptBindWidth().
  int64_t trunc_bytes;
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
  // hstmt is NULL when classifying a *parameter*, which has no IRD row to query.
  if (!hstmt) return false;
  if (!SQL_SUCCEEDED(OdbcColAttributeStrUtf8(hstmt, col, SQL_DESC_TYPE_NAME, (char*)name, sizeof(name), &len))) {
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
  if (!SQL_SUCCEEDED(OdbcColAttributeStrUtf8(hstmt, col, SQL_DESC_TYPE_NAME, (char*)name, sizeof(name),
                                             &len))) {
    return false;
  }
  name[sizeof(name) - 1] = '\0';
  return ContainsFold((const char*)name, "with time zone") ||
         ContainsFold((const char*)name, "timestamptz") ||
         ContainsFold((const char*)name, "timestampoffset");
}

// Can a value that comes back longer than its bound buffer still be read in full?
// Either SQLGetData can re-read it where it sits (getdata_repair) or the rowset it
// belongs to can be fetched again one row at a time (refetch_repair).  When neither
// holds, a column whose declared width is a type maximum rather than a real bound has
// to stay unbound, because a bound buffer would silently clip it.
static inline bool TruncationRepairable(const struct OdbcReaderOptions* opts) {
  // Both routes end in SQLGetData on a bound column, which needs SQL_GD_BOUND.  The
  // refetch route also has to drop SQL_ATTR_ROW_ARRAY_SIZE to 1 and put it back, which a
  // fixed-rowset driver cannot survive (see OdbcReaderOptions::fixed_rowset), so there it
  // is not a route at all.
  return opts->getdata_bound &&
         (opts->getdata_repair || (opts->refetch_repair && !opts->fixed_rowset));
}

// Settle how a variable-length column is read, given the width `ClassifyColumn` derived
// from what the driver said about it.
//
// Drivers routinely describe a text or binary column by what its *type* could hold
// rather than by anything the table holds: sqliteodbc says 65,536 characters for every
// TEXT column, MySQL says 16,777,215, SQL Server says 2,147,483,647 for NVARCHAR(MAX).
// Binding that literally is out of the question -- the rowset alone would be hundreds of
// megabytes, and a driver that null-fills a bound buffer writes every byte of it on every
// row (sqliteodbc reads a 100,000-row table 4x slower bound at 256 KiB than at 4 KiB) --
// so such a column is bound at `long_bind_bytes` instead and the few values that overflow
// that are re-read in full.  Re-reading needs a driver that can go back to a row (see
// TruncationRepairable); without one the column stays unbound, which costs the whole
// result set its block cursor because SQLGetData needs a one-row rowset.
// IBM's CLI driver (clidriver's libdb2, which speaks DRDA to Db2 and to Informix) does
// not describe a large-object byte column as SQL_LONGVARBINARY: it reports its own type
// code, SQL_BLOB from sqlcli1.h, which no other driver uses.  Left unrecognised the
// column falls through to the reader's text default, where the driver hands the bytes
// back hex-encoded ("0102" for b"\x01\x02") instead of as bytes.
#define ODBC_SQL_BLOB_IBM (-98)

static void ApplyBindWidth(struct OdbcColumn* c, const struct OdbcReaderOptions* opts) {
  // SQL_LONGVARCHAR / SQL_WLONGVARCHAR / SQL_LONGVARBINARY name a type with no length at
  // all -- whatever width the driver reports for one is a guess, so binding it is only
  // safe where a value that outgrows the buffer can be read again.
  const bool no_declared_length = c->sql_type == SQL_LONGVARCHAR ||
                                  c->sql_type == SQL_WLONGVARCHAR ||
                                  c->sql_type == SQL_LONGVARBINARY ||
                                  c->sql_type == ODBC_SQL_BLOB_IBM;
  const bool repairable = TruncationRepairable(opts) && opts->long_bind_bytes > 0;
  if (c->column_size == 0) {
    // No width to bind against.  For one of the no-length types that is not a different
    // situation from the width such a driver would otherwise have invented: psqlodbc
    // describes PostgreSQL's `bytea` as SQL_LONGVARBINARY of size 0 however long its
    // values are, exactly as it describes `text` as SQL_LONGVARCHAR of size 8190.  Where
    // a too-long value can be re-read, bind it at `long_bind_bytes` like any other
    // guessed width instead of giving up the block cursor for the whole result set -- a
    // 500,000-row read of (int4, text, varchar, numeric, bool, timestamp, bytea) out of
    // PostgreSQL goes from 0.633 s to 0.530 s that way, and (int4, bytea) from 0.182 s
    // to 0.139 s.  A type that does have a declared
    // length and still comes back as size 0 (MatrixOne reports octet length 0 for
    // int/numeric/timestamp) is a driver saying nothing useful at all, and stays unbound.
    if (!no_declared_length || !repairable) {
      c->bound = false;
      return;
    }
  } else if (no_declared_length && !repairable) {
    c->bound = false;
    return;
  }
  // A guess can be too small as well as too large: MatrixOne describes a TEXT column as
  // five characters (and octet length 0) however long its values are, so binding what it
  // says would truncate -- and re-read -- every single row, which is slower than not
  // binding at all (a 100,000-row read runs at 3k rows/s that way, 900k bound wide).
  // Since the width of such a column is a guess either way, bind it at the same
  // `long_bind_bytes` an over-large guess is clamped to; the values that outgrow that are
  // re-read exactly as before.
  if (no_declared_length) c->clipped = true;  // the width was a guess either way
  if (no_declared_length && repairable && c->elem_size < opts->long_bind_bytes) {
    c->elem_size = opts->long_bind_bytes;
  }
  if (c->elem_size > opts->max_bind_bytes) {
    if (!repairable) {
      c->bound = false;
      return;
    }
    c->elem_size = opts->long_bind_bytes;
    c->clipped = true;
  }
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

// The Arrow unit implied by what SQLDescribeCol says about a timestamp column:
// TIMESTAMP(0) is whole seconds, TIMESTAMP(3) milliseconds, and so on.
//
// `decimal_digits` is the column's fractional-seconds precision and is
// authoritative whenever a driver reports one.  Several drivers always report 0
// there, so a reported 0 is cross-checked against the column size, which ODBC
// defines as 19 for a timestamp with no fractional seconds and 20 + precision
// otherwise: Oracle's SQORA reports scale 0 with size 20/23/26/29 for
// TIMESTAMP(0)/(3)/(6)/(9) and size 19 for DATE, and clickhouse-odbc reports
// size 19 for DateTime and 29 for every DateTime64.
//
// When neither answers -- or the driver reports scale 0 with the no-fraction size,
// which MySQL Connector/ODBC does for DATETIME(6) -- microseconds remain the
// default: SQLiteODBC reports
// scale 0 and size 32 for every TIMESTAMP column whatever its declared
// precision, and truncating those to whole seconds would throw away the
// milliseconds it does store.
static enum ArrowTimeUnit TimestampUnitForColumn(SQLSMALLINT decimal_digits,
                                                 SQLULEN column_size) {
  int digits = decimal_digits;
  if (digits <= 0) {
    if (column_size > 20 && column_size <= 29) {
      digits = (int)column_size - 20;
    } else {
      // Scale 0 with the plain "no fraction" size is not trustworthy either: MySQL
      // Connector/ODBC reports 0 / 19 for DATETIME(6).  Microseconds lose nothing;
      // whole seconds would.
      return NANOARROW_TIME_UNIT_MICRO;
    }
  }
  if (digits <= 0) return NANOARROW_TIME_UNIT_SECOND;
  if (digits <= 3) return NANOARROW_TIME_UNIT_MILLI;
  if (digits <= 6) return NANOARROW_TIME_UNIT_MICRO;
  return NANOARROW_TIME_UNIT_NANO;
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
      // seconds has to come across as text.  The reported scale picks the
      // Arrow unit: 0 -> time32[s], 1-6 -> time64[us], 7-9 -> time64[ns].
      if (c->decimal_digits > 0) {
        c->kind = FETCH_TIME64;
        // time64 has no second or millisecond unit, so anything under a second
        // rounds up to microseconds.
        c->unit = c->decimal_digits > 6 ? NANOARROW_TIME_UNIT_NANO : NANOARROW_TIME_UNIT_MICRO;
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
      if (opts->timestamp_as_text) {
        // The driver has no TIMESTAMP_STRUCT for this column (see timestamp_as_text):
        // take its text form, which is the same ISO-8601 the tz case parses, minus the
        // offset -- so the value stays local and the Arrow type carries no timezone.
        c->kind = FETCH_TIMESTAMP_TEXT;
        c->unit = NANOARROW_TIME_UNIT_MICRO;
        UseTextBuffer(c, opts, 80);
        break;
      }
      c->kind = FETCH_TIMESTAMP; c->c_type = SQL_C_TYPE_TIMESTAMP;
      c->elem_size = sizeof(TIMESTAMP_STRUCT);
      c->unit = TimestampUnitForColumn(c->decimal_digits, c->column_size);
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
    case ODBC_SQL_BLOB_IBM:
      c->kind = FETCH_BINARY; c->c_type = SQL_C_BINARY;
      c->elem_size = (SQLLEN)c->column_size;
      ApplyBindWidth(c, opts);
      break;
#if defined(_WIN32)
    // The Windows driver manager transcodes SQL_C_CHAR data to the process's ANSI code
    // page -- "héllo" becomes bytes that are not UTF-8, anything outside the code page
    // becomes '?' -- so on Windows every character column is read as SQL_C_WCHAR and
    // converted here, whatever the driver calls it.  unixODBC and iODBC pass narrow
    // bytes through untouched, which is what the SQL_C_CHAR path below relies on.
    // (First seen on psqlodbc against a UTF8 database on Windows 11.)
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    default:
#endif
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      if (opts->wchar_as_utf8) {  // see OdbcReaderOptions::wchar_as_utf8 (never on Windows)
        c->kind = FETCH_CHAR; c->c_type = SQL_C_CHAR;
        c->elem_size = (SQLLEN)c->column_size * 4 + 1;
        ApplyBindWidth(c, opts);
        break;
      }
      c->kind = FETCH_WCHAR; c->c_type = SQL_C_WCHAR;
      c->elem_size = ((SQLLEN)c->column_size + 1) * (SQLLEN)sizeof(SQLWCHAR);
      ApplyBindWidth(c, opts);
      // A capped SQL_C_WCHAR buffer still has to be a whole number of UTF-16 code units.
      c->elem_size -= c->elem_size % (SQLLEN)sizeof(SQLWCHAR);
      break;
#if !defined(_WIN32)
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    default:
      // Anything unknown: ask the driver for a string representation.
      c->kind = FETCH_CHAR; c->c_type = SQL_C_CHAR;
      // column_size is in characters; UTF-8 may need up to 4 bytes each.
      c->elem_size = (SQLLEN)c->column_size * 4 + 1;
      ApplyBindWidth(c, opts);
      break;
#endif
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
    SQLRETURN ret = OdbcDescribeColUtf8(hstmt, (SQLUSMALLINT)(i + 1), (char*)name, sizeof(name),
                                        &name_len, &c->sql_type, &c->column_size,
                                        &c->decimal_digits, &c->nullable);
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
                 ArrowSchemaSetTypeDateTime(f, NANOARROW_TYPE_TIME64, c->unit, NULL),
                 error);
        goto named;
      case FETCH_TIMESTAMP:
      case FETCH_TIMESTAMP_TEXT:
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

AdbcStatusCode OdbcDescribeParameterSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                           struct ArrowSchema* out, struct AdbcError* error) {
  SQLSMALLINT n = 0;
  ODBC_CHECK(SQLNumParams(hstmt, &n), SQL_HANDLE_STMT, hstmt, "SQLNumParams", error);
  if (n < 0) n = 0;
  struct OdbcColumn* cols = calloc((size_t)(n > 0 ? n : 1), sizeof(struct OdbcColumn));
  if (!cols) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  // ADBC names parameter fields positionally; match the SQLite driver's "0".."N-1".
  bool described = true;
  for (SQLSMALLINT i = 0; i < n; i++) {
    struct OdbcColumn* c = &cols[i];
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)i);
    c->name = strdup(buf);
    if (!c->name) {
      FreeColumns(cols, n);
      InternalAdbcSetError(error, "out of memory");
      return ADBC_STATUS_INTERNAL;
    }
    if (!described) continue;
    if (opts->no_describe_param) {
      described = false;
      continue;
    }
    SQLRETURN ret = SQLDescribeParam(hstmt, (SQLUSMALLINT)(i + 1), &c->sql_type, &c->column_size,
                                     &c->decimal_digits, &c->nullable);
    if (!SQL_SUCCEEDED(ret) || c->sql_type == SQL_UNKNOWN_TYPE) {
      // Plenty of ODBC drivers cannot describe parameters at all (SQLiteODBC among
      // them).  Fall back to the shape every ADBC caller can consume: N nullable
      // utf8 fields, which is what the upstream SQLite driver reports.
      described = false;
      continue;
    }
    ClassifyColumn(NULL, 0, c, opts);
  }
  if (!described) {
    for (SQLSMALLINT i = 0; i < n; i++) {
      cols[i].kind = FETCH_CHAR;
      cols[i].nullable = SQL_NULLABLE;
    }
  }
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

// Scan ".ffffff" (or ",ffffff") into exactly `want` fractional digits: extra
// digits are truncated, missing ones are right-padded with zeros.
static void ScanFractionDigits(const char* s, size_t len, size_t* pos, int want, int64_t* out) {
  *out = 0;
  if (*pos >= len || (s[*pos] != '.' && s[*pos] != ',')) return;
  (*pos)++;
  int digits = 0;
  while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') {
    if (digits < want) {
      *out = *out * 10 + (s[*pos] - '0');
      digits++;
    }
    (*pos)++;
  }
  while (digits < want) {
    *out *= 10;
    digits++;
  }
}

// Scan ".ffffff" (or ",ffffff") into microseconds, truncating extra digits.
static void ScanFraction(const char* s, size_t len, size_t* pos, int64_t* out_micros) {
  ScanFractionDigits(s, len, pos, 6, out_micros);
}

static void SkipBlanks(const char* s, size_t len, size_t* pos) {
  while (*pos < len && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\0')) (*pos)++;
}

// "HH:MM[:SS[.frac]]" -> time since midnight in units of 10^-`frac_digits`
// seconds (`frac_digits` is 6 for time64[us], 9 for time64[ns]).
static bool ParseTimeScaled(const char* s, size_t len, int frac_digits, int64_t* out) {
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
  ScanFractionDigits(s, len, &p, frac_digits, &frac);
  SkipBlanks(s, len, &p);
  if (p != len) return false;
  if (h > 23 || m > 59 || sec > 59) return false;
  int64_t scale = 1;
  for (int i = 0; i < frac_digits; i++) scale *= 10;
  *out = ((h * 60 + m) * 60 + sec) * scale + frac;
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

// Transcode a UTF-16 buffer (n units) into `o`, returning the number of bytes
// written.  Surrogate pairs become 4-byte sequences (non-BMP characters such as
// emoji); an unpaired surrogate becomes U+FFFD so the output is always valid
// UTF-8.  `o` must have room for `Utf16Utf8MaxBytes(n)` bytes.
static size_t Utf16ToUtf8(const SQLWCHAR* w, size_t n, uint8_t* o) {
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
  return k;
}

// Worst case UTF-8 expansion of n UTF-16 code units: a BMP unit (2 bytes) can
// reach 3 bytes, a surrogate pair (4 bytes) stays 4, so never more than 3n.
static inline int64_t Utf16Utf8MaxBytes(int64_t units) { return units * 3; }

// Append a UTF-16 buffer (n units) to a utf8 string array, via `scratch`.
static ArrowErrorCode AppendUtf16(struct ArrowArray* arr, const SQLWCHAR* w, size_t n,
                                  struct ArrowBuffer* scratch) {
  scratch->size_bytes = 0;
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(scratch, (int64_t)n * 3 + 1));
  size_t k = Utf16ToUtf8(w, n, scratch->data);
  struct ArrowStringView sv = {(const char*)scratch->data, (int64_t)k};
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

// --- Prefetch ------------------------------------------------------------------------
//
// SQLFetch blocks on the socket while the CPU does nothing, and the conversion into
// Arrow then runs while the socket does nothing.  Those two are the whole read path, and
// they are disjoint, so overlapping them is worth up to the smaller of the two.
//
// The mechanism is a small ring of *rowset slots*, each a complete set of bound buffers
// and indicator arrays.  A background thread owns the ODBC handle and does nothing but
// rebind onto the next free slot and SQLFetch into it; the calling thread pops filled
// slots and converts them.  Nothing about the conversion changes: `OdbcColumn::buffer`
// and `::indicators` are repointed at the slot the caller currently owns, so
// BulkAppendColumn() and AppendValue() read exactly the memory they always did.
//
// Two invariants keep this safe against an ODBC driver that is not thread-safe:
//
//  1. The statement handle is touched by *exactly one* thread at a time.  The fetch
//     thread owns it from the moment it starts until it is joined; the calling thread
//     touches it only before starting the thread and after joining it, and both
//     transfers are through pthread_create/pthread_join, which are full barriers.  No
//     ODBC call is ever concurrent with another on the same handle.
//  2. It engages only when every column is bound at a width that cannot truncate (see
//     OdbcColumn::clipped).  The repair paths -- SQLGetData and SQLFetchScroll on an
//     earlier row -- are ODBC calls issued *during* conversion, and the fetch thread has
//     by then moved the cursor on.  A clipped column therefore disables prefetch
//     outright rather than racing for the cursor.
//
// It is off by default (`adbc.odbc.prefetch`) because neither invariant can be checked
// against a driver's actual behaviour, only against what it declares.
struct OdbcRowsetSlot {
  void** buffers;             // ncols entries; NULL for an unbound column
  SQLLEN** indicators;        // ncols entries
  SQLUSMALLINT* row_status;
  SQLULEN rows_fetched_raw;   // what the driver wrote through SQL_ATTR_ROWS_FETCHED_PTR
  SQLULEN fetched;            // ... resolved to a row count
  int64_t first_row;          // 1-based position of this rowset's first row
  bool eos;                   // the fetch that filled this slot returned SQL_NO_DATA
  // A value in this rowset outgrew its bound buffer.  Repairing it means re-reading rows
  // the fetch thread has already scrolled past, so the fetch thread publishes the slot
  // with this set and then stops, handing the cursor back to the caller.
  bool needs_repair;
};

// Rows the bind-width decision below is taken on, and the rowset it takes them in.  The
// window is the whole cost of the decision -- every row of it that truncates is read
// twice -- so it is fetched in small rowsets to keep that cost off result sets that are
// themselves only a few thousand rows: 3,000 rows of 64 KiB `bytea` learn in 2 x 128
// rows (0.02 s of the 0.72 s read) where one 1,024-row rowset would have cost 0.09 s.
// The Arrow batches do not change size: the probe rowsets fill the first batch to
// `batch_size` between them, and the full rowset comes back for the batch after it.
#define ODBC_ADAPT_WINDOW_ROWS 256
#define ODBC_ADAPT_PROBE_ROWS 128

// Bytes a truncated value may cost the re-read for each row the block cursor keeps.
// See AdaptBindWidth() for where 256 comes from.
#define ODBC_ADAPT_REREAD_BUDGET 256

struct OdbcReader {
  struct OdbcHandleRef* ref;
  struct OdbcReaderOptions opts;
  struct OdbcColumn* cols;
  SQLSMALLINT ncols;
  struct ArrowSchema schema;
  SQLULEN rows_per_fetch;
  SQLULEN rows_fetched;
  // Rows SQLFetch has produced so far, so a rowset can name its rows to
  // SQLFetchScroll(SQL_FETCH_ABSOLUTE), which counts from 1.  A rowset that reported a
  // skipped or failed row breaks that correspondence, and gives up the repair path.
  int64_t rows_seen;
  bool rows_seen_exact;
  // Rowsets read, and how many of those had to be re-read because a value overflowed its
  // bound buffer.  When most of them do, the table's values are simply wider than
  // long_bind_bytes and repairing rowset after rowset costs more than never binding a
  // block cursor in the first place: see ReaderNextBatch().
  int64_t rowsets_read;
  int64_t rowsets_repaired;
  // A column bound at a guessed width could still be given up on: see AdaptBindWidth().
  // `adapt_rows` is how many rows that decision has been able to look at so far, and
  // while it is open the rowset is the small probe one -- `rowset_full` is the size the
  // next batch goes back to once the columns have kept their bindings.
  bool adapt_open;
  int64_t adapt_rows;
  bool rowset_restore;
  SQLULEN rowset_full;
  // Set when the driver rejected SQL_ATTR_ROWS_FETCHED_PTR and therefore never reports
  // how many rows SQLFetch produced; then each successful SQLFetch means exactly one row.
  bool no_rows_fetched_ptr;
  SQLUSMALLINT* row_status;
  bool done;
  bool bound;
  bool all_bound;  // every column is SQLBindCol'd => rowsets convert column-at-a-time
  struct ArrowBuffer scratch;  // for SQLGetData chunks / utf16 conversion
  struct AdbcError error;
  char error_message[1024];

  // --- prefetch ---
  struct OdbcRowsetSlot* slots;
  int nslots;          // 1 = no prefetch; otherwise prefetch depth + 1
  int cur_slot;        // the slot whose buffers cols[] currently point at
#ifdef ADBC_ODBC_HAVE_PREFETCH
  pthread_t fetch_thread;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool thread_started;
  bool prefetching;    // the fetch thread is the owner of the handle
  int ring_head;       // next slot the caller will pop
  int ring_tail;       // next slot the fetch thread will fill
  int ring_filled;
  bool fetch_done;     // producer reached the end of the result set
  bool fetch_failed;   // producer hit an ODBC error; see fetch_status/fetch_error
  bool fetch_stop;     // caller asked the producer to stop (release, or an error)
  AdbcStatusCode fetch_status;
  struct AdbcError fetch_error;
#endif
};

// Why this reader cannot prefetch, or NULL if it can.  See the prefetch commentary
// above OdbcRowsetSlot for what each condition protects.
static const char* PrefetchRefusalReason(const struct OdbcReader* r) {
#ifndef ADBC_ODBC_HAVE_PREFETCH
  (void)r;
  return "this platform has no thread support compiled in";
#else
  if (!r->all_bound) return "the result set has a column this driver cannot bind";
  if (r->rows_per_fetch <= 1) return "this driver fetches one row at a time";
  if (r->no_rows_fetched_ptr) return "this driver does not report how many rows it fetched";
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    // A clipped column can truncate, and repairing a truncated value means going back to
    // a row the fetch thread has already read past.
    if (r->cols[i].clipped) return "a column is bound narrower than its declared width";
  }
  return NULL;
#endif
}

// Point the columns at slot `sl`'s buffers.  Pure pointer assignment: every conversion
// path reads through OdbcColumn::buffer / ::indicators and needs no other notion of
// which rowset it is looking at.
static void ReaderUseSlot(struct OdbcReader* r, int sl) {
  struct OdbcRowsetSlot* slot = &r->slots[sl];
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    r->cols[i].buffer = slot->buffers[i];
    r->cols[i].indicators = slot->indicators[i];
  }
  r->row_status = slot->row_status;
  r->cur_slot = sl;
}

// Aim the ODBC handle's bound columns and status pointers at slot `sl`, so the next
// SQLFetch lands there.  Only ever called by whichever thread owns the handle.
static AdbcStatusCode ReaderBindSlot(struct OdbcReader* r, int sl, struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  struct OdbcRowsetSlot* slot = &r->slots[sl];
  SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_STATUS_PTR, slot->row_status, 0);
  if (!r->no_rows_fetched_ptr) {
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &slot->rows_fetched_raw, 0);
  }
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    if (!r->cols[i].bound) continue;
    ODBC_CHECK(SQLBindCol(hstmt, (SQLUSMALLINT)(i + 1), r->cols[i].c_type, slot->buffers[i],
                          r->cols[i].elem_size, slot->indicators[i]),
               SQL_HANDLE_STMT, hstmt, "SQLBindCol", error);
  }
  return ADBC_STATUS_OK;
}

#ifdef ADBC_ODBC_HAVE_PREFETCH
static AdbcStatusCode PrefetchStart(struct OdbcReader* r, struct AdbcError* error);
static AdbcStatusCode PrefetchNextRowset(struct OdbcReader* r, struct ArrowArray* batch,
                                         int64_t* total, struct AdbcError* error);
static void PrefetchJoin(struct OdbcReader* r);
#endif
static SQLULEN ResolveFetched(const struct OdbcReader* r, const struct OdbcRowsetSlot* slot);
static bool RowsetIsBulk(const struct OdbcReader* r, SQLULEN fetched);
static AdbcStatusCode ConvertRowset(struct OdbcReader* r, SQLULEN fetched, bool bulk,
                                    struct ArrowArray* batch, int64_t* total,
                                    struct AdbcError* error);

static AdbcStatusCode ReaderBind(struct OdbcReader* r, struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  bool all_bound = true;
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    if (!r->cols[i].bound) all_bound = false;
  }
  r->all_bound = all_bound;
  r->rows_per_fetch = all_bound ? (SQLULEN)r->opts.batch_size : 1;
  if (r->rows_per_fetch < 1) r->rows_per_fetch = 1;
  // Size the rowset by bytes as well as by rows.  A column the driver describes as
  // 65,536 characters wide costs 262,145 bytes per row all by itself, so batch_size rows
  // of it would be a quarter-gigabyte allocation that no cache holds; batch_size stays
  // the Arrow batch size and the rowset becomes however many rows fit in the budget.
  if (r->opts.rowset_bytes > 0 && r->rows_per_fetch > 1) {
    uint64_t row_bytes = 0;
    for (SQLSMALLINT i = 0; i < r->ncols; i++) {
      if (!r->cols[i].bound) continue;
      row_bytes += (uint64_t)r->cols[i].elem_size + sizeof(SQLLEN);
    }
    if (row_bytes > 0) {
      uint64_t fit = (uint64_t)r->opts.rowset_bytes / row_bytes;
      if (fit < 1) fit = 1;
      if (fit < (uint64_t)r->rows_per_fetch) r->rows_per_fetch = (SQLULEN)fit;
    }
  }
  if (r->opts.min_buffer_rows > 0 && all_bound) {
    // Round up to a multiple of the driver's internal chunk so rows stay aligned.
    SQLULEN m = (SQLULEN)r->opts.min_buffer_rows;
    r->rows_per_fetch = ((r->rows_per_fetch + m - 1) / m) * m;
  }
  SQLULEN capacity = r->rows_per_fetch;
  if (r->opts.min_buffer_rows > 0 && capacity < (SQLULEN)r->opts.min_buffer_rows) {
    capacity = (SQLULEN)r->opts.min_buffer_rows;
  }

  // Arm the width adaptation (AdaptBindWidth) if a column was bound at a guessed width
  // and giving that binding up could buy anything: the repair route has to be the
  // in-place one (getdata_repair; the others have their own remedy, see AdaptBindWidth)
  // and a one-row rowset has nothing to give up.  While the decision is open the rowset
  // is the small probe one, so that the rows it is taken on are few; the buffers are
  // still allocated for the full rowset the batch after it goes back to.
  // A driver whose rowset cannot move once the cursor is open cannot run the probe
  // either: the probe is a small rowset that goes back to the full one, and the remedy it
  // may reach for is a collapse to a single row.  Such a driver keeps whatever binding
  // ApplyBindWidth() chose, and repairs the values that outgrow it where they sit.
  r->rowset_full = r->rows_per_fetch;
  if (r->opts.getdata_repair && r->rows_per_fetch > 1 && !r->opts.fixed_rowset) {
    for (SQLSMALLINT i = 0; i < r->ncols; i++) {
      if (r->cols[i].bound && r->cols[i].clipped) r->adapt_open = true;
    }
  }
  if (r->adapt_open && r->opts.min_buffer_rows <= 0 &&
      r->rows_per_fetch > (SQLULEN)ODBC_ADAPT_PROBE_ROWS) {
    r->rows_per_fetch = (SQLULEN)ODBC_ADAPT_PROBE_ROWS;
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
  // MDB Tools rejects SQL_ATTR_ROWS_FETCHED_PTR, leaving rows_fetched at 0 forever, so
  // every result set read as empty. Without a row count we can only fetch one row per
  // SQLFetch, which is also all such a driver supports.  Probe it here, before any slot
  // exists; ReaderBindSlot() then points it at whichever slot is being fetched into.
  if (!SQL_SUCCEEDED(SQLSetStmtAttr(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &r->rows_fetched, 0))) {
    r->no_rows_fetched_ptr = true;
    r->rows_per_fetch = 1;
    SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0);
  }

  if (r->rows_per_fetch <= 1) {  // a driver that refused the block cursor outright
    r->adapt_open = false;
    r->rowset_full = 1;
  }

  // Decide how many rowset slots to allocate.  Prefetch asks for one set of bound
  // buffers per rowset in flight, and only engages where the whole rowset can be
  // converted out of memory once the fetch thread has moved on -- see the prefetch
  // commentary above OdbcRowsetSlot.
  r->nslots = 1;
  if (r->opts.prefetch > 0) {
    const char* why = PrefetchRefusalReason(r);
    if (!why) {
      int depth = (int)r->opts.prefetch;
      if (depth > ADBC_ODBC_MAX_PREFETCH) depth = ADBC_ODBC_MAX_PREFETCH;
      r->nslots = depth + 1;
    }
  }
  if (r->nslots > 1) r->adapt_open = false;  // prefetch refuses a clipped column anyway
  r->slots = calloc((size_t)r->nslots, sizeof(struct OdbcRowsetSlot));
  if (!r->slots) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  for (int sl = 0; sl < r->nslots; sl++) {
    struct OdbcRowsetSlot* slot = &r->slots[sl];
    slot->buffers = calloc((size_t)r->ncols ? (size_t)r->ncols : 1, sizeof(void*));
    slot->indicators = calloc((size_t)r->ncols ? (size_t)r->ncols : 1, sizeof(SQLLEN*));
    // calloc leaves the statuses at SQL_ROW_SUCCESS, which is what a driver that ignores
    // SQL_ATTR_ROW_STATUS_PTR (MDB Tools) implies for the row it just fetched.
    slot->row_status = calloc(capacity, sizeof(SQLUSMALLINT));
    if (!slot->buffers || !slot->indicators || !slot->row_status) {
      InternalAdbcSetError(error, "out of memory");
      return ADBC_STATUS_INTERNAL;
    }
    for (SQLSMALLINT i = 0; i < r->ncols; i++) {
      // A 32-bit-SQLLEN driver fills this as int32[capacity] (stride 4), which fits
      // inside the same allocation; OdbcIndicatorGet() reads it back with the right
      // stride.
      slot->indicators[i] = calloc(capacity, sizeof(SQLLEN));
      if (!slot->indicators[i]) {
        InternalAdbcSetError(error, "out of memory");
        return ADBC_STATUS_INTERNAL;
      }
      if (!r->cols[i].bound) continue;
      slot->buffers[i] = calloc(capacity, (size_t)r->cols[i].elem_size);
      if (!slot->buffers[i]) {
        InternalAdbcSetError(error, "out of memory binding column %s", r->cols[i].name);
        return ADBC_STATUS_INTERNAL;
      }
    }
  }
  // Slot 0 is where a non-prefetching reader fetches and converts, every time.
  ReaderUseSlot(r, 0);
  RAISE_ADBC(ReaderBindSlot(r, 0, error));
  r->bound = true;
#ifdef ADBC_ODBC_HAVE_PREFETCH
  if (r->nslots > 1) RAISE_ADBC(PrefetchStart(r, error));
#endif
  return ADBC_STATUS_OK;
}

// Bytes of a bound value the buffer can actually hold (the driver reserves room for
// a terminator in the character types).
static inline size_t BoundValueCap(const struct OdbcColumn* c) {
  return (size_t)c->elem_size - (c->c_type == SQL_C_CHAR      ? 1
                                 : c->c_type == SQL_C_WCHAR ? sizeof(SQLWCHAR)
                                                            : 0);
}

// Did the driver have more data for this value than the bound buffer could hold?
// Only the variable-length C types can report this.
static inline bool BoundValueTruncated(const struct OdbcColumn* c, SQLLEN ind) {
  if (c->c_type != SQL_C_CHAR && c->c_type != SQL_C_WCHAR && c->c_type != SQL_C_BINARY) {
    return false;
  }
  if (ind == SQL_NULL_DATA) return false;
  return ind == SQL_NO_TOTAL || ind < 0 || (size_t)ind > BoundValueCap(c);
}

// Make `row` of the current rowset the row SQLGetData reads from.  A one-row rowset
// is already positioned, so this is a no-op there.
static AdbcStatusCode PositionOnRow(struct OdbcReader* r, SQLULEN row,
                                    struct AdbcError* error) {
  if (r->rows_per_fetch <= 1) return ADBC_STATUS_OK;
  ODBC_CHECK(SQLSetPos(r->ref->hstmt, (SQLSETPOSIROW)(row + 1), SQL_POSITION,
                       SQL_LOCK_NO_CHANGE),
             SQL_HANDLE_STMT, r->ref->hstmt, "SQLSetPos(SQL_POSITION)", error);
  return ADBC_STATUS_OK;
}

// Fetch one unbound column value for the current row via SQLGetData into scratch.
// Returns status; sets *is_null; data is in r->scratch (size_bytes).
static AdbcStatusCode GetDataLong(struct OdbcReader* r, SQLSMALLINT i, SQLULEN row,
                                  bool* is_null, struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  struct OdbcColumn* c = &r->cols[i];
  r->scratch.size_bytes = 0;
  *is_null = false;
  const size_t chunk = 65536;
  size_t term = 0;
  if (c->c_type == SQL_C_CHAR) term = 1;
  else if (c->c_type == SQL_C_WCHAR) term = sizeof(SQLWCHAR);

  for (bool first = true;; first = false) {
    CHECK_NA(INTERNAL, ArrowBufferReserve(&r->scratch, (int64_t)chunk), error);
    uint8_t* dst = r->scratch.data + r->scratch.size_bytes;
    SQLLEN ind = 0;
    SQLRETURN ret = OdbcGetData(hstmt, (SQLUSMALLINT)(i + 1), c->c_type, dst, (SQLLEN)chunk, &ind,
                                r->opts.sqllen_32bit);
    if (ret == SQL_NO_DATA) {
      // SQL_NO_DATA on the *first* call means the driver has no value to give for the
      // row that was asked for.  On a one-row cursor that is a driver answering a
      // zero-length value, which the loop below treats as the empty string.  On a block
      // cursor it means SQLSetPos(SQL_POSITION) did not move the cursor even though the
      // driver claimed SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER, and returning an
      // empty value would quietly replace real data.
      if (first && r->rows_per_fetch > 1) {
        InternalAdbcSetError(error,
                             "[ODBC] SQLGetData returned no data for column %s after "
                             "SQLSetPos positioned on row %llu of a rowset: this driver's "
                             "SQL_GETDATA_EXTENSIONS overstates what it supports. Set "
                             "adbc.odbc.long_bind_bytes higher, or adbc.odbc.batch_size=1 "
                             "to read one row per fetch.",
                             c->name, (unsigned long long)(row + 1));
        return ADBC_STATUS_IO;
      }
      break;
    }
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
    if (BoundValueTruncated(c, ind)) {
      // SQLGetData can re-read a bound column where it sits when the driver says so, and
      // always when the cursor holds a single row (RepairRowset() re-reads a rowset that
      // way).  Otherwise the value is clipped to what the buffer held.
      if (r->opts.getdata_repair || (r->rows_per_fetch <= 1 && r->opts.getdata_bound)) {
        // The driver had more data than the bound buffer could hold.  Re-read the
        // whole value with SQLGetData rather than silently returning a prefix.
        RAISE_ADBC(PositionOnRow(r, row, error));
        RAISE_ADBC(GetDataLong(r, i, row, &is_null, error));
        data = r->scratch.data;
        len = (size_t)r->scratch.size_bytes;
      } else {
        len = (ind == SQL_NO_TOTAL || ind < 0) ? 0 : BoundValueCap(c);
      }
    } else if (ind == SQL_NO_TOTAL || ind < 0) {
      len = 0;
    } else {
      len = (size_t)ind;
    }
  } else {
    RAISE_ADBC(GetDataLong(r, i, row, &is_null, error));
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
      if (!ParseTimeScaled((const char*)data, len,
                           c->unit == NANOARROW_TIME_UNIT_NANO ? 9 : 6, &v)) {
        InternalAdbcSetError(error, "Could not parse time value '%.*s' for column %s", (int)len,
                             (const char*)data, c->name);
        return ADBC_STATUS_INVALID_DATA;
      }
      CHECK_NA(INTERNAL, ArrowArrayAppendInt(arr, v), error);
      break;
    }
    case FETCH_TIMESTAMP_TZ:
    case FETCH_TIMESTAMP_TEXT: {
      // Same parser for both: a text timestamp without an offset is already local, so
      // it lands unshifted in a naive timestamp[us] (FETCH_TIMESTAMP_TEXT), while one
      // that carries an offset is normalised to UTC (FETCH_TIMESTAMP_TZ).
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
      // TIMESTAMP_STRUCT::fraction is always in nanoseconds.
      int64_t v;
      switch (c->unit) {
        case NANOARROW_TIME_UNIT_SECOND: v = secs; break;
        case NANOARROW_TIME_UNIT_MILLI: v = secs * 1000LL + (int64_t)t->fraction / 1000000; break;
        case NANOARROW_TIME_UNIT_MICRO: v = secs * 1000000LL + (int64_t)t->fraction / 1000; break;
        default: v = secs * 1000000000LL + (int64_t)t->fraction; break;
      }
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

// ---------------------------------------------------------------------------
// Bulk (column-at-a-time) conversion of a whole ODBC rowset
//
// A bound column's rowset buffer is already a contiguous C array, so a whole
// rowset can be converted with one pass per column rather than one nanoarrow
// append call per value.  nanoarrow only materialises a validity bitmap once a
// null is appended, so a rowset with no nulls costs a reserve plus a memcpy (or
// one tight conversion loop) per column and touches no bitmap at all.

// Every kind handled by MemcpyWidth() copies the ODBC rowset buffer straight
// into the Arrow data buffer, which is only correct if the C types match.
_Static_assert(sizeof(SQLSCHAR) == 1, "SQLSCHAR must be 1 byte");
_Static_assert(sizeof(SQLSMALLINT) == 2, "SQLSMALLINT must be 2 bytes");
_Static_assert(sizeof(SQLUSMALLINT) == 2, "SQLUSMALLINT must be 2 bytes");
_Static_assert(sizeof(SQLINTEGER) == 4, "SQLINTEGER must be 4 bytes");
_Static_assert(sizeof(SQLUINTEGER) == 4, "SQLUINTEGER must be 4 bytes");
_Static_assert(sizeof(SQLBIGINT) == 8, "SQLBIGINT must be 8 bytes");
_Static_assert(sizeof(SQLUBIGINT) == 8, "SQLUBIGINT must be 8 bytes");
_Static_assert(sizeof(SQLREAL) == 4, "SQLREAL must be 4 bytes");
_Static_assert(sizeof(SQLDOUBLE) == 8, "SQLDOUBLE must be 8 bytes");

static inline int64_t BytesForBits(int64_t bits) { return (bits >> 3) + ((bits & 7) != 0); }

// Arrow element width for the kinds whose bound ODBC buffer already has the
// exact Arrow memory layout; 0 for kinds that need per-value conversion.
static int MemcpyWidth(const struct OdbcColumn* c) {
  switch (c->kind) {
    case FETCH_I8:
    case FETCH_U8: return 1;
    case FETCH_I16:
    case FETCH_U16: return 2;
    case FETCH_I32:
    case FETCH_U32:
    case FETCH_F32: return 4;
    case FETCH_I64:
    case FETCH_U64:
    case FETCH_F64: return 8;
    default: return 0;
  }
}

// Arrow element width of a fixed-width column, for reserving a batch up front.
static int FixedArrowWidth(const struct OdbcColumn* c) {
  int w = MemcpyWidth(c);
  if (w) return w;
  switch (c->kind) {
    case FETCH_DATE:
    case FETCH_TIME: return 4;
    case FETCH_TIMESTAMP:
    case FETCH_TIMESTAMP_TZ:
    case FETCH_TIMESTAMP_TEXT:
    case FETCH_TIME64: return 8;
    case FETCH_DECIMAL: return 16;
    default: return 0;
  }
}

// Byte length of the value in row `ind`, with the same truncation clamp
// AppendValue() applies.  Callers handle SQL_NULL_DATA separately.
static inline size_t BoundValueLen(const struct OdbcColumn* c, SQLLEN ind) {
  if (ind == SQL_NO_TOTAL || ind < 0) return 0;
  size_t len = (size_t)ind;
  size_t cap = (size_t)c->elem_size - (c->c_type == SQL_C_CHAR      ? 1
                                       : c->c_type == SQL_C_WCHAR ? sizeof(SQLWCHAR)
                                                                  : 0);
  return len > cap ? cap : len;
}

// Append n validity bits for a rowset.  Mirrors nanoarrow's laziness: an array
// that has seen no null keeps its bitmap unallocated, which is what lets a
// null-free result set skip the bitmap entirely.
static ArrowErrorCode BulkValidity(struct ArrowArray* arr, const SQLLEN* ind, int64_t n,
                                   int64_t nulls) {
  struct ArrowBitmap* bitmap = ArrowArrayValidityBitmap(arr);
  if (nulls == 0 && bitmap->buffer.data == NULL) return NANOARROW_OK;
  if (bitmap->buffer.data == NULL) {
    NANOARROW_RETURN_NOT_OK(ArrowBitmapReserve(bitmap, arr->length + n));
    ArrowBitmapAppendUnsafe(bitmap, 1, arr->length);
  } else {
    NANOARROW_RETURN_NOT_OK(ArrowBitmapReserve(bitmap, n));
  }
  int64_t i = 0;
  while (i < n) {
    uint8_t valid = ind[i] != SQL_NULL_DATA;
    int64_t j = i + 1;
    while (j < n && (uint8_t)(ind[j] != SQL_NULL_DATA) == valid) j++;
    ArrowBitmapAppendUnsafe(bitmap, valid, j - i);
    i = j;
  }
  arr->null_count += nulls;
  return NANOARROW_OK;
}

// Convert `nrows` rows of one bound column into `arr`.  Returns
// ADBC_STATUS_NOT_IMPLEMENTED when the caller should use the per-value path.
static AdbcStatusCode BulkAppendColumn(struct OdbcReader* r, SQLSMALLINT i, SQLULEN nrows,
                                       struct ArrowArray* arr, struct AdbcError* error) {
  struct OdbcColumn* c = &r->cols[i];
  const int64_t n = (int64_t)nrows;
  // The bulk loops index the indicator array as SQLLEN; a 32-bit-SQLLEN driver
  // (Db2 clidriver, MDB Tools) writes it at stride 4, so those columns take the
  // per-value path, which reads indicators through OdbcIndicatorGet().
  if (r->opts.sqllen_32bit) return ADBC_STATUS_NOT_IMPLEMENTED;
  const SQLLEN* ind = c->indicators;
  const uint8_t* base = (const uint8_t*)c->buffer;
  const size_t stride = (size_t)c->elem_size;
  const int64_t len0 = arr->length;
  const int width = MemcpyWidth(c);

  switch (c->kind) {
    case FETCH_I8: case FETCH_I16: case FETCH_I32: case FETCH_I64:
    case FETCH_U8: case FETCH_U16: case FETCH_U32: case FETCH_U64:
    case FETCH_F32: case FETCH_F64:
    case FETCH_BOOL: case FETCH_BOOL_STR:
    case FETCH_CHAR: case FETCH_BINARY: case FETCH_WCHAR:
    case FETCH_DATE: case FETCH_TIME: case FETCH_TIMESTAMP:
      break;
    default:
      // FETCH_DECIMAL / FETCH_TIME64 / FETCH_TIMESTAMP_TZ parse text per value;
      // the parse dominates, so there is nothing for a bulk path to save.
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }

  int64_t nulls = 0;
  for (int64_t row = 0; row < n; row++) nulls += (ind[row] == SQL_NULL_DATA);
  if (c->c_type == SQL_C_CHAR || c->c_type == SQL_C_WCHAR || c->c_type == SQL_C_BINARY) {
    for (int64_t row = 0; row < n; row++) {
      if (BoundValueTruncated(c, ind[row])) return ADBC_STATUS_NOT_IMPLEMENTED;
    }
  }

  if (width > 0) {
    if (stride != (size_t)width) return ADBC_STATUS_NOT_IMPLEMENTED;
    struct ArrowBuffer* dat = ArrowArrayBuffer(arr, 1);
    CHECK_NA(INTERNAL, ArrowBufferReserve(dat, n * width), error);
    memcpy(dat->data + dat->size_bytes, base, (size_t)n * (size_t)width);
    dat->size_bytes += n * width;
  } else if (c->kind == FETCH_DATE || c->kind == FETCH_TIME) {
    struct ArrowBuffer* dat = ArrowArrayBuffer(arr, 1);
    CHECK_NA(INTERNAL, ArrowBufferReserve(dat, n * 4), error);
    int32_t* o = (int32_t*)(dat->data + dat->size_bytes);
    if (c->kind == FETCH_DATE) {
      for (int64_t row = 0; row < n; row++) {
        if (ind[row] == SQL_NULL_DATA) { o[row] = 0; continue; }
        const DATE_STRUCT* d = (const DATE_STRUCT*)(base + (size_t)row * stride);
        o[row] = (int32_t)DaysFromCivil(d->year, d->month, d->day);
      }
    } else {
      for (int64_t row = 0; row < n; row++) {
        if (ind[row] == SQL_NULL_DATA) { o[row] = 0; continue; }
        const TIME_STRUCT* t = (const TIME_STRUCT*)(base + (size_t)row * stride);
        o[row] = (int32_t)(t->hour * 3600 + t->minute * 60 + t->second);
      }
    }
    dat->size_bytes += n * 4;
  } else if (c->kind == FETCH_TIMESTAMP) {
    struct ArrowBuffer* dat = ArrowArrayBuffer(arr, 1);
    CHECK_NA(INTERNAL, ArrowBufferReserve(dat, n * 8), error);
    int64_t* o = (int64_t*)(dat->data + dat->size_bytes);
    // Must agree with the per-value FETCH_TIMESTAMP arm: ClassifyColumn picks the
    // unit from the column's fractional-seconds precision, so all four are reachable.
    // TIMESTAMP_STRUCT::fraction is always in nanoseconds.
    const int64_t mul = c->unit == NANOARROW_TIME_UNIT_SECOND   ? 1
                        : c->unit == NANOARROW_TIME_UNIT_MILLI  ? 1000LL
                        : c->unit == NANOARROW_TIME_UNIT_MICRO  ? 1000000LL
                                                                : 1000000000LL;
    const int64_t div = 1000000000LL / mul;
    for (int64_t row = 0; row < n; row++) {
      if (ind[row] == SQL_NULL_DATA) { o[row] = 0; continue; }
      const TIMESTAMP_STRUCT* t = (const TIMESTAMP_STRUCT*)(base + (size_t)row * stride);
      int64_t secs = DaysFromCivil(t->year, t->month, t->day) * 86400 + t->hour * 3600 +
                     t->minute * 60 + t->second;
      o[row] = secs * mul + (int64_t)t->fraction / div;
    }
    dat->size_bytes += n * 8;
  } else if (c->kind == FETCH_BOOL || c->kind == FETCH_BOOL_STR) {
    struct ArrowBuffer* dat = ArrowArrayBuffer(arr, 1);
    int64_t need = BytesForBits(len0 + n);
    if (need > dat->size_bytes) {
      CHECK_NA(INTERNAL, ArrowBufferAppendFill(dat, 0, need - dat->size_bytes), error);
    }
    for (int64_t row = 0; row < n; row++) {
      uint8_t v = 0;
      if (ind[row] != SQL_NULL_DATA) {
        const uint8_t* p = base + (size_t)row * stride;
        if (c->kind == FETCH_BOOL) {
          v = *p != 0;
        } else if (BoundValueLen(c, ind[row]) > 0) {
          char ch = (char)*p;
          v = (ch == 't' || ch == 'T' || ch == '1' || ch == 'y' || ch == 'Y');
        }
      }
      ArrowBitSetTo(dat->data, len0 + row, v);
    }
  } else {
    // FETCH_CHAR / FETCH_BINARY / FETCH_WCHAR: one offsets entry and a memcpy
    // (or transcode) per value, with the batch's data buffer sized from the
    // indicator array in a single pass first.
    struct ArrowBuffer* off = ArrowArrayBuffer(arr, 1);
    struct ArrowBuffer* dat = ArrowArrayBuffer(arr, 2);
    int64_t src_bytes = 0;
    for (int64_t row = 0; row < n; row++) {
      if (ind[row] != SQL_NULL_DATA) src_bytes += (int64_t)BoundValueLen(c, ind[row]);
    }
    int64_t max_bytes = c->kind == FETCH_WCHAR
                            ? Utf16Utf8MaxBytes(src_bytes / (int64_t)sizeof(SQLWCHAR))
                            : src_bytes;
    const int32_t start = ((const int32_t*)off->data)[len0];
    // Leave a >2GiB batch to the per-value path, which reports EOVERFLOW.
    if ((int64_t)start + max_bytes > INT32_MAX) return ADBC_STATUS_NOT_IMPLEMENTED;
    CHECK_NA(INTERNAL, ArrowBufferReserve(off, n * 4), error);
    CHECK_NA(INTERNAL, ArrowBufferReserve(dat, max_bytes), error);
    int32_t* o = (int32_t*)(off->data + off->size_bytes);
    uint8_t* d = dat->data + dat->size_bytes;
    int32_t at = start;
    for (int64_t row = 0; row < n; row++) {
      size_t len = ind[row] == SQL_NULL_DATA ? 0 : BoundValueLen(c, ind[row]);
      if (len > 0) {
        const uint8_t* p = base + (size_t)row * stride;
        size_t wrote;
        if (c->kind == FETCH_WCHAR) {
          wrote = Utf16ToUtf8((const SQLWCHAR*)p, len / sizeof(SQLWCHAR), d);
        } else {
          memcpy(d, p, len);
          wrote = len;
        }
        d += wrote;
        at += (int32_t)wrote;
      }
      o[row] = at;
    }
    off->size_bytes += n * 4;
    dat->size_bytes = d - dat->data;
  }

  CHECK_NA(INTERNAL, BulkValidity(arr, ind, n, nulls), error);
  arr->length = len0 + n;
  return ADBC_STATUS_OK;
}

// Did any bound value in this rowset come back longer than its buffer?
// Takes the slot explicitly rather than reading through OdbcColumn::indicators: the
// fetch thread asks this about the rowset *it* just filled, while OdbcColumn::indicators
// points at whichever slot the calling thread is converting.  Everything else it reads
// off the column (bound, c_type, elem_size) is fixed by ReaderBind before the fetch
// thread exists.
static bool RowsetTruncated(const struct OdbcReader* r, const struct OdbcRowsetSlot* slot,
                            SQLULEN fetched) {
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    const struct OdbcColumn* c = &r->cols[i];
    if (!c->bound) continue;
    if (c->c_type != SQL_C_CHAR && c->c_type != SQL_C_WCHAR && c->c_type != SQL_C_BINARY) {
      continue;
    }
    for (SQLULEN row = 0; row < fetched; row++) {
      SQLLEN ind = OdbcIndicatorGet(slot->indicators[i], (size_t)row, r->opts.sqllen_32bit);
      if (BoundValueTruncated(c, ind)) return true;
    }
  }
  return false;
}

// Change SQL_ATTR_ROW_ARRAY_SIZE on a cursor that is already open, and say whether the
// cursor is now fetching that many rows.  Every mid-cursor resize goes through here: a
// driver whose rowset is fixed once the statement has been executed
// (OdbcReaderOptions::fixed_rowset -- Oracle's SQORA, where raising it segfaults inside
// the driver and lowering it truncates the result set) is left at the size it was given
// before the first fetch, and every caller has a path that works without the resize.
static bool ReaderResizeRowset(struct OdbcReader* r, SQLULEN rows) {
  if (r->opts.fixed_rowset) return false;
  return SQL_SUCCEEDED(
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)rows, 0));
}

// Re-read a rowset that clipped a value, one row at a time, and append every row.
//
// `first` is the 1-based position of the rowset's first row in the result set.  With
// SQL_ATTR_ROW_ARRAY_SIZE at 1 the cursor holds a single row, so SQLGetData is legal
// again and AppendValue() re-reads each clipped value in full.  The last
// SQLFetchScroll leaves the cursor on the rowset's last row, which is where a plain
// SQLFetch has to resume from.
static AdbcStatusCode RepairRowset(struct OdbcReader* r, int64_t first, SQLULEN fetched,
                                   struct ArrowArray* batch, struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  const SQLULEN block = r->rows_per_fetch;
  AdbcStatusCode status = ADBC_STATUS_OK;
  ODBC_CHECK(SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0), SQL_HANDLE_STMT,
             hstmt, "SQLSetStmtAttr(SQL_ATTR_ROW_ARRAY_SIZE=1)", error);
  r->rows_per_fetch = 1;
  for (SQLULEN k = 0; k < fetched && status == ADBC_STATUS_OK; k++) {
    r->slots[r->cur_slot].rows_fetched_raw = 0;
    SQLRETURN ret = SQLFetchScroll(hstmt, SQL_FETCH_ABSOLUTE, (SQLLEN)(first + (int64_t)k));
    if (!SQL_SUCCEEDED(ret)) {
      status = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLFetchScroll(SQL_FETCH_ABSOLUTE)", error);
      break;
    }
    for (SQLSMALLINT i = 0; i < r->ncols && status == ADBC_STATUS_OK; i++) {
      status = AppendValue(r, i, 0, batch->children[i], error);
    }
  }
  r->rows_per_fetch = block;
  SQLRETURN back = SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)block, 0);
  if (status == ADBC_STATUS_OK && !SQL_SUCCEEDED(back)) {
    return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLSetStmtAttr(SQL_ATTR_ROW_ARRAY_SIZE)", error);
  }
  return status;
}

// Reserve each column's fixed-size buffers for a whole batch, so no column has
// to walk a realloc chain while it fills.  A failed reserve has to be propagated
// rather than ignored: ArrowBufferReserve() zeroes the buffer's size on ENOMEM,
// which would drop the leading 0 that ArrowArrayStartAppending() put in an
// offsets buffer.
// How many rows the SQLFetch that filled `slot` produced.  SQL_ATTR_ROWS_FETCHED_PTR is
// a SQLULEN the driver writes; it is zeroed before every fetch so a 32-bit-SQLLEN
// driver's low half is the whole count.  A driver that refused the attribute outright
// (MDB Tools) never writes it and fetches one row per SQLFetch.
static SQLULEN ResolveFetched(const struct OdbcReader* r,
                              const struct OdbcRowsetSlot* slot) {
  if (r->no_rows_fetched_ptr) return 1;
  SQLULEN raw = slot->rows_fetched_raw;
  return OdbcReadULen(&raw, r->opts.sqllen_32bit);
}

// Column-at-a-time conversion needs every row of the rowset to be usable; a rowset with
// skipped or failed rows falls back to the row-at-a-time path.
static bool RowsetIsBulk(const struct OdbcReader* r, SQLULEN fetched) {
  if (fetched == 0) return false;
  for (SQLULEN row = 0; row < fetched; row++) {
    SQLUSMALLINT st = r->row_status[row];
    if (st == SQL_ROW_NOROW || st == SQL_ROW_ERROR) return false;
  }
  return true;
}

// Append one already-fetched rowset -- whichever slot the columns currently point at --
// to `batch`.  Reads only memory for a bound column, which is what lets the prefetching
// caller run this while the fetch thread is filling the next slot.
static AdbcStatusCode ConvertRowset(struct OdbcReader* r, SQLULEN fetched, bool bulk,
                                    struct ArrowArray* batch, int64_t* total,
                                    struct AdbcError* error) {
  AdbcStatusCode status = ADBC_STATUS_OK;
  if (bulk) {
    // Bound columns: one pass per column over the whole rowset.
    for (SQLSMALLINT i = 0; i < r->ncols && status == ADBC_STATUS_OK; i++) {
      if (!r->cols[i].bound) continue;
      status = BulkAppendColumn(r, i, fetched, batch->children[i], error);
      if (status == ADBC_STATUS_NOT_IMPLEMENTED) {
        status = ADBC_STATUS_OK;
        for (SQLULEN row = 0; row < fetched && status == ADBC_STATUS_OK; row++) {
          status = AppendValue(r, i, row, batch->children[i], error);
        }
      }
    }
    // Unbound columns: SQLGetData, which must be issued row by row and in increasing
    // column order.  DescribeColumns() has already put every unbound column after every
    // bound one.  (Prefetch never engages when there is one -- see
    // PrefetchRefusalReason -- so this is always the caller's own cursor.)
    if (!r->all_bound) {
      for (SQLULEN row = 0; row < fetched && status == ADBC_STATUS_OK; row++) {
        for (SQLSMALLINT i = 0; i < r->ncols && status == ADBC_STATUS_OK; i++) {
          if (r->cols[i].bound) continue;
          status = AppendValue(r, i, row, batch->children[i], error);
        }
      }
    }
    if (status == ADBC_STATUS_OK) *total += (int64_t)fetched;
    return status;
  }
  for (SQLULEN row = 0; row < fetched && status == ADBC_STATUS_OK; row++) {
    if (r->row_status[row] == SQL_ROW_NOROW) continue;
    if (r->row_status[row] == SQL_ROW_ERROR) {
      InternalAdbcSetError(error, "[ODBC] row %lu reported SQL_ROW_ERROR", (unsigned long)row);
      return ADBC_STATUS_IO;
    }
    for (SQLSMALLINT i = 0; i < r->ncols && status == ADBC_STATUS_OK; i++) {
      status = AppendValue(r, i, row, batch->children[i], error);
    }
    (*total)++;
  }
  return status;
}

#ifdef ADBC_ODBC_HAVE_PREFETCH
// --- The fetch thread ----------------------------------------------------------------
//
// Owns r->ref->hstmt outright for as long as it runs.  Everything it shares with the
// caller -- the ring indices, the end-of-stream and error flags -- is under r->mu; the
// slot contents themselves need no lock, because a slot is either the fetch thread's
// (not yet published) or the caller's (published and not yet freed), never both.

static void* PrefetchMain(void* arg) {
  struct OdbcReader* r = (struct OdbcReader*)arg;
  struct AdbcError err = {0};

  for (;;) {
    pthread_mutex_lock(&r->mu);
    while (r->ring_filled >= r->nslots && !r->fetch_stop) pthread_cond_wait(&r->cv, &r->mu);
    if (r->fetch_stop) {
      pthread_mutex_unlock(&r->mu);
      break;
    }
    const int sl = r->ring_tail;
    pthread_mutex_unlock(&r->mu);

    struct OdbcRowsetSlot* slot = &r->slots[sl];
    slot->eos = false;
    slot->needs_repair = false;
    slot->fetched = 0;
    slot->rows_fetched_raw = 0;

    AdbcStatusCode status = ReaderBindSlot(r, sl, &err);
    SQLRETURN ret = SQL_NO_DATA;
    if (status == ADBC_STATUS_OK) {
      ret = SQLFetch(r->ref->hstmt);
      if (ret == SQL_NO_DATA) {
        slot->eos = true;
      } else if (!SQL_SUCCEEDED(ret)) {
        status = OdbcSetError(SQL_HANDLE_STMT, r->ref->hstmt, "SQLFetch", &err);
      }
    }

    bool stop_after = false;
    if (status == ADBC_STATUS_OK && !slot->eos) {
      slot->fetched = ResolveFetched(r, slot);
      slot->first_row = r->rows_seen + 1;
      r->rows_seen += (int64_t)slot->fetched;
      // Truncation needs rows this thread has already scrolled past; publish the rowset
      // for the caller to repair and give the cursor back.  PrefetchRefusalReason()
      // keeps a clipped column from ever getting here, so this is a driver contradicting
      // its own metadata rather than the ordinary long-value path.
      if (RowsetTruncated(r, slot, slot->fetched)) {
        slot->needs_repair = true;
        stop_after = true;
      }
    }

    pthread_mutex_lock(&r->mu);
    if (status != ADBC_STATUS_OK) {
      r->fetch_status = status;
      r->fetch_error = err;  // ownership moves to the reader
      memset(&err, 0, sizeof(err));
      r->fetch_failed = true;
      pthread_cond_broadcast(&r->cv);
      pthread_mutex_unlock(&r->mu);
      break;
    }
    r->ring_tail = (r->ring_tail + 1) % r->nslots;
    r->ring_filled++;
    if (slot->eos) r->fetch_done = true;
    pthread_cond_broadcast(&r->cv);
    const bool leave = slot->eos || stop_after;
    pthread_mutex_unlock(&r->mu);
    if (leave) break;
  }
  if (err.release) err.release(&err);
  return NULL;
}

static AdbcStatusCode PrefetchStart(struct OdbcReader* r, struct AdbcError* error) {
  if (pthread_mutex_init(&r->mu, NULL) != 0) {
    InternalAdbcSetError(error, "failed to create the prefetch mutex");
    return ADBC_STATUS_INTERNAL;
  }
  if (pthread_cond_init(&r->cv, NULL) != 0) {
    pthread_mutex_destroy(&r->mu);
    InternalAdbcSetError(error, "failed to create the prefetch condition variable");
    return ADBC_STATUS_INTERNAL;
  }
  if (pthread_create(&r->fetch_thread, NULL, PrefetchMain, r) != 0) {
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    // Not fatal: a reader that cannot start a thread reads the ordinary way out of slot
    // 0, which ReaderBind has already bound.  `nslots` deliberately keeps its value --
    // the other slots are allocated and ReaderRelease frees exactly that many.
    return ADBC_STATUS_OK;
  }
  r->thread_started = true;
  r->prefetching = true;
  return ADBC_STATUS_OK;
}

// Stop the fetch thread and take the handle back.  Idempotent, and the only way the
// caller is ever allowed to touch the handle again -- pthread_join is the barrier that
// makes the transfer of ownership real.
static void PrefetchJoin(struct OdbcReader* r) {
  if (!r->thread_started) return;
  pthread_mutex_lock(&r->mu);
  r->fetch_stop = true;
  pthread_cond_broadcast(&r->cv);
  pthread_mutex_unlock(&r->mu);
  pthread_join(r->fetch_thread, NULL);
  r->thread_started = false;
  r->prefetching = false;
  pthread_cond_destroy(&r->cv);
  pthread_mutex_destroy(&r->mu);
}

// Take the next rowset from the ring and append it to `batch`.  Sets r->done at the end
// of the stream, and clears r->prefetching if the fetch thread handed the cursor back.
static AdbcStatusCode PrefetchNextRowset(struct OdbcReader* r, struct ArrowArray* batch,
                                         int64_t* total, struct AdbcError* error) {
  pthread_mutex_lock(&r->mu);
  while (r->ring_filled == 0 && !r->fetch_failed && !r->fetch_done) {
    pthread_cond_wait(&r->cv, &r->mu);
  }
  if (r->ring_filled == 0) {
    // The ring is drained; whatever the thread stopped for is now the answer.
    const bool failed = r->fetch_failed;
    const AdbcStatusCode status = r->fetch_status;
    pthread_mutex_unlock(&r->mu);
    PrefetchJoin(r);
    r->done = true;
    if (failed) {
      // The thread captured the ODBC diagnostics; move them to the caller's error.
      if (r->fetch_error.message) {
        InternalAdbcSetError(error, "%s", r->fetch_error.message);
      }
      return status;
    }
    return ADBC_STATUS_OK;
  }
  const int sl = r->ring_head;
  pthread_mutex_unlock(&r->mu);

  struct OdbcRowsetSlot* slot = &r->slots[sl];
  if (slot->eos) {
    PrefetchJoin(r);
    r->done = true;
    return ADBC_STATUS_OK;
  }

  ReaderUseSlot(r, sl);
  if (slot->needs_repair) {
    // The fetch thread stopped here.  Join it, rebind the handle to this slot, and let
    // the ordinary repair path re-read the rowset row by row; the rest of the result set
    // is then read synchronously.
    PrefetchJoin(r);
    RAISE_ADBC(ReaderBindSlot(r, sl, error));
    RAISE_ADBC(RepairRowset(r, slot->first_row, slot->fetched, batch, error));
    *total += (int64_t)slot->fetched;
    r->rowsets_read++;
    r->rowsets_repaired++;
    return ADBC_STATUS_OK;
  }

  const bool bulk = RowsetIsBulk(r, slot->fetched);
  if (!bulk) r->rows_seen_exact = false;
  r->rowsets_read++;
  AdbcStatusCode status = ConvertRowset(r, slot->fetched, bulk, batch, total, error);

  pthread_mutex_lock(&r->mu);
  r->ring_head = (r->ring_head + 1) % r->nslots;
  r->ring_filled--;
  pthread_cond_broadcast(&r->cv);
  pthread_mutex_unlock(&r->mu);
  return status;
}
#endif  // ADBC_ODBC_HAVE_PREFETCH

static ArrowErrorCode ReserveBatch(struct OdbcReader* r, struct ArrowArray* batch) {
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    struct ArrowArray* arr = batch->children[i];
    int width = FixedArrowWidth(&r->cols[i]);
    if (width > 0) {
      NANOARROW_RETURN_NOT_OK(
          ArrowBufferReserve(ArrowArrayBuffer(arr, 1), r->opts.batch_size * width));
    } else if (arr->n_buffers == 3) {
      NANOARROW_RETURN_NOT_OK(
          ArrowBufferReserve(ArrowArrayBuffer(arr, 1), r->opts.batch_size * 4));
    }
  }
  return NANOARROW_OK;
}

// Give up on a column bound at a guessed width, and on every column after it, for the
// rest of this result set: unbind them and read them with SQLGetData instead.  ODBC (and
// SQL Server strictly) wants the SQLGetData columns after all the bound ones and read in
// increasing order, which is the same rule DescribeColumns() applies up front.
//
// The rowset collapses to one row with them, which is the whole cost of this and the
// reason ApplyBindWidth() binds such a column in the first place.  It is not avoidable
// by unbinding the one column and keeping the block cursor for the rest: psqlodbc --
// which does support SQLSetPos + SQLGetData on a *bound* column of a block cursor, and
// is why this reader can repair a truncated value at all -- answers SQL_NO_DATA for
// every row after the first when the column is not bound.  Measured on the same driver,
// keeping the block cursor would not have paid anyway: with the column bound one byte
// wide, so that the buffer write costs nothing and every row is repaired, 50,000 rows of
// 3 KiB `bytea` still took 1.04 s against 0.71 s unbound.  What the repair costs is the
// SQLGetData against a block cursor, not the buffer it truncates into.
static AdbcStatusCode ReaderUnbindFrom(struct OdbcReader* r, SQLSMALLINT from,
                                       struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  // Only give the binding up if the cursor can actually be collapsed; SQLGetData needs
  // the one-row rowset (see above), so a driver that refuses is left as it was.
  if (r->rows_per_fetch > 1) {
    if (!ReaderResizeRowset(r, 1)) {
      // Keep the bindings, and the rowset they were sized for.  A fixed-rowset driver is
      // already at the size it will stay at, so it needs no restore either.
      r->rowset_restore = !r->opts.fixed_rowset;
      return ADBC_STATUS_OK;
    }
    r->rows_per_fetch = 1;
  }
  for (SQLSMALLINT i = from; i < r->ncols; i++) {
    if (!r->cols[i].bound) continue;
    ODBC_CHECK(SQLBindCol(hstmt, (SQLUSMALLINT)(i + 1), r->cols[i].c_type, NULL, 0, NULL),
               SQL_HANDLE_STMT, hstmt, "SQLBindCol(unbind)", error);
    r->cols[i].bound = false;
    r->cols[i].clipped = false;
    for (int sl = 0; sl < r->nslots; sl++) {
      free(r->slots[sl].buffers[i]);
      r->slots[sl].buffers[i] = NULL;
    }
  }
  r->all_bound = false;
  ReaderUseSlot(r, r->cur_slot);  // repoint the columns; the freed ones become NULL
  return ADBC_STATUS_OK;
}

// Watch what a column bound at a guessed width actually costs, and stop binding it if
// the values do not fit in it.
//
// `ApplyBindWidth` binds a length-less column -- psqlodbc's `bytea` (column_size 0) and
// its `text` (8190), sqliteodbc's every TEXT column -- at a width it invented, because
// one unbound column costs the whole result set its block cursor.  That is a good bet
// when the values fit.  It is a bad bet when they do not, because the driver then reads
// a truncated value twice: once decoding it into the rowset buffer (which it does in
// full, to report the length it did not fit) and once again for the SQLGetData that
// repairs it, where an unbound column would have decoded it exactly once.  Measured on
// PostgreSQL 16 through psqlodbc 16, 50,000 rows of (int4, bytea), median of 7:
//
//   value size    64 B    512 B    1 KiB    2 KiB   |   3 KiB    4 KiB   16 KiB*  64 KiB*
//   unbound     0.028 s  0.113 s  0.199 s  0.477 s  |  0.742 s  0.815 s  0.692 s  0.722 s
//   bound       0.023 s  0.100 s  0.182 s  0.447 s  |  1.091 s  1.180 s  0.972 s  0.994 s
//                 1.24x    1.13x    1.09x    1.07x  |    0.68x    0.69x    0.71x    0.73x
//   (* 12,000 and 3,000 rows; `bound` truncates at long_bind_bytes = 2 KiB.  The full
//   sweep, the mixed tables and the controls are in bench/BENCHMARKS.md.)
//
// The rule is *the bytes that had to be re-read*, not the fraction of rows that
// truncated.  What a truncation costs is the second decode, which is proportional to the
// value -- 7 us for a 3 KiB value, 54 us for a 64 KiB one, i.e. about 0.8 us/KiB over a
// 5 us fixed part -- while what the block cursor saves is a flat ~0.2 us per row that
// does not truncate.  So the two sides cross over at roughly 256 re-read bytes per row,
// and a row-fraction rule would get the case this exists to protect exactly backwards: a
// table where 1% of the rows hold 64 KiB and the rest 64 B truncates on 1% of its rows
// and still reads 21% slower bound (0.183 s) than unbound (0.151 s), because 1% of
// 64 KiB is 655 bytes per row of double decoding and 99% of a 0.2 us saving is not.
// Every table measured here lands at least 1.6x clear of the 256-byte line on one side
// or the other, which is the sense in which the constant is not load-bearing.
//
// The decision is per column and per result set, and once taken it is never revisited: a
// column that does not truncate in the window keeps exactly the binding it has today,
// and a result set with no clipped column never arms this at all (see ReaderBind).
// Columns after the one that loses its binding have to follow it -- ODBC wants the
// SQLGetData columns last and in increasing order -- but columns before it keep theirs.
//
// Drivers that repair a truncated value by re-reading its whole rowset instead
// (refetch_repair -- sqliteodbc, MariaDB Connector/ODBC) are not covered here and do not
// need to be: their cost is per *rowset*, not per row, and ReaderNextBatch() already
// drops them to one row per fetch once three rowsets in four have had to be repaired.
static AdbcStatusCode AdaptBindWidth(struct OdbcReader* r, SQLULEN fetched,
                                     struct AdbcError* error) {
  const struct OdbcRowsetSlot* slot = &r->slots[r->cur_slot];
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    struct OdbcColumn* c = &r->cols[i];
    if (!c->bound || !c->clipped) continue;
    if (c->c_type != SQL_C_CHAR && c->c_type != SQL_C_WCHAR && c->c_type != SQL_C_BINARY) {
      continue;
    }
    for (SQLULEN row = 0; row < fetched; row++) {
      SQLLEN ind = OdbcIndicatorGet(slot->indicators[i], (size_t)row, r->opts.sqllen_32bit);
      if (!BoundValueTruncated(c, ind)) continue;
      // A driver that answers SQL_NO_TOTAL (or a negative length) is saying only that
      // the value did not fit; charge it the buffer, which is what it did write.
      c->trunc_bytes += (ind > c->elem_size) ? (int64_t)ind : (int64_t)c->elem_size;
    }
  }
  r->adapt_rows += (int64_t)fetched;
  if (r->adapt_rows < ODBC_ADAPT_WINDOW_ROWS) return ADBC_STATUS_OK;

  r->adapt_open = false;
  for (SQLSMALLINT i = 0; i < r->ncols; i++) {
    const struct OdbcColumn* c = &r->cols[i];
    if (!c->bound || !c->clipped) continue;
    if (c->trunc_bytes > r->adapt_rows * ODBC_ADAPT_REREAD_BUDGET) {
      return ReaderUnbindFrom(r, i, error);
    }
  }
  // Every clipped column earns its binding: go back to the full rowset, at the next
  // batch boundary so that this batch keeps the size it would have had.
  r->rowset_restore = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode ReaderNextBatch(struct OdbcReader* r, struct ArrowArray* out,
                                      struct AdbcError* error) {
  SQLHSTMT hstmt = r->ref->hstmt;
  if (!r->bound) RAISE_ADBC(ReaderBind(r, error));

  // The width adaptation is over and every column kept its binding: take the full rowset
  // back, here rather than where that was decided so the batch it was decided in keeps
  // the size it would otherwise have had.
  if (r->rowset_restore) {
    r->rowset_restore = false;
    if (ReaderResizeRowset(r, r->rowset_full)) r->rows_per_fetch = r->rowset_full;
  }

  struct ArrowArray batch;
  batch.release = NULL;
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&batch, &r->schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&batch), error);
  CHECK_NA(INTERNAL, ReserveBatch(r, &batch), error);

  int64_t total = 0;
  AdbcStatusCode status = ADBC_STATUS_OK;
  // Stop before a rowset would take the batch past batch_size -- unless it is the first
  // one, since a batch always holds at least one rowset however wide the rowset is.
  while ((total == 0 || total + (int64_t)r->rows_per_fetch <= r->opts.batch_size) && !r->done) {
#ifdef ADBC_ODBC_HAVE_PREFETCH
    if (r->prefetching) {
      status = PrefetchNextRowset(r, &batch, &total, error);
      // A hand-back clears r->prefetching, and the next turn of this loop picks the
      // cursor up synchronously exactly where the fetch thread put it down.
      if (status != ADBC_STATUS_OK) break;
      continue;
    }
#endif
    r->slots[r->cur_slot].rows_fetched_raw = 0;
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
    // 32-bit-SQLLEN driver's low half is the whole count.  A driver that refused the
    // attribute outright (MDB Tools) never writes it and fetches one row per SQLFetch.
    const SQLULEN fetched = ResolveFetched(r, &r->slots[r->cur_slot]);
    const bool bulk = RowsetIsBulk(r, fetched);
    const int64_t first_row = r->rows_seen + 1;
    r->rows_seen += (int64_t)fetched;
    if (!bulk) r->rows_seen_exact = false;
    // A value longer than its bound buffer is only a prefix.  When the driver cannot
    // re-read it in place, re-read the whole rowset one row at a time -- which this
    // reader only ever bound a "long" column for because that is possible.
    r->rowsets_read++;
    if (bulk && r->rows_seen_exact && r->rows_per_fetch > 1 && !r->opts.getdata_repair &&
        r->opts.refetch_repair && !r->opts.fixed_rowset &&
        RowsetTruncated(r, &r->slots[r->cur_slot], fetched)) {
      status = RepairRowset(r, first_row, fetched, &batch, error);
      if (status != ADBC_STATUS_OK) break;
      total += (int64_t)fetched;
      r->rowsets_repaired++;
      // Repairing means reading the rowset twice.  Once that is the rule rather than the
      // exception, read one row per SQLFetch instead: every value is then read where it
      // sits, which is what an unbound column would have cost from the start.
      if (r->rowsets_repaired >= 4 && r->rowsets_repaired * 4 >= r->rowsets_read * 3) {
        if (ReaderResizeRowset(r, 1)) r->rows_per_fetch = 1;
      }
      continue;
    }
    status = ConvertRowset(r, fetched, bulk, &batch, &total, error);
    if (status != ADBC_STATUS_OK) break;
    // The rowset has been converted out of the bound buffers, so they can now be given
    // up if this column's values do not fit in them (AdaptBindWidth).
    if (r->adapt_open && bulk) {
      status = AdaptBindWidth(r, fetched, error);
      if (status != ADBC_STATUS_OK) break;
    }
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
#ifdef ADBC_ODBC_HAVE_PREFETCH
    // The fetch thread owns the handle; nothing below may touch it until it is joined.
    // This is also the abort path: a caller that releases the stream part-way through
    // gets here with the thread mid-SQLFetch, and it is stopped at the next rowset
    // boundary rather than left running against a freed handle.
    PrefetchJoin(r);
    if (r->fetch_error.release) r->fetch_error.release(&r->fetch_error);
#endif
    if (r->ref && r->ref->hstmt) {
      SQLCloseCursor(r->ref->hstmt);
      SQLFreeStmt(r->ref->hstmt, SQL_UNBIND);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROW_STATUS_PTR, NULL, 0);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROWS_FETCHED_PTR, NULL, 0);
      SQLSetStmtAttr(r->ref->hstmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0);
    }
    OdbcHandleRefRelease(r->ref);
    for (int sl = 0; sl < r->nslots; sl++) {
      struct OdbcRowsetSlot* slot = &r->slots[sl];
      for (SQLSMALLINT i = 0; slot->buffers && i < r->ncols; i++) free(slot->buffers[i]);
      for (SQLSMALLINT i = 0; slot->indicators && i < r->ncols; i++) free(slot->indicators[i]);
      free(slot->buffers);
      free(slot->indicators);
      free(slot->row_status);
    }
    free(r->slots);
    // The columns' buffer/indicator pointers are borrowed from a slot, so FreeColumns()
    // must not free them.
    for (SQLSMALLINT i = 0; i < r->ncols; i++) {
      r->cols[i].buffer = NULL;
      r->cols[i].indicators = NULL;
    }
    FreeColumns(r->cols, r->ncols);
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
  r->rows_seen_exact = true;
  r->opts = *opts;
  if (r->opts.batch_size <= 0) r->opts.batch_size = ADBC_ODBC_DEFAULT_BATCH_SIZE;
  if (r->opts.max_bind_bytes <= 0) r->opts.max_bind_bytes = ADBC_ODBC_DEFAULT_MAX_BIND_BYTES;
  if (r->opts.long_bind_bytes <= 0) r->opts.long_bind_bytes = ADBC_ODBC_DEFAULT_LONG_BIND_BYTES;
  if (r->opts.rowset_bytes <= 0) r->opts.rowset_bytes = ADBC_ODBC_DEFAULT_ROWSET_BYTES;
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
