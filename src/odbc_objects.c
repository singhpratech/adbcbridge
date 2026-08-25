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

// ConnectionGetObjects: ODBC catalog functions -> ADBC nested objects schema.

#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// ---------------------------------------------------------------------------
// Small helpers

static char* GetStrCol(const struct OdbcConnection* conn, SQLHSTMT hstmt, SQLUSMALLINT col) {
  // Zeroed: a minimal driver (MDB Tools) can answer SQL_SUCCESS for an empty value
  // without writing so much as the terminator.
  char buf[2048] = {0};
  SQLLEN ind = 0;
  SQLRETURN ret = OdbcGetDataStrUtf8(hstmt, col, buf, sizeof(buf), &ind, conn->reader_opts.sqllen_32bit);
  if (!SQL_SUCCEEDED(ret) || ind == SQL_NULL_DATA) return NULL;
  return strdup(buf);
}

static bool GetIntCol(const struct OdbcConnection* conn, SQLHSTMT hstmt, SQLUSMALLINT col,
                      int64_t* out) {
  SQLBIGINT v = 0;
  SQLLEN ind = 0;
  SQLRETURN ret = OdbcGetData(hstmt, col, SQL_C_SBIGINT, &v, sizeof(v), &ind,
                              conn->reader_opts.sqllen_32bit);
  if (!SQL_SUCCEEDED(ret) || ind == SQL_NULL_DATA) return false;
  *out = (int64_t)v;
  return true;
}

// Like GetStrCol, but for catalog/schema names: drivers for backends without
// catalogs or schemas report the absent name as either NULL or an empty string,
// and ADBC wants NULL for both.
static char* GetNameCol(const struct OdbcConnection* conn, SQLHSTMT hstmt, SQLUSMALLINT col) {
  char* s = GetStrCol(conn, hstmt, col);
  if (s && !*s) { free(s); return NULL; }
  return s;
}

static ArrowErrorCode AppendStrOrNull(struct ArrowArray* a, const char* s) {
  if (!s) return ArrowArrayAppendNull(a, 1);
  return ArrowArrayAppendString(a, ArrowCharView(s));
}

// For catalog/schema fields: an empty string means "no catalog/schema", i.e. NULL.
static ArrowErrorCode AppendNameOrNull(struct ArrowArray* a, const char* s) {
  if (!s || !*s) return ArrowArrayAppendNull(a, 1);
  return ArrowArrayAppendString(a, ArrowCharView(s));
}

static int StrCmpNull(const char* a, const char* b) {
  if (!a && !b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}

static SQLCHAR* Pat(const char* s) { return (SQLCHAR*)s; }
static SQLSMALLINT PatLen(const char* s) { return s ? SQL_NTS : 0; }

// ---------------------------------------------------------------------------
// SQL LIKE pattern matching, applied client-side to catalog/schema names.
//
// ODBC drivers are permitted to ignore the catalog/schema patterns passed to
// SQLTables (SQLiteODBC ignores both), so the rows that come back are not
// necessarily filtered.  Matching is ASCII case-insensitive: LIKE case folding
// is backend-specific, and being permissive here only risks keeping a row the
// driver already chose to return, never dropping one it filtered on purpose.

static char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static bool LikeMatch(const char* pat, const char* s) {
  const char* star_pat = NULL;
  const char* star_s = NULL;
  while (*s) {
    if (*pat == '%') {
      star_pat = ++pat;
      star_s = s;
      continue;
    }
    if (*pat == '_') {
      pat++;
      s++;
      continue;
    }
    // Backslash escapes the next character, so "\%" matches a literal '%'.
    const char* lit = (*pat == '\\' && pat[1]) ? pat + 1 : pat;
    if (*lit && AsciiLower(*lit) == AsciiLower(*s)) {
      pat = lit + 1;
      s++;
      continue;
    }
    if (star_pat) {
      pat = star_pat;
      s = ++star_s;
      continue;
    }
    return false;
  }
  while (*pat == '%') pat++;
  return *pat == '\0';
}

// ADBC filter semantics: a NULL filter means "no filtering"; an empty-string
// filter means "the unnamed catalog/schema".  A LIKE pattern never matches a
// name that does not exist, so a NULL name is kept only by a NULL filter.
static bool NameMatches(const char* pattern, const char* name) {
  if (!pattern) return true;
  if (!*pattern) return !name || !*name;
  if (!name) return false;
  return LikeMatch(pattern, name);
}

struct TableRow {
  char* catalog;
  char* schema;
  char* table;
  char* type;
};

static int TableRowCmp(const void* a, const void* b) {
  const struct TableRow* x = a;
  const struct TableRow* y = b;
  int c = StrCmpNull(x->catalog, y->catalog);
  if (c) return c;
  c = StrCmpNull(x->schema, y->schema);
  if (c) return c;
  return StrCmpNull(x->table, y->table);
}

// ---------------------------------------------------------------------------
// Column + constraint emission for one table

// Emit one column into table_columns[].  `num`/`has` are indexed by SQLColumns result
// column number, so both the SQLColumns path and the SQLDescribeCol fallback below fill
// the same slots; the strings are consumed (freed) here.
static AdbcStatusCode EmitColumn(struct ArrowArray* c, char* name, char* type_name,
                                 char* remarks, char* column_def, char* is_nullable,
                                 const int64_t* num, const bool* has, struct AdbcError* error) {
  CHECK_NA(INTERNAL, AppendStrOrNull(c->children[0], name ? name : ""), error);
  free(name);
  // ordinal_position
  if (has[17]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[1], num[17]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[1], 1), error);
  CHECK_NA(INTERNAL, AppendStrOrNull(c->children[2], remarks), error); free(remarks);
  if (has[5]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[3], num[5]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[3], 1), error);
  CHECK_NA(INTERNAL, AppendStrOrNull(c->children[4], type_name), error); free(type_name);
  if (has[7]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[5], num[7]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[5], 1), error);
  if (has[9]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[6], num[9]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[6], 1), error);
  if (has[10]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[7], num[10]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[7], 1), error);
  if (has[11]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[8], num[11]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[8], 1), error);
  CHECK_NA(INTERNAL, AppendStrOrNull(c->children[9], column_def), error); free(column_def);
  if (has[14]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[10], num[14]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[10], 1), error);
  if (has[15]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[11], num[15]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[11], 1), error);
  if (has[16]) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[12], num[16]), error);
  else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[12], 1), error);
  CHECK_NA(INTERNAL, AppendStrOrNull(c->children[13], is_nullable), error); free(is_nullable);
  // scope catalog/schema/table, autoincrement, generated: unknown via SQLColumns
  for (int k = 14; k <= 18; k++) CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[k], 1), error);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
  return ADBC_STATUS_OK;
}

// Execute "SELECT * FROM <table> WHERE 1=0" on `hstmt`, so the columns can be read off
// the result set with SQLDescribeCol.  Tries the most qualified name first and drops one
// qualifier at a time: a driver whose SQLColumns just failed is not one to trust about
// how much qualification the server accepts (QuestDB rejects the catalog its own
// SQLTables reports).  Returns false if no spelling executes.
static bool DescribeTableStmt(struct OdbcConnection* conn, const struct TableRow* t,
                              SQLHSTMT hstmt) {
  char q[8];
  OdbcQuoteChar(conn->hdbc, q);
  const bool have_cat = t->catalog && *t->catalog, have_sch = t->schema && *t->schema;
  for (int level = 0; level < 3; level++) {
    if (level == 0 && !have_cat) continue;  // same query as level 1
    if (level == 1 && !have_sch) continue;  // same query as level 2
    struct InternalAdbcStringBuilder sb;
    InternalAdbcStringBuilderInit(&sb, 128);
    InternalAdbcStringBuilderAppend(&sb, "SELECT * FROM ");
    if (level == 0) InternalAdbcStringBuilderAppend(&sb, "%s%s%s.", (char*)q, t->catalog, (char*)q);
    if (level <= 1 && have_sch) {
      InternalAdbcStringBuilderAppend(&sb, "%s%s%s.", (char*)q, t->schema, (char*)q);
    }
    InternalAdbcStringBuilderAppend(&sb, "%s%s%s WHERE 1=0", (char*)q, t->table, (char*)q);
    SQLRETURN ret = OdbcExecDirectSql(hstmt, sb.buffer, conn->reader_opts.narrow_sql);
    InternalAdbcStringBuilderReset(&sb);
    if (SQL_SUCCEEDED(ret)) return true;
    SQLFreeStmt(hstmt, SQL_CLOSE);
  }
  return false;
}

// Fallback for a driver whose SQLColumns fails outright: describe the result set of a
// zero-row SELECT instead, which is what GetTableSchema does anyway.  psqlodbc builds
// SQLColumns from a pg_catalog query and binds pg_attribute.attidentity with a NULL
// StrLen_or_IndPtr, so any PostgreSQL-wire server that reports that column as NULL
// rather than as the empty string -- QuestDB does -- fails the whole call with
// "Unrecognized return value from copy_and_convert_field".  SQLDescribeCol knows less
// than SQLColumns (no remarks, no default, no radix), so the fields it cannot answer are
// left NULL rather than guessed.
static AdbcStatusCode AppendColumnsViaDescribe(struct OdbcConnection* conn,
                                               struct ArrowArray* cols_list,
                                               const struct TableRow* t, const char* column_name,
                                               struct AdbcError* error) {
  struct ArrowArray* c = cols_list->children[0];
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt))) return ADBC_STATUS_IO;
  if (!DescribeTableStmt(conn, t, hstmt)) {
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return ADBC_STATUS_IO;
  }
  SQLSMALLINT ncols = 0;
  if (!SQL_SUCCEEDED(SQLNumResultCols(hstmt, &ncols))) ncols = 0;
  for (SQLSMALLINT i = 1; i <= ncols; i++) {
    SQLCHAR name[512] = {0};
    SQLSMALLINT name_len = 0, data_type = 0, decimal_digits = 0, nullable = SQL_NULLABLE_UNKNOWN;
    SQLULEN column_size = 0;
    if (!SQL_SUCCEEDED(OdbcDescribeColUtf8(hstmt, (SQLUSMALLINT)i, (char*)name, sizeof(name), &name_len,
                                           &data_type, &column_size, &decimal_digits, &nullable))) {
      continue;
    }
    if (column_name && !LikeMatch(column_name, (const char*)name)) continue;
    int64_t num[19] = {0};
    bool has[19] = {false};
    num[5] = data_type;  has[5] = true;   // DATA_TYPE
    num[7] = (int64_t)OdbcReadULen(&column_size, conn->reader_opts.sqllen_32bit);
    has[7] = num[7] > 0;                  // COLUMN_SIZE
    num[9] = decimal_digits; has[9] = true;   // DECIMAL_DIGITS
    num[11] = nullable; has[11] = true;       // NULLABLE
    num[14] = data_type; has[14] = true;      // SQL_DATA_TYPE
    num[17] = i; has[17] = true;              // ORDINAL_POSITION
    SQLCHAR type_name[256] = {0};
    SQLSMALLINT type_name_len = 0;
    if (!SQL_SUCCEEDED(OdbcColAttributeStrUtf8(hstmt, (SQLUSMALLINT)i, SQL_DESC_TYPE_NAME, (char*)type_name,
                                               sizeof(type_name), &type_name_len))) {
      type_name[0] = '\0';
    }
    const char* yes_no = nullable == SQL_NO_NULLS ? "NO" : nullable == SQL_NULLABLE ? "YES" : NULL;
    RAISE_ADBC(EmitColumn(c, strdup((const char*)name),
                          type_name[0] ? strdup((const char*)type_name) : NULL, NULL, NULL,
                          yes_no ? strdup(yes_no) : NULL, num, has, error));
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(cols_list), error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode AppendColumns(struct OdbcConnection* conn, struct ArrowArray* cols_list,
                                    const struct TableRow* t, const char* column_name,
                                    struct AdbcError* error) {
  // no_sql_columns: this driver's SQLColumns result set cannot be fetched at all, so
  // there is no return code to fall back on -- the call has to be skipped outright and
  // the describe fallback used unconditionally.  See OdbcDetectQuirks().
  if (conn->reader_opts.no_sql_columns) {
    return AppendColumnsViaDescribe(conn, cols_list, t, column_name, error);
  }
  struct ArrowArray* c = cols_list->children[0];
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret = OdbcColumnsUtf8(hstmt, (const char*)Pat(t->catalog), PatLen(t->catalog),
                                  (const char*)Pat(t->schema), PatLen(t->schema),
                                  (const char*)Pat(t->table), SQL_NTS, (const char*)Pat(column_name),
                                  PatLen(column_name));
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLColumns", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    // The driver cannot list this table's columns; describing a zero-row SELECT can.
    // Keep the SQLColumns diagnostic if that fails too.
    struct AdbcError ignored = ADBC_ERROR_INIT;
    if (AppendColumnsViaDescribe(conn, cols_list, t, column_name, &ignored) == ADBC_STATUS_OK) {
      if (error && error->release) error->release(error);
      return ADBC_STATUS_OK;
    }
    if (ignored.release) ignored.release(&ignored);
    return s;
  }
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    // Read the whole row first, in ascending column order: SQLGetData may only
    // revisit an earlier column when the driver advertises SQL_GD_ANY_ORDER,
    // and msodbcsql18 does not.  `has[i]` records whether column i was non-NULL.
    char* name = GetStrCol(conn, hstmt, 4);              // COLUMN_NAME
    int64_t num[19] = {0};
    bool has[19] = {false};
    has[5] = GetIntCol(conn, hstmt, 5, &num[5]);         // DATA_TYPE
    char* type_name = GetStrCol(conn, hstmt, 6);         // TYPE_NAME
    has[7] = GetIntCol(conn, hstmt, 7, &num[7]);         // COLUMN_SIZE
    has[9] = GetIntCol(conn, hstmt, 9, &num[9]);         // DECIMAL_DIGITS
    has[10] = GetIntCol(conn, hstmt, 10, &num[10]);      // NUM_PREC_RADIX
    has[11] = GetIntCol(conn, hstmt, 11, &num[11]);      // NULLABLE
    char* remarks = GetStrCol(conn, hstmt, 12);          // REMARKS
    char* column_def = GetStrCol(conn, hstmt, 13);       // COLUMN_DEF
    has[14] = GetIntCol(conn, hstmt, 14, &num[14]);      // SQL_DATA_TYPE
    has[15] = GetIntCol(conn, hstmt, 15, &num[15]);      // SQL_DATETIME_SUB
    has[16] = GetIntCol(conn, hstmt, 16, &num[16]);      // CHAR_OCTET_LENGTH
    has[17] = GetIntCol(conn, hstmt, 17, &num[17]);      // ORDINAL_POSITION
    char* is_nullable = GetStrCol(conn, hstmt, 18);      // IS_NULLABLE
    RAISE_ADBC(EmitColumn(c, name, type_name, remarks, column_def, is_nullable, num, has, error));
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(cols_list), error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode AppendConstraints(struct OdbcConnection* conn, struct ArrowArray* cons_list,
                                        const struct TableRow* t, struct AdbcError* error) {
  struct ArrowArray* c = cons_list->children[0];
  struct ArrowArray* names = c->children[2];
  struct ArrowArray* usage = c->children[3];
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);

  // Primary key (one constraint, columns ordered by KEY_SEQ as returned).
  SQLRETURN ret = OdbcPrimaryKeysUtf8(hstmt, (const char*)Pat(t->catalog), PatLen(t->catalog),
                                      (const char*)Pat(t->schema), PatLen(t->schema),
                                      (const char*)Pat(t->table), SQL_NTS);
  if (SQL_SUCCEEDED(ret)) {
    int n = 0;
    char* pk_name = NULL;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      char* col = GetStrCol(conn, hstmt, 4);
      if (!pk_name) pk_name = GetStrCol(conn, hstmt, 6);
      CHECK_NA(INTERNAL, AppendStrOrNull(names->children[0], col ? col : ""), error);
      free(col);
      n++;
    }
    if (n > 0) {
      CHECK_NA(INTERNAL, AppendStrOrNull(c->children[0], pk_name), error);
      CHECK_NA(INTERNAL, ArrowArrayAppendString(c->children[1], ArrowCharView("PRIMARY KEY")), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(names), error);
      // constraint_column_usage is nullable precisely so that non-foreign-key
      // constraints can say "not applicable"; an empty list would instead mean
      // "a foreign key that references nothing".
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(usage, 1), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
    }
    free(pk_name);
  }
  SQLFreeStmt(hstmt, SQL_CLOSE);

  // Foreign keys: rows grouped by FK_NAME (col 12), ordered by KEY_SEQ.
  ret = OdbcForeignKeysUtf8(hstmt, (const char*)Pat(t->catalog), PatLen(t->catalog),
                            (const char*)Pat(t->schema), PatLen(t->schema),
                            (const char*)Pat(t->table), SQL_NTS);
  if (SQL_SUCCEEDED(ret)) {
    char* cur_name = NULL;
    bool open = false;
    int64_t seq_prev = 0;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      // Read every column of the row up front, in ascending order: SQLGetData
      // may only jump around when the driver advertises SQL_GD_ANY_ORDER, and
      // msodbcsql18 does not.
      // PKTABLE_CAT / PKTABLE_SCHEM come back as "" from drivers whose backend
      // has no catalogs or schemas; ADBC wants NULL there, as for
      // TABLE_CAT/TABLE_SCHEM.
      char* pk_cat = GetStrCol(conn, hstmt, 1);
      char* pk_sch = GetStrCol(conn, hstmt, 2);
      char* pk_tbl = GetStrCol(conn, hstmt, 3);
      char* pk_col = GetStrCol(conn, hstmt, 4);
      char* fkcol = GetStrCol(conn, hstmt, 8);
      int64_t seq = 0;
      GetIntCol(conn, hstmt, 9, &seq);
      char* fk_name = GetStrCol(conn, hstmt, 12);
      bool new_group = !open || StrCmpNull(fk_name, cur_name) != 0 || seq <= seq_prev;
      if (new_group) {
        if (open) {
          CHECK_NA(INTERNAL, ArrowArrayFinishElement(names), error);
          CHECK_NA(INTERNAL, ArrowArrayFinishElement(usage), error);
          CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
        }
        CHECK_NA(INTERNAL, AppendStrOrNull(c->children[0], fk_name), error);
        CHECK_NA(INTERNAL, ArrowArrayAppendString(c->children[1], ArrowCharView("FOREIGN KEY")), error);
        free(cur_name);
        cur_name = fk_name ? strdup(fk_name) : NULL;
        open = true;
      }
      seq_prev = seq;
      CHECK_NA(INTERNAL, AppendStrOrNull(names->children[0], fkcol ? fkcol : ""), error);
      struct ArrowArray* u = usage->children[0];
      CHECK_NA(INTERNAL, AppendNameOrNull(u->children[0], pk_cat), error);
      CHECK_NA(INTERNAL, AppendNameOrNull(u->children[1], pk_sch), error);
      CHECK_NA(INTERNAL, AppendStrOrNull(u->children[2], pk_tbl ? pk_tbl : ""), error);
      CHECK_NA(INTERNAL, AppendStrOrNull(u->children[3], pk_col ? pk_col : ""), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(u), error);
      free(pk_cat); free(pk_sch); free(pk_tbl); free(pk_col);
      free(fkcol);
      free(fk_name);
    }
    if (open) {
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(names), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(usage), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
    }
    free(cur_name);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  CHECK_NA(INTERNAL, ArrowArrayFinishElement(cons_list), error);
  return ADBC_STATUS_OK;
}

// ---------------------------------------------------------------------------

static void FreeRows(struct TableRow* rows, size_t n) {
  for (size_t i = 0; i < n; i++) {
    free(rows[i].catalog); free(rows[i].schema); free(rows[i].table); free(rows[i].type);
  }
  free(rows);
}

struct RowList {
  struct TableRow* rows;
  size_t n;
  size_t cap;
};

// Takes ownership of all four strings, even on failure.
static bool RowListPush(struct RowList* l, char* catalog, char* schema, char* table, char* type) {
  if (l->n == l->cap) {
    size_t cap = l->cap ? l->cap * 2 : 64;
    struct TableRow* p = realloc(l->rows, cap * sizeof(*p));
    if (!p) {
      free(catalog); free(schema); free(table); free(type);
      return false;
    }
    l->rows = p;
    l->cap = cap;
  }
  l->rows[l->n].catalog = catalog;
  l->rows[l->n].schema = schema;
  l->rows[l->n].table = table;
  l->rows[l->n].type = type;
  l->n++;
  return true;
}

// One SQLTables() call, appending its rows to `out`.  `tolerate_failure` is for
// the SQL_ALL_CATALOGS / SQL_ALL_SCHEMAS probes: drivers whose backend has
// neither may reject them outright, which is not an error for us.
static AdbcStatusCode FetchTables(struct OdbcConnection* conn, SQLCHAR* cat, SQLSMALLINT cat_len,
                                  SQLCHAR* sch, SQLSMALLINT sch_len, SQLCHAR* tbl,
                                  SQLSMALLINT tbl_len, SQLCHAR* type, SQLSMALLINT type_len,
                                  bool tolerate_failure, struct RowList* out,
                                  struct AdbcError* error) {
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret = OdbcTablesUtf8(hstmt, (const char*)cat, cat_len, (const char*)sch, sch_len,
                                 (const char*)tbl, tbl_len, (const char*)type, type_len);
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = ADBC_STATUS_OK;
    if (!tolerate_failure) s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLTables", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return s;
  }
  AdbcStatusCode status = ADBC_STATUS_OK;
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    // SQLGetData must be issued in ascending column order unless the driver
    // advertises SQL_GD_ANY_ORDER (msodbcsql18 does not), and C leaves the
    // evaluation order of call arguments unspecified -- so read the four
    // columns into locals, in order, before handing them over.
    char* row_cat = GetNameCol(conn, hstmt, 1);
    char* row_sch = GetNameCol(conn, hstmt, 2);
    char* row_tbl = GetStrCol(conn, hstmt, 3);
    char* row_type = GetStrCol(conn, hstmt, 4);
    if (!RowListPush(out, row_cat, row_sch, row_tbl, row_type)) {
      InternalAdbcSetError(error, "out of memory");
      status = ADBC_STATUS_INTERNAL;
      break;
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return status;
}

// SQL_ATTR_CURRENT_CATALOG, or NULL when the backend has no catalogs (drivers
// report that as either a failure or an empty string).
static char* CurrentCatalog(struct OdbcConnection* conn) {
  SQLCHAR buf[1024] = {0};
  SQLINTEGER len = 0;
  if (!SQL_SUCCEEDED(
          SQLGetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, buf, sizeof(buf), &len)))
    return NULL;
  if (buf[0] == '\0') return NULL;
  return strdup((const char*)buf);
}

// Drop the fields below the requested depth so the rows dedupe cleanly.
static void TruncateRows(struct RowList* l, int depth) {
  for (size_t i = 0; i < l->n; i++) {
    if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
      free(l->rows[i].schema);
      l->rows[i].schema = NULL;
    }
    free(l->rows[i].table);
    free(l->rows[i].type);
    l->rows[i].table = NULL;
    l->rows[i].type = NULL;
  }
}

static void SortAndDedupe(struct RowList* l) {
  if (l->n > 1) qsort(l->rows, l->n, sizeof(*l->rows), TableRowCmp);
  size_t w = 0;
  for (size_t i = 0; i < l->n; i++) {
    if (w > 0 && TableRowCmp(&l->rows[w - 1], &l->rows[i]) == 0) {
      free(l->rows[i].catalog); free(l->rows[i].schema);
      free(l->rows[i].table); free(l->rows[i].type);
      continue;
    }
    l->rows[w++] = l->rows[i];
  }
  l->n = w;
}

static AdbcStatusCode CollectTables(struct OdbcConnection* conn, int depth, const char* catalog,
                                    const char* db_schema, const char* table_name,
                                    const char** table_type, struct TableRow** out_rows,
                                    size_t* out_n, struct AdbcError* error) {
  struct RowList l = {0};
  AdbcStatusCode status;
  if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
    status = FetchTables(conn, (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0,
                         (SQLCHAR*)"", 0, /*tolerate_failure=*/true, &l, error);
    if (status == ADBC_STATUS_OK && l.n == 0) {
      // The driver reports no catalog list (SQLiteODBC returns an empty result
      // set); derive the catalogs from the full SQLTables listing instead.
      status = FetchTables(conn, Pat(catalog), PatLen(catalog), NULL, 0, NULL, 0, NULL, 0,
                           /*tolerate_failure=*/false, &l, error);
    }
    TruncateRows(&l, depth);
    // Every connection is in some catalog, even an unnamed one, so the list must
    // never be empty: seed it with SQL_ATTR_CURRENT_CATALOG (NULL if unnamed).
    if (status == ADBC_STATUS_OK && !RowListPush(&l, CurrentCatalog(conn), NULL, NULL, NULL)) {
      InternalAdbcSetError(error, "out of memory");
      status = ADBC_STATUS_INTERNAL;
    }
  } else if (depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS) {
    // SQL_ALL_SCHEMAS reports schemas with a NULL TABLE_CAT, so it cannot answer a
    // catalog-filtered request; use the full listing when a catalog filter is set.
    if (!catalog) {
      status = FetchTables(conn, (SQLCHAR*)"", 0, (SQLCHAR*)SQL_ALL_SCHEMAS, SQL_NTS, (SQLCHAR*)"",
                           0, (SQLCHAR*)"", 0, /*tolerate_failure=*/true, &l, error);
    } else {
      status = ADBC_STATUS_OK;
    }
    if (status == ADBC_STATUS_OK && l.n == 0) {
      status = FetchTables(conn, Pat(catalog), PatLen(catalog), Pat(db_schema), PatLen(db_schema),
                           NULL, 0, NULL, 0, /*tolerate_failure=*/false, &l, error);
    }
    TruncateRows(&l, depth);
    if (status == ADBC_STATUS_OK && l.n == 0 &&
        !RowListPush(&l, CurrentCatalog(conn), NULL, NULL, NULL)) {
      InternalAdbcSetError(error, "out of memory");
      status = ADBC_STATUS_INTERNAL;
    }
  } else {
    char* types = NULL;
    if (table_type && table_type[0]) {
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 64);
      for (int i = 0; table_type[i]; i++) {
        InternalAdbcStringBuilderAppend(&sb, i ? ",%s" : "%s", table_type[i]);
      }
      types = sb.buffer;  // take ownership
    }
    status = FetchTables(conn, Pat(catalog), PatLen(catalog), Pat(db_schema), PatLen(db_schema),
                         Pat(table_name), PatLen(table_name), Pat(types), PatLen(types),
                         /*tolerate_failure=*/false, &l, error);
    free(types);
  }
  if (status != ADBC_STATUS_OK) {
    FreeRows(l.rows, l.n);
    return status;
  }

  // ODBC drivers may ignore the patterns they were handed, so enforce them here as
  // well: SQLiteODBC ignores the catalog and schema ones, and MDB Tools ignores
  // TableName outright, handing back every table in the .mdb file (MSys* system
  // tables included).  A filter only applies at a depth that reports that name.
  const bool has_table = depth != ADBC_OBJECT_DEPTH_CATALOGS &&
                         depth != ADBC_OBJECT_DEPTH_DB_SCHEMAS;
  size_t w = 0;
  for (size_t i = 0; i < l.n; i++) {
    bool keep = NameMatches(catalog, l.rows[i].catalog) &&
                (depth == ADBC_OBJECT_DEPTH_CATALOGS ||
                 NameMatches(db_schema, l.rows[i].schema)) &&
                (!has_table || NameMatches(table_name, l.rows[i].table));
    if (!keep) {
      free(l.rows[i].catalog); free(l.rows[i].schema);
      free(l.rows[i].table); free(l.rows[i].type);
      continue;
    }
    l.rows[w++] = l.rows[i];
  }
  l.n = w;

  SortAndDedupe(&l);
  *out_rows = l.rows;
  *out_n = l.n;
  return ADBC_STATUS_OK;
}

AdbcStatusCode OdbcConnectionGetObjects(struct AdbcConnection* connection, int depth,
                                        const char* catalog, const char* db_schema,
                                        const char* table_name, const char** table_type,
                                        const char* column_name, struct ArrowArrayStream* out,
                                        struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;

  struct TableRow* rows = NULL;
  size_t n = 0;
  RAISE_ADBC(CollectTables(conn, depth, catalog, db_schema, table_name, table_type, &rows, &n, error));

  struct ArrowSchema schema;
  struct ArrowArray array;
  AdbcStatusCode status = InternalAdbcInitConnectionObjectsSchema(&schema, error);
  if (status != ADBC_STATUS_OK) { FreeRows(rows, n); return status; }
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&array, &schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&array), error);

  struct ArrowArray* cat_name = array.children[0];
  struct ArrowArray* cat_schemas = array.children[1];
  struct ArrowArray* sch = cat_schemas->children[0];
  struct ArrowArray* sch_name = sch->children[0];
  struct ArrowArray* sch_tables = sch->children[1];
  struct ArrowArray* tbl = sch_tables->children[0];

  size_t i = 0;
  while (i < n) {
    const char* cur_cat = rows[i].catalog;
    CHECK_NA(INTERNAL, AppendStrOrNull(cat_name, cur_cat), error);
    if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
      CHECK_NA(INTERNAL, ArrowArrayAppendNull(cat_schemas, 1), error);
      while (i < n && StrCmpNull(rows[i].catalog, cur_cat) == 0) i++;
      continue;
    }
    while (i < n && StrCmpNull(rows[i].catalog, cur_cat) == 0) {
      const char* cur_sch = rows[i].schema;
      CHECK_NA(INTERNAL, AppendStrOrNull(sch_name, cur_sch), error);
      if (depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS) {
        CHECK_NA(INTERNAL, ArrowArrayAppendNull(sch_tables, 1), error);
        while (i < n && StrCmpNull(rows[i].catalog, cur_cat) == 0 &&
               StrCmpNull(rows[i].schema, cur_sch) == 0)
          i++;
      } else {
        while (i < n && StrCmpNull(rows[i].catalog, cur_cat) == 0 &&
               StrCmpNull(rows[i].schema, cur_sch) == 0) {
          CHECK_NA(INTERNAL, AppendStrOrNull(tbl->children[0], rows[i].table ? rows[i].table : ""), error);
          CHECK_NA(INTERNAL, AppendStrOrNull(tbl->children[1], rows[i].type ? rows[i].type : ""), error);
          if (depth == ADBC_OBJECT_DEPTH_TABLES) {
            CHECK_NA(INTERNAL, ArrowArrayAppendNull(tbl->children[2], 1), error);
            CHECK_NA(INTERNAL, ArrowArrayAppendNull(tbl->children[3], 1), error);
          } else {
            status = AppendColumns(conn, tbl->children[2], &rows[i], column_name, error);
            if (status == ADBC_STATUS_OK) status = AppendConstraints(conn, tbl->children[3], &rows[i], error);
            if (status != ADBC_STATUS_OK) {
              FreeRows(rows, n);
              ArrowArrayRelease(&array);
              schema.release(&schema);
              return status;
            }
          }
          CHECK_NA(INTERNAL, ArrowArrayFinishElement(tbl), error);
          i++;
        }
        CHECK_NA(INTERNAL, ArrowArrayFinishElement(sch_tables), error);
      }
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(sch), error);
    }
    CHECK_NA(INTERNAL, ArrowArrayFinishElement(cat_schemas), error);
  }
  FreeRows(rows, n);

  array.length = cat_name->length;
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}
