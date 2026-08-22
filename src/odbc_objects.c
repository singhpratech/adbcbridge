// SPDX-License-Identifier: Apache-2.0
// ConnectionGetObjects: ODBC catalog functions -> ADBC nested objects schema.

#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// ---------------------------------------------------------------------------
// Small helpers

static char* GetStrCol(SQLHSTMT hstmt, SQLUSMALLINT col) {
  char buf[2048];
  SQLLEN ind = 0;
  SQLRETURN ret = SQLGetData(hstmt, col, SQL_C_CHAR, buf, sizeof(buf), &ind);
  if (!SQL_SUCCEEDED(ret) || ind == SQL_NULL_DATA) return NULL;
  return strdup(buf);
}

static bool GetIntCol(SQLHSTMT hstmt, SQLUSMALLINT col, int64_t* out) {
  SQLBIGINT v = 0;
  SQLLEN ind = 0;
  SQLRETURN ret = SQLGetData(hstmt, col, SQL_C_SBIGINT, &v, sizeof(v), &ind);
  if (!SQL_SUCCEEDED(ret) || ind == SQL_NULL_DATA) return false;
  *out = (int64_t)v;
  return true;
}

static ArrowErrorCode AppendStrOrNull(struct ArrowArray* a, const char* s) {
  if (!s) return ArrowArrayAppendNull(a, 1);
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

static AdbcStatusCode AppendColumns(struct OdbcConnection* conn, struct ArrowArray* cols_list,
                                    const struct TableRow* t, const char* column_name,
                                    struct AdbcError* error) {
  struct ArrowArray* c = cols_list->children[0];
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret = SQLColumns(hstmt, Pat(t->catalog), PatLen(t->catalog), Pat(t->schema),
                             PatLen(t->schema), Pat(t->table), SQL_NTS, Pat(column_name),
                             PatLen(column_name));
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLColumns", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return s;
  }
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    char* name = GetStrCol(hstmt, 4);
    int64_t v;
    CHECK_NA(INTERNAL, AppendStrOrNull(c->children[0], name ? name : ""), error);
    free(name);
    // ordinal_position
    if (GetIntCol(hstmt, 17, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[1], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[1], 1), error);
    char* s = GetStrCol(hstmt, 12);  // remarks
    CHECK_NA(INTERNAL, AppendStrOrNull(c->children[2], s), error); free(s);
    if (GetIntCol(hstmt, 5, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[3], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[3], 1), error);
    s = GetStrCol(hstmt, 6);  // type name
    CHECK_NA(INTERNAL, AppendStrOrNull(c->children[4], s), error); free(s);
    if (GetIntCol(hstmt, 7, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[5], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[5], 1), error);
    if (GetIntCol(hstmt, 9, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[6], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[6], 1), error);
    if (GetIntCol(hstmt, 10, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[7], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[7], 1), error);
    if (GetIntCol(hstmt, 11, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[8], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[8], 1), error);
    s = GetStrCol(hstmt, 13);  // column_def
    CHECK_NA(INTERNAL, AppendStrOrNull(c->children[9], s), error); free(s);
    if (GetIntCol(hstmt, 14, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[10], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[10], 1), error);
    if (GetIntCol(hstmt, 15, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[11], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[11], 1), error);
    if (GetIntCol(hstmt, 16, &v)) CHECK_NA(INTERNAL, ArrowArrayAppendInt(c->children[12], v), error);
    else CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[12], 1), error);
    s = GetStrCol(hstmt, 18);  // is_nullable
    CHECK_NA(INTERNAL, AppendStrOrNull(c->children[13], s), error); free(s);
    // scope catalog/schema/table, autoincrement, generated: unknown via SQLColumns
    for (int k = 14; k <= 18; k++) CHECK_NA(INTERNAL, ArrowArrayAppendNull(c->children[k], 1), error);
    CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
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
  SQLRETURN ret = SQLPrimaryKeys(hstmt, Pat(t->catalog), PatLen(t->catalog), Pat(t->schema),
                                 PatLen(t->schema), Pat(t->table), SQL_NTS);
  if (SQL_SUCCEEDED(ret)) {
    int n = 0;
    char* pk_name = NULL;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      char* col = GetStrCol(hstmt, 4);
      if (!pk_name) pk_name = GetStrCol(hstmt, 6);
      CHECK_NA(INTERNAL, AppendStrOrNull(names->children[0], col ? col : ""), error);
      free(col);
      n++;
    }
    if (n > 0) {
      CHECK_NA(INTERNAL, AppendStrOrNull(c->children[0], pk_name), error);
      CHECK_NA(INTERNAL, ArrowArrayAppendString(c->children[1], ArrowCharView("PRIMARY KEY")), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(names), error);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(usage), error);  // empty usage
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(c), error);
    }
    free(pk_name);
  }
  SQLFreeStmt(hstmt, SQL_CLOSE);

  // Foreign keys: rows grouped by FK_NAME (col 12), ordered by KEY_SEQ.
  ret = SQLForeignKeys(hstmt, NULL, 0, NULL, 0, NULL, 0, Pat(t->catalog), PatLen(t->catalog),
                       Pat(t->schema), PatLen(t->schema), Pat(t->table), SQL_NTS);
  if (SQL_SUCCEEDED(ret)) {
    char* cur_name = NULL;
    bool open = false;
    int64_t seq_prev = 0;
    while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
      char* fk_name = GetStrCol(hstmt, 12);
      int64_t seq = 0;
      GetIntCol(hstmt, 9, &seq);
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
      char* fkcol = GetStrCol(hstmt, 8);
      CHECK_NA(INTERNAL, AppendStrOrNull(names->children[0], fkcol ? fkcol : ""), error);
      free(fkcol);
      struct ArrowArray* u = usage->children[0];
      char* s;
      s = GetStrCol(hstmt, 1); CHECK_NA(INTERNAL, AppendStrOrNull(u->children[0], s), error); free(s);
      s = GetStrCol(hstmt, 2); CHECK_NA(INTERNAL, AppendStrOrNull(u->children[1], s), error); free(s);
      s = GetStrCol(hstmt, 3); CHECK_NA(INTERNAL, AppendStrOrNull(u->children[2], s ? s : ""), error); free(s);
      s = GetStrCol(hstmt, 4); CHECK_NA(INTERNAL, AppendStrOrNull(u->children[3], s ? s : ""), error); free(s);
      CHECK_NA(INTERNAL, ArrowArrayFinishElement(u), error);
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

static AdbcStatusCode CollectTables(struct OdbcConnection* conn, int depth, const char* catalog,
                                    const char* db_schema, const char* table_name,
                                    const char** table_type, struct TableRow** out_rows,
                                    size_t* out_n, struct AdbcError* error) {
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret;
  char* types = NULL;
  if (depth == ADBC_OBJECT_DEPTH_CATALOGS) {
    ret = SQLTables(hstmt, (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0,
                    (SQLCHAR*)"", 0);
  } else if (depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS) {
    ret = SQLTables(hstmt, (SQLCHAR*)"", 0, (SQLCHAR*)SQL_ALL_SCHEMAS, SQL_NTS, (SQLCHAR*)"", 0,
                    (SQLCHAR*)"", 0);
  } else {
    if (table_type && table_type[0]) {
      struct InternalAdbcStringBuilder sb;
      InternalAdbcStringBuilderInit(&sb, 64);
      for (int i = 0; table_type[i]; i++) {
        InternalAdbcStringBuilderAppend(&sb, i ? ",%s" : "%s", table_type[i]);
      }
      types = sb.buffer;  // take ownership
    }
    ret = SQLTables(hstmt, Pat(catalog), PatLen(catalog), Pat(db_schema), PatLen(db_schema),
                    Pat(table_name), PatLen(table_name), Pat(types), PatLen(types));
  }
  free(types);
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLTables", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return s;
  }
  struct TableRow* rows = NULL;
  size_t n = 0, cap = 0;
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      rows = realloc(rows, cap * sizeof(*rows));
    }
    rows[n].catalog = GetStrCol(hstmt, 1);
    rows[n].schema = GetStrCol(hstmt, 2);
    rows[n].table = GetStrCol(hstmt, 3);
    rows[n].type = GetStrCol(hstmt, 4);
    n++;
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  if (n > 1) qsort(rows, n, sizeof(*rows), TableRowCmp);
  *out_rows = rows;
  *out_n = n;
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

  // In catalog/schema-only listings the driver may filter nothing; apply patterns loosely
  // (exact-or-null match) so the result is still sensible.
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
