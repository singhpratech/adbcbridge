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

// Text-carrying ODBC calls, in UTF-8.
//
// Every string this driver hands to ODBC -- statement text, catalog names -- and every
// string it takes back -- column names, type names, diagnostics -- is UTF-8.  unixODBC
// and iODBC pass a narrow `char*` through to the driver as the bytes it is, so on Linux
// and macOS the narrow entry points (SQLExecDirect, SQLDescribeCol, ...) carry UTF-8
// end to end.  The Windows driver manager does not: it transcodes narrow text from the
// process's ANSI code page (1252 on a Western install) to UTF-16 before the driver sees
// it, and back on the way out.  A UTF-8 "héllo" read as cp1252 is "hÃ©llo", so a string
// literal in statement text was stored double-encoded, `WHERE s = 'héllo'` matched
// nothing, a column named "prix_€" came back as a byte 0x80 that is not UTF-8, and
// anything outside cp1252 was best-fit mapped and lost.  Bound parameters and fetched
// columns were never affected -- they are SQL_C_WCHAR -- which is what hid it.
//
// So on Windows every such call goes through its W entry point with UTF-16 on both
// sides, converted here; everywhere else these are the narrow calls, unchanged.

#include "odbc_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)

SQLRETURN OdbcExecDirectUtf8(SQLHSTMT hstmt, const char* sql) {
  return SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS);
}

SQLRETURN OdbcPrepareUtf8(SQLHSTMT hstmt, const char* sql) {
  return SQLPrepare(hstmt, (SQLCHAR*)sql, SQL_NTS);
}

SQLRETURN OdbcDescribeColUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, char* name, SQLSMALLINT name_cap,
                              SQLSMALLINT* name_len, SQLSMALLINT* type, SQLULEN* size,
                              SQLSMALLINT* digits, SQLSMALLINT* nullable) {
  return SQLDescribeCol(hstmt, col, (SQLCHAR*)name, name_cap, name_len, type, size, digits,
                        nullable);
}

SQLRETURN OdbcColAttributeStrUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, SQLUSMALLINT field,
                                  char* buf, SQLSMALLINT cap, SQLSMALLINT* len) {
  return SQLColAttribute(hstmt, col, field, buf, cap, len, NULL);
}

SQLRETURN OdbcGetDiagRecUtf8(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec,
                             SQLCHAR* state, SQLINTEGER* native, char* msg, SQLSMALLINT cap,
                             SQLSMALLINT* len) {
  return SQLGetDiagRec(handle_type, handle, rec, state, native, (SQLCHAR*)msg, cap, len);
}

SQLRETURN OdbcTablesUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                         SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                         const char* type, SQLSMALLINT type_len) {
  return SQLTables(hstmt, (SQLCHAR*)cat, cat_len, (SQLCHAR*)sch, sch_len, (SQLCHAR*)tbl, tbl_len,
                   (SQLCHAR*)type, type_len);
}

SQLRETURN OdbcColumnsUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                          SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                          const char* col, SQLSMALLINT col_len) {
  return SQLColumns(hstmt, (SQLCHAR*)cat, cat_len, (SQLCHAR*)sch, sch_len, (SQLCHAR*)tbl,
                    tbl_len, (SQLCHAR*)col, col_len);
}

SQLRETURN OdbcPrimaryKeysUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len,
                              const char* sch, SQLSMALLINT sch_len, const char* tbl,
                              SQLSMALLINT tbl_len) {
  return SQLPrimaryKeys(hstmt, (SQLCHAR*)cat, cat_len, (SQLCHAR*)sch, sch_len, (SQLCHAR*)tbl,
                        tbl_len);
}

SQLRETURN OdbcForeignKeysUtf8(SQLHSTMT hstmt, const char* fcat, SQLSMALLINT fcat_len,
                              const char* fsch, SQLSMALLINT fsch_len, const char* ftbl,
                              SQLSMALLINT ftbl_len) {
  return SQLForeignKeys(hstmt, NULL, 0, NULL, 0, NULL, 0, (SQLCHAR*)fcat, fcat_len,
                        (SQLCHAR*)fsch, fsch_len, (SQLCHAR*)ftbl, ftbl_len);
}

#else  // _WIN32

// UTF-8 -> UTF-16 units.  Self-contained rather than OdbcUtf8ToUtf16Into (odbc_bind.c),
// because this file is also linked into the C unit tests, which do not carry
// odbc_bind.c.  A malformed byte becomes U+FFFD and the scan moves on one byte, so
// bad input can only shorten the output, never overrun it.
static size_t Utf8ToUtf16(SQLWCHAR* o, const char* s, size_t n) {
  size_t i = 0, u = 0;
  while (i < n) {
    unsigned char b = (unsigned char)s[i];
    uint32_t c;
    size_t k;
    if (b < 0x80) { c = b; k = 1; }
    else if ((b & 0xE0) == 0xC0 && i + 1 < n) { c = ((b & 0x1F) << 6) | (s[i + 1] & 0x3F); k = 2; }
    else if ((b & 0xF0) == 0xE0 && i + 2 < n) {
      c = ((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); k = 3;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < n) {
      c = ((b & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      k = 4;
    } else { c = 0xFFFD; k = 1; }
    if (c >= 0x10000) {
      c -= 0x10000;
      o[u++] = (SQLWCHAR)(0xD800 + (c >> 10));
      o[u++] = (SQLWCHAR)(0xDC00 + (c & 0x3FF));
    } else {
      o[u++] = (SQLWCHAR)c;
    }
    i += k;
  }
  return u;
}

// UTF-8 -> UTF-16, NUL-terminated, malloc'd; NULL stays NULL (a NULL catalog argument
// means "no restriction" and must be passed through as NULL).  `len` is SQL_NTS or a
// byte count.
static SQLWCHAR* ToW(const char* s, SQLSMALLINT len, SQLSMALLINT* out_len) {
  if (!s) {
    if (out_len) *out_len = 0;
    return NULL;
  }
  size_t n = len == SQL_NTS ? strlen(s) : (size_t)len;
  // Every UTF-8 byte yields at most one UTF-16 unit.
  SQLWCHAR* w = malloc((n + 1) * sizeof(SQLWCHAR));
  if (!w) return NULL;
  size_t units = Utf8ToUtf16(w, s, n);
  w[units] = 0;
  if (out_len) *out_len = (SQLSMALLINT)units;
  return w;
}

// UTF-16 -> UTF-8 into a caller's buffer, NUL-terminated and truncated to fit.  Returns
// the length the full text would have, the way ODBC's own out-lengths do.
static SQLSMALLINT FromW(const SQLWCHAR* w, size_t units, char* out, size_t cap) {
  size_t o = 0, full = 0;
  for (size_t i = 0; i < units; i++) {
    uint32_t c = w[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
      i++;
    }
    unsigned char b[4];
    size_t k;
    if (c < 0x80) {
      b[0] = (unsigned char)c;
      k = 1;
    } else if (c < 0x800) {
      b[0] = (unsigned char)(0xC0 | (c >> 6));
      b[1] = (unsigned char)(0x80 | (c & 0x3F));
      k = 2;
    } else if (c < 0x10000) {
      b[0] = (unsigned char)(0xE0 | (c >> 12));
      b[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
      b[2] = (unsigned char)(0x80 | (c & 0x3F));
      k = 3;
    } else {
      b[0] = (unsigned char)(0xF0 | (c >> 18));
      b[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
      b[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
      b[3] = (unsigned char)(0x80 | (c & 0x3F));
      k = 4;
    }
    full += k;
    if (out && o + k < cap) {
      memcpy(out + o, b, k);
      o += k;
    }
  }
  if (out && cap) out[o] = '\0';
  return (SQLSMALLINT)(full > 32000 ? 32000 : full);
}

SQLRETURN OdbcExecDirectUtf8(SQLHSTMT hstmt, const char* sql) {
  SQLWCHAR* w = ToW(sql, SQL_NTS, NULL);
  if (!w) return SQL_ERROR;
  SQLRETURN r = SQLExecDirectW(hstmt, w, SQL_NTS);
  free(w);
  return r;
}

SQLRETURN OdbcPrepareUtf8(SQLHSTMT hstmt, const char* sql) {
  SQLWCHAR* w = ToW(sql, SQL_NTS, NULL);
  if (!w) return SQL_ERROR;
  SQLRETURN r = SQLPrepareW(hstmt, w, SQL_NTS);
  free(w);
  return r;
}

SQLRETURN OdbcDescribeColUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, char* name, SQLSMALLINT name_cap,
                              SQLSMALLINT* name_len, SQLSMALLINT* type, SQLULEN* size,
                              SQLSMALLINT* digits, SQLSMALLINT* nullable) {
  SQLWCHAR wname[1024];
  SQLSMALLINT wlen = 0;
  SQLRETURN r = SQLDescribeColW(hstmt, col, wname, (SQLSMALLINT)(sizeof(wname) / sizeof(wname[0])),
                                &wlen, type, size, digits, nullable);
  if (SQL_SUCCEEDED(r)) {
    size_t units = wlen < 0 ? 0 : (size_t)wlen;
    if (units >= sizeof(wname) / sizeof(wname[0])) units = sizeof(wname) / sizeof(wname[0]) - 1;
    SQLSMALLINT full = FromW(wname, units, name, (size_t)name_cap);
    if (name_len) *name_len = full;
  } else if (name && name_cap > 0) {
    name[0] = '\0';
  }
  return r;
}

SQLRETURN OdbcColAttributeStrUtf8(SQLHSTMT hstmt, SQLUSMALLINT col, SQLUSMALLINT field,
                                  char* buf, SQLSMALLINT cap, SQLSMALLINT* len) {
  SQLWCHAR wbuf[1024];
  SQLSMALLINT wbytes = 0;  // SQLColAttributeW reports bytes, not characters
  SQLRETURN r = SQLColAttributeW(hstmt, col, field, wbuf, (SQLSMALLINT)sizeof(wbuf), &wbytes, NULL);
  if (SQL_SUCCEEDED(r)) {
    size_t units = wbytes < 0 ? 0 : (size_t)wbytes / sizeof(SQLWCHAR);
    if (units >= sizeof(wbuf) / sizeof(wbuf[0])) units = sizeof(wbuf) / sizeof(wbuf[0]) - 1;
    SQLSMALLINT full = FromW(wbuf, units, buf, (size_t)cap);
    if (len) *len = full;
  } else if (buf && cap > 0) {
    buf[0] = '\0';
  }
  return r;
}

SQLRETURN OdbcGetDiagRecUtf8(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec,
                             SQLCHAR* state, SQLINTEGER* native, char* msg, SQLSMALLINT cap,
                             SQLSMALLINT* len) {
  SQLWCHAR wstate[6] = {0};
  SQLWCHAR wmsg[SQL_MAX_MESSAGE_LENGTH];
  SQLSMALLINT wlen = 0;
  SQLRETURN r = SQLGetDiagRecW(handle_type, handle, rec, wstate, native, wmsg,
                               (SQLSMALLINT)(sizeof(wmsg) / sizeof(wmsg[0])), &wlen);
  if (SQL_SUCCEEDED(r)) {
    if (state) {
      for (int i = 0; i < 5; i++) state[i] = (SQLCHAR)wstate[i];
      state[5] = 0;
    }
    size_t units = wlen < 0 ? 0 : (size_t)wlen;
    if (units >= sizeof(wmsg) / sizeof(wmsg[0])) units = sizeof(wmsg) / sizeof(wmsg[0]) - 1;
    SQLSMALLINT full = FromW(wmsg, units, msg, (size_t)cap);
    if (len) *len = full;
  }
  return r;
}

#define W_ARG(name) SQLSMALLINT name##_wlen = 0; SQLWCHAR* name##_w = ToW(name, name##_len, &name##_wlen)

SQLRETURN OdbcTablesUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                         SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                         const char* type, SQLSMALLINT type_len) {
  W_ARG(cat); W_ARG(sch); W_ARG(tbl); W_ARG(type);
  SQLRETURN r = SQLTablesW(hstmt, cat_w, cat_wlen, sch_w, sch_wlen, tbl_w, tbl_wlen, type_w,
                           type_wlen);
  free(cat_w); free(sch_w); free(tbl_w); free(type_w);
  return r;
}

SQLRETURN OdbcColumnsUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len, const char* sch,
                          SQLSMALLINT sch_len, const char* tbl, SQLSMALLINT tbl_len,
                          const char* col, SQLSMALLINT col_len) {
  W_ARG(cat); W_ARG(sch); W_ARG(tbl); W_ARG(col);
  SQLRETURN r = SQLColumnsW(hstmt, cat_w, cat_wlen, sch_w, sch_wlen, tbl_w, tbl_wlen, col_w,
                            col_wlen);
  free(cat_w); free(sch_w); free(tbl_w); free(col_w);
  return r;
}

SQLRETURN OdbcPrimaryKeysUtf8(SQLHSTMT hstmt, const char* cat, SQLSMALLINT cat_len,
                              const char* sch, SQLSMALLINT sch_len, const char* tbl,
                              SQLSMALLINT tbl_len) {
  W_ARG(cat); W_ARG(sch); W_ARG(tbl);
  SQLRETURN r = SQLPrimaryKeysW(hstmt, cat_w, cat_wlen, sch_w, sch_wlen, tbl_w, tbl_wlen);
  free(cat_w); free(sch_w); free(tbl_w);
  return r;
}

SQLRETURN OdbcForeignKeysUtf8(SQLHSTMT hstmt, const char* fcat, SQLSMALLINT fcat_len,
                              const char* fsch, SQLSMALLINT fsch_len, const char* ftbl,
                              SQLSMALLINT ftbl_len) {
  W_ARG(fcat); W_ARG(fsch); W_ARG(ftbl);
  SQLRETURN r = SQLForeignKeysW(hstmt, NULL, 0, NULL, 0, NULL, 0, fcat_w, fcat_wlen, fsch_w,
                                fsch_wlen, ftbl_w, ftbl_wlen);
  free(fcat_w); free(fsch_w); free(ftbl_w);
  return r;
}

#endif  // _WIN32
