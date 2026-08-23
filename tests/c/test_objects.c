// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ConnectionGetObjects against a fake ODBC driver that enforces
// the SQLGetData contract of a driver *without* SQL_GD_ANY_ORDER: within a row,
// SQLGetData may only be called for strictly increasing column numbers.
// msodbcsql18 is such a driver, so any backwards jump here is a real bug --
// it silently produced garbage catalog metadata on SQL Server.
//
// The whole translation unit is included so the ODBC entry points it calls
// resolve to the fakes below (a definition in the executable wins over the one
// in libodbc), and so the internal helpers are visible.

#include "odbc_objects.c"

#include "test_common.h"

// ---------------------------------------------------------------------------
// Fake ODBC driver

#define FAKE_MAX_COLS 24

struct FakeCell {
  const char* s;  // NULL => SQL NULL, unless is_int
  long long i;
  bool is_int;
};

#define S(str) {(str), 0, false}
#define I(v) {NULL, (v), true}
#define NUL {NULL, 0, false}

struct FakeRow {
  struct FakeCell cells[FAKE_MAX_COLS + 1];  // 1-based, [0] unused
};

struct FakeStmt {
  const struct FakeRow* rows;
  int nrows;
  int row;       // 0 => before the first row
  int last_col;  // highest column already fetched from this row
};

static int g_order_violations = 0;

// TABLE_CAT, TABLE_SCHEM, TABLE_NAME, TABLE_TYPE
static const struct FakeRow kTableRows[] = {
    {{NUL, S("maindb"), S("dbo"), S("t1"), S("TABLE")}},
    {{NUL, S("maindb"), S("dbo"), S("t2"), S("VIEW")}},
};
// SQL_ALL_CATALOGS: TABLE_CAT only.
static const struct FakeRow kCatalogRows[] = {
    {{NUL, S("maindb"), NUL, NUL, NUL}},
    {{NUL, S("otherdb"), NUL, NUL, NUL}},
};
// SQLColumns: 4 name, 5 data type, 6 type name, 7 size, 9 digits, 10 radix,
// 11 nullable, 12 remarks, 13 default, 14 sql type, 15 datetime sub,
// 16 octet length, 17 ordinal, 18 is nullable.
static const struct FakeRow kColumnRows[] = {
    {{NUL, S("maindb"), S("dbo"), S("t1"), S("a"), I(4), S("int"), I(10), NUL, I(0), I(10), I(0),
      S("the key"), S("7"), I(4), NUL, I(4), I(1), S("NO")}},
    {{NUL, S("maindb"), S("dbo"), S("t1"), S("b"), I(12), S("varchar"), I(32), NUL, NUL, NUL, I(1),
      NUL, NUL, I(12), NUL, I(32), I(2), S("YES")}},
};
// SQLPrimaryKeys: 4 COLUMN_NAME, 5 KEY_SEQ, 6 PK_NAME.
static const struct FakeRow kPrimaryKeyRows[] = {
    {{NUL, S("maindb"), S("dbo"), S("t1"), S("a"), I(1), S("pk_t1")}},
};
// SQLForeignKeys: 1-4 PKTABLE_*, 8 FKCOLUMN_NAME, 9 KEY_SEQ, 12 FK_NAME.
static const struct FakeRow kForeignKeyRows[] = {
    {{NUL, S("maindb"), S("dbo"), S("pkt"), S("id"), S("maindb"), S("dbo"), S("t1"), S("b"), I(1),
      NUL, NUL, S("fk_t1_pkt")}},
};

static void FakeSetResult(SQLHSTMT hstmt, const struct FakeRow* rows, int nrows) {
  struct FakeStmt* st = (struct FakeStmt*)hstmt;
  st->rows = rows;
  st->nrows = nrows;
  st->row = 0;
  st->last_col = 0;
}

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* output) {
  (void)input;
  if (type != SQL_HANDLE_STMT) return SQL_ERROR;
  struct FakeStmt* st = (struct FakeStmt*)calloc(1, sizeof(*st));
  if (!st) return SQL_ERROR;
  *output = st;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT type, SQLHANDLE handle) {
  (void)type;
  free(handle);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT hstmt, SQLUSMALLINT option) {
  (void)option;
  FakeSetResult(hstmt, NULL, 0);
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFetch(SQLHSTMT hstmt) {
  struct FakeStmt* st = (struct FakeStmt*)hstmt;
  if (st->row >= st->nrows) return SQL_NO_DATA;
  st->row++;
  st->last_col = 0;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetData(SQLHSTMT hstmt, SQLUSMALLINT col, SQLSMALLINT target_type,
                             SQLPOINTER value, SQLLEN buflen, SQLLEN* ind) {
  struct FakeStmt* st = (struct FakeStmt*)hstmt;
  if (st->row < 1 || st->row > st->nrows) return SQL_ERROR;
  if (col > FAKE_MAX_COLS) return SQL_ERROR;
  if (col <= st->last_col) {
    // What a driver without SQL_GD_ANY_ORDER does: refuse and report HY109.
    fprintf(stderr, "FAIL out-of-order SQLGetData: column %d after column %d\n", (int)col,
            st->last_col);
    g_order_violations++;
    return SQL_ERROR;
  }
  st->last_col = col;
  const struct FakeCell* c = &st->rows[st->row - 1].cells[col];
  if (!c->is_int && !c->s) {
    if (ind) *ind = SQL_NULL_DATA;
    return SQL_SUCCESS;
  }
  char tmp[64];
  const char* text = c->s;
  if (c->is_int) {
    snprintf(tmp, sizeof(tmp), "%lld", c->i);
    text = tmp;
  }
  if (target_type == SQL_C_SBIGINT) {
    if (buflen < (SQLLEN)sizeof(SQLBIGINT)) return SQL_ERROR;
    *(SQLBIGINT*)value = c->is_int ? (SQLBIGINT)c->i : (SQLBIGINT)atoll(text);
    if (ind) *ind = (SQLLEN)sizeof(SQLBIGINT);
    return SQL_SUCCESS;
  }
  if (target_type != SQL_C_CHAR) return SQL_ERROR;
  size_t len = strlen(text);
  if (buflen <= (SQLLEN)len) return SQL_ERROR;  // the tests never truncate
  memcpy(value, text, len + 1);
  if (ind) *ind = (SQLLEN)len;
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLTables(SQLHSTMT hstmt, SQLCHAR* cat, SQLSMALLINT cat_len, SQLCHAR* sch,
                            SQLSMALLINT sch_len, SQLCHAR* tbl, SQLSMALLINT tbl_len, SQLCHAR* type,
                            SQLSMALLINT type_len) {
  (void)cat_len; (void)sch_len; (void)tbl; (void)tbl_len; (void)type; (void)type_len;
  bool all_catalogs = cat && strcmp((const char*)cat, SQL_ALL_CATALOGS) == 0 && sch && !*sch;
  if (all_catalogs) {
    FakeSetResult(hstmt, kCatalogRows, (int)(sizeof(kCatalogRows) / sizeof(kCatalogRows[0])));
  } else {
    FakeSetResult(hstmt, kTableRows, (int)(sizeof(kTableRows) / sizeof(kTableRows[0])));
  }
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT hstmt, SQLCHAR* cat, SQLSMALLINT cat_len, SQLCHAR* sch,
                             SQLSMALLINT sch_len, SQLCHAR* tbl, SQLSMALLINT tbl_len, SQLCHAR* col,
                             SQLSMALLINT col_len) {
  (void)cat; (void)cat_len; (void)sch; (void)sch_len; (void)tbl; (void)tbl_len; (void)col;
  (void)col_len;
  FakeSetResult(hstmt, kColumnRows, (int)(sizeof(kColumnRows) / sizeof(kColumnRows[0])));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT hstmt, SQLCHAR* cat, SQLSMALLINT cat_len, SQLCHAR* sch,
                                 SQLSMALLINT sch_len, SQLCHAR* tbl, SQLSMALLINT tbl_len) {
  (void)cat; (void)cat_len; (void)sch; (void)sch_len; (void)tbl; (void)tbl_len;
  FakeSetResult(hstmt, kPrimaryKeyRows,
                (int)(sizeof(kPrimaryKeyRows) / sizeof(kPrimaryKeyRows[0])));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLForeignKeys(SQLHSTMT hstmt, SQLCHAR* pkcat, SQLSMALLINT pkcat_len,
                                 SQLCHAR* pksch, SQLSMALLINT pksch_len, SQLCHAR* pktbl,
                                 SQLSMALLINT pktbl_len, SQLCHAR* fkcat, SQLSMALLINT fkcat_len,
                                 SQLCHAR* fksch, SQLSMALLINT fksch_len, SQLCHAR* fktbl,
                                 SQLSMALLINT fktbl_len) {
  (void)pkcat; (void)pkcat_len; (void)pksch; (void)pksch_len; (void)pktbl; (void)pktbl_len;
  (void)fkcat; (void)fkcat_len; (void)fksch; (void)fksch_len; (void)fktbl; (void)fktbl_len;
  FakeSetResult(hstmt, kForeignKeyRows,
                (int)(sizeof(kForeignKeyRows) / sizeof(kForeignKeyRows[0])));
  return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC hdbc, SQLINTEGER attr, SQLPOINTER value,
                                    SQLINTEGER buflen, SQLINTEGER* out_len) {
  (void)hdbc;
  if (attr != SQL_ATTR_CURRENT_CATALOG || buflen < 7) return SQL_ERROR;
  memcpy(value, "maindb", 7);
  if (out_len) *out_len = 6;
  return SQL_SUCCESS;
}

// odbc_reader.c is not linked here; the fakes never fail in a way that reaches
// the diagnostics path.
AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error) {
  (void)handle_type;
  (void)handle;
  InternalAdbcSetError(error, "[ODBC] %s failed", context ? context : "call");
  return ADBC_STATUS_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Tests

static AdbcStatusCode GetObjects(int depth, const char* catalog, const char* db_schema,
                                 struct ArrowArrayStream* out) {
  struct OdbcConnection conn;
  memset(&conn, 0, sizeof(conn));
  conn.connected = true;
  conn.hdbc = (SQLHDBC)(void*)&conn;
  struct AdbcConnection adbc_conn;
  memset(&adbc_conn, 0, sizeof(adbc_conn));
  adbc_conn.private_data = &conn;
  struct AdbcError error = ADBC_ERROR_INIT;
  AdbcStatusCode status =
      OdbcConnectionGetObjects(&adbc_conn, depth, catalog, db_schema, NULL, NULL, NULL, out, &error);
  if (status != ADBC_STATUS_OK) {
    fprintf(stderr, "GetObjects failed: %s\n", error.message ? error.message : "(no message)");
    if (error.release) error.release(&error);
  }
  return status;
}

// Reads the single batch and hands back schema + array; caller releases both.
static bool ReadOne(struct ArrowArrayStream* stream, struct ArrowSchema* schema,
                    struct ArrowArray* array) {
  if (stream->get_schema(stream, schema) != 0) return false;
  if (stream->get_next(stream, array) != 0) return false;
  return array->release != NULL;
}

static const char* StrAt(struct ArrowArrayView* v, int64_t i) {
  if (ArrowArrayViewIsNull(v, i)) return NULL;
  struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(v, i);
  static char buf[256];
  size_t n = (size_t)s.size_bytes < sizeof(buf) - 1 ? (size_t)s.size_bytes : sizeof(buf) - 1;
  memcpy(buf, s.data, n);
  buf[n] = '\0';
  return buf;
}

static void TestTablesDepth(void) {
  struct ArrowArrayStream stream;
  if (GetObjects(ADBC_OBJECT_DEPTH_TABLES, NULL, NULL, &stream) != ADBC_STATUS_OK) {
    adbc_test_failures++;
    return;
  }
  struct ArrowSchema schema;
  struct ArrowArray array;
  if (!ReadOne(&stream, &schema, &array)) {
    adbc_test_failures++;
    stream.release(&stream);
    return;
  }
  struct ArrowArrayView view;
  struct ArrowError na_err;
  CHECK_I64(ArrowArrayViewInitFromSchema(&view, &schema, &na_err), NANOARROW_OK);
  CHECK_I64(ArrowArrayViewSetArray(&view, &array, &na_err), NANOARROW_OK);

  // catalog_name, catalog_db_schemas[db_schema_name, db_schema_tables[...]]
  struct ArrowArrayView* cat = view.children[0];
  struct ArrowArrayView* schemas = view.children[1];
  struct ArrowArrayView* sch = schemas->children[0];
  struct ArrowArrayView* tables = sch->children[1];
  struct ArrowArrayView* tbl = tables->children[0];

  CHECK_I64(view.length, 1);
  CHECK_TRUE(StrAt(cat, 0) && strcmp(StrAt(cat, 0), "maindb") == 0);
  CHECK_I64(sch->length, 1);
  CHECK_TRUE(StrAt(sch->children[0], 0) && strcmp(StrAt(sch->children[0], 0), "dbo") == 0);
  CHECK_I64(tbl->length, 2);
  CHECK_TRUE(StrAt(tbl->children[0], 0) && strcmp(StrAt(tbl->children[0], 0), "t1") == 0);
  CHECK_TRUE(StrAt(tbl->children[1], 0) && strcmp(StrAt(tbl->children[1], 0), "TABLE") == 0);
  CHECK_TRUE(StrAt(tbl->children[0], 1) && strcmp(StrAt(tbl->children[0], 1), "t2") == 0);
  CHECK_TRUE(StrAt(tbl->children[1], 1) && strcmp(StrAt(tbl->children[1], 1), "VIEW") == 0);

  ArrowArrayViewReset(&view);
  ArrowArrayRelease(&array);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
}

static void TestAllDepth(void) {
  struct ArrowArrayStream stream;
  if (GetObjects(ADBC_OBJECT_DEPTH_ALL, NULL, NULL, &stream) != ADBC_STATUS_OK) {
    adbc_test_failures++;
    return;
  }
  struct ArrowSchema schema;
  struct ArrowArray array;
  if (!ReadOne(&stream, &schema, &array)) {
    adbc_test_failures++;
    stream.release(&stream);
    return;
  }
  struct ArrowArrayView view;
  struct ArrowError na_err;
  CHECK_I64(ArrowArrayViewInitFromSchema(&view, &schema, &na_err), NANOARROW_OK);
  CHECK_I64(ArrowArrayViewSetArray(&view, &array, &na_err), NANOARROW_OK);

  struct ArrowArrayView* tbl =
      view.children[1]->children[0]->children[1]->children[0];
  struct ArrowArrayView* cols = tbl->children[2]->children[0];
  struct ArrowArrayView* cons = tbl->children[3]->children[0];

  // Columns land in the right fields even though SQLColumns is read in
  // ascending column order rather than in schema order.
  CHECK_I64(cols->length, 4);  // two columns for each of the two tables
  CHECK_TRUE(StrAt(cols->children[0], 0) && strcmp(StrAt(cols->children[0], 0), "a") == 0);
  CHECK_I64(ArrowArrayViewGetIntUnsafe(cols->children[1], 0), 1);   // ordinal_position
  CHECK_TRUE(StrAt(cols->children[2], 0) && strcmp(StrAt(cols->children[2], 0), "the key") == 0);
  CHECK_I64(ArrowArrayViewGetIntUnsafe(cols->children[3], 0), 4);   // xdbc_data_type
  CHECK_TRUE(StrAt(cols->children[4], 0) && strcmp(StrAt(cols->children[4], 0), "int") == 0);
  CHECK_I64(ArrowArrayViewGetIntUnsafe(cols->children[5], 0), 10);  // xdbc_column_size
  CHECK_TRUE(StrAt(cols->children[9], 0) && strcmp(StrAt(cols->children[9], 0), "7") == 0);
  CHECK_I64(ArrowArrayViewGetIntUnsafe(cols->children[12], 0), 4);  // char_octet_length
  CHECK_TRUE(StrAt(cols->children[13], 0) && strcmp(StrAt(cols->children[13], 0), "NO") == 0);
  CHECK_TRUE(StrAt(cols->children[0], 1) && strcmp(StrAt(cols->children[0], 1), "b") == 0);
  CHECK_I64(ArrowArrayViewGetIntUnsafe(cols->children[1], 1), 2);
  CHECK_TRUE(ArrowArrayViewIsNull(cols->children[2], 1));  // no remarks

  // One PRIMARY KEY (NULL usage) and one FOREIGN KEY per table.
  CHECK_I64(cons->length, 4);
  CHECK_TRUE(StrAt(cons->children[0], 0) && strcmp(StrAt(cons->children[0], 0), "pk_t1") == 0);
  CHECK_TRUE(StrAt(cons->children[1], 0) && strcmp(StrAt(cons->children[1], 0), "PRIMARY KEY") == 0);
  CHECK_TRUE(ArrowArrayViewIsNull(cons->children[3], 0));
  CHECK_TRUE(StrAt(cons->children[0], 1) && strcmp(StrAt(cons->children[0], 1), "fk_t1_pkt") == 0);
  CHECK_TRUE(StrAt(cons->children[1], 1) && strcmp(StrAt(cons->children[1], 1), "FOREIGN KEY") == 0);
  struct ArrowArrayView* usage = cons->children[3]->children[0];
  CHECK_TRUE(StrAt(usage->children[0], 0) && strcmp(StrAt(usage->children[0], 0), "maindb") == 0);
  CHECK_TRUE(StrAt(usage->children[1], 0) && strcmp(StrAt(usage->children[1], 0), "dbo") == 0);
  CHECK_TRUE(StrAt(usage->children[2], 0) && strcmp(StrAt(usage->children[2], 0), "pkt") == 0);
  CHECK_TRUE(StrAt(usage->children[3], 0) && strcmp(StrAt(usage->children[3], 0), "id") == 0);
  // The constrained column, not the referenced one.
  CHECK_TRUE(StrAt(cons->children[2]->children[0], 1) &&
             strcmp(StrAt(cons->children[2]->children[0], 1), "b") == 0);

  ArrowArrayViewReset(&view);
  ArrowArrayRelease(&array);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
}

static void TestCatalogsDepth(void) {
  struct ArrowArrayStream stream;
  if (GetObjects(ADBC_OBJECT_DEPTH_CATALOGS, NULL, NULL, &stream) != ADBC_STATUS_OK) {
    adbc_test_failures++;
    return;
  }
  struct ArrowSchema schema;
  struct ArrowArray array;
  if (!ReadOne(&stream, &schema, &array)) {
    adbc_test_failures++;
    stream.release(&stream);
    return;
  }
  struct ArrowArrayView view;
  struct ArrowError na_err;
  CHECK_I64(ArrowArrayViewInitFromSchema(&view, &schema, &na_err), NANOARROW_OK);
  CHECK_I64(ArrowArrayViewSetArray(&view, &array, &na_err), NANOARROW_OK);
  // SQL_ALL_CATALOGS reported two, and the current catalog is one of them.
  CHECK_I64(view.length, 2);
  CHECK_TRUE(StrAt(view.children[0], 0) && strcmp(StrAt(view.children[0], 0), "maindb") == 0);
  CHECK_TRUE(StrAt(view.children[0], 1) && strcmp(StrAt(view.children[0], 1), "otherdb") == 0);
  ArrowArrayViewReset(&view);
  ArrowArrayRelease(&array);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
}

int main(void) {
  TestTablesDepth();
  TestAllDepth();
  TestCatalogsDepth();
  // The point of the fake: not one SQLGetData went backwards.
  CHECK_I64(g_order_violations, 0);
  return TEST_MAIN_RESULT();
}
