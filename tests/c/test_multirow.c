// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the multi-row INSERT text the bulk-ingest batching builds
// (MultiRowSql in odbc_bind.c) -- the standard `VALUES (...),(...)` form, Oracle's
// `INSERT ALL ... SELECT 1 FROM dual`, and Firebird's `SELECT CAST(? AS t), ... FROM
// RDB$DATABASE UNION ALL ...`.  The statement text is what every backend is judged on,
// and it is not otherwise observable from a test.
//
// The binding translation unit is included so its internal helpers are visible; the
// handful of symbols it takes from the other translation units are stubbed below.

#include "odbc_bind.c"

#include "test_common.h"

// --- stubs for the symbols odbc_bind.c takes from the rest of the driver ----
AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error) {
  InternalAdbcSetError(error, "%s failed", context);
  return ADBC_STATUS_IO;
}
SQLLEN OdbcRowCount(SQLHSTMT hstmt, bool sqllen_32bit) { return -1; }
struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt) { return NULL; }
void OdbcHandleRefRelease(struct OdbcHandleRef* ref) {}
void OdbcQuoteChar(SQLHDBC hdbc, char* out) { out[0] = '"'; out[1] = '\0'; }
AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error) {
  return ADBC_STATUS_INVALID_STATE;
}
AdbcStatusCode OdbcOpenHdbc(struct OdbcDatabase* db, SQLHDBC* out, struct AdbcError* error) {
  *out = NULL;
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error) {
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
AdbcStatusCode OdbcDescribeResultSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                        struct ArrowSchema* out, struct AdbcError* error) {
  return ADBC_STATUS_NOT_IMPLEMENTED;
}
AdbcStatusCode OdbcDescribeParameterSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                           struct ArrowSchema* out, struct AdbcError* error) {
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static char* const kCastTypes[] = {(char*)"BIGINT",  (char*)"BLOB SUB_TYPE TEXT",
                                   (char*)"INTEGER", (char*)"DATE",
                                   (char*)"TIMESTAMP"};

static void CheckSqlForm(const char* into, int64_t ncols, int64_t rows, int form,
                         const char* expected) {
  char* sql = MultiRowSql(into, ncols, rows, form, kCastTypes, "RDB$DATABASE");
  CHECK_TRUE(sql != NULL);
  if (sql) {
    CHECK_STR(sql, strlen(sql), expected);
    free(sql);
  }
}

static void CheckSql(const char* into, int64_t ncols, int64_t rows, bool insert_all,
                     const char* expected) {
  CheckSqlForm(into, ncols, rows, insert_all ? ODBC_MULTIROW_INSERT_ALL : ODBC_MULTIROW_VALUES,
               expected);
}

static void TestStandardForm(void) {
  // One row-group is what the single-row INSERT already is; the ingest path never asks
  // for it, but the text must still be the ordinary statement.
  CheckSql("\"t\" (\"a\", \"b\")", 2, 1, false,
           "INSERT INTO \"t\" (\"a\", \"b\") VALUES (?, ?)");
  CheckSql("\"t\" (\"a\", \"b\")", 2, 3, false,
           "INSERT INTO \"t\" (\"a\", \"b\") VALUES (?, ?), (?, ?), (?, ?)");
  // A single column, and a qualified name with a schema.
  CheckSql("\"s\".\"t\" (\"a\")", 1, 2, false, "INSERT INTO \"s\".\"t\" (\"a\") VALUES (?), (?)");
}

static void TestInsertAllForm(void) {
  // Oracle: no multi-row VALUES, so each row-group is its own INTO and the statement is
  // closed by a one-row subquery.
  CheckSql("\"t\" (\"a\", \"b\")", 2, 2, true,
           "INSERT ALL INTO \"t\" (\"a\", \"b\") VALUES (?, ?)"
           " INTO \"t\" (\"a\", \"b\") VALUES (?, ?) SELECT 1 FROM dual");
  CheckSql("\"t\" (\"a\")", 1, 1, true,
           "INSERT ALL INTO \"t\" (\"a\") VALUES (?) SELECT 1 FROM dual");
}

static void TestUnionForm(void) {
  // Firebird: no multi-row VALUES and no INSERT ALL, and a bare `?` in a select list has
  // no type the server can infer -- so every parameter is CAST to the type ingest would
  // have given the column, and the row-groups are UNION ALL branches over a one-row table.
  CheckSqlForm("\"t\" (\"a\", \"b\")", 2, 2, ODBC_MULTIROW_UNION,
               "INSERT INTO \"t\" (\"a\", \"b\") "
               "SELECT CAST(? AS BIGINT), CAST(? AS BLOB SUB_TYPE TEXT) FROM RDB$DATABASE"
               " UNION ALL SELECT CAST(? AS BIGINT), CAST(? AS BLOB SUB_TYPE TEXT)"
               " FROM RDB$DATABASE");
  CheckSqlForm("\"t\" (\"a\")", 1, 1, ODBC_MULTIROW_UNION,
               "INSERT INTO \"t\" (\"a\") SELECT CAST(? AS BIGINT) FROM RDB$DATABASE");
}

// The parameter placeholders must line up with what MultiRowExecGroup binds:
// parameter (r * ncols + c + 1) is row r, column c.  Counting them is the cheapest
// check that the text and the binder agree on the layout.
static void TestParameterCount(void) {
  for (int64_t ncols = 1; ncols <= 5; ncols++) {
    for (int64_t rows = 1; rows <= 7; rows++) {
      for (int pass = 0; pass < 3; pass++) {
        char* sql = MultiRowSql("\"t\" (\"a\")", ncols, rows, pass, kCastTypes, "RDB$DATABASE");
        CHECK_TRUE(sql != NULL);
        if (!sql) continue;
        int64_t n = 0;
        for (const char* p = sql; *p; p++) {
          if (*p == '?') n++;
        }
        CHECK_I64(n, ncols * rows);
        free(sql);
      }
    }
  }
}

// MultiRowInit only arms the rewrite for a statement bulk ingest set up, and only when
// the option has not turned it off.  Nothing else may ever be rewritten.
static void TestEnabledOnlyForIngest(void) {
  struct OdbcConnection conn;
  struct OdbcStatement stmt;
  struct MultiRowInsert mr;
  memset(&conn, 0, sizeof(conn));
  memset(&stmt, 0, sizeof(stmt));
  conn.connected = true;
  stmt.conn = &conn;

  // A caller's own query: no ingest_into, so no rewrite.
  stmt.query = (char*)"INSERT INTO t VALUES (?, ?)";
  MultiRowInit(&mr, &stmt, NULL, 2);
  CHECK_TRUE(!mr.enabled);

  // Bulk ingest.
  stmt.ingest_into = (char*)"\"t\" (\"a\", \"b\")";
  MultiRowInit(&mr, &stmt, NULL, 2);
  CHECK_TRUE(mr.enabled);

  // adbc.odbc.rows_per_insert = 1 disables it.
  stmt.rows_per_insert = 1;
  MultiRowInit(&mr, &stmt, NULL, 2);
  CHECK_TRUE(!mr.enabled);
  stmt.rows_per_insert = 0;

  // Nothing to group.
  MultiRowInit(&mr, &stmt, NULL, 0);
  CHECK_TRUE(!mr.enabled);

  // A connection that is not up.
  conn.connected = false;
  MultiRowInit(&mr, &stmt, NULL, 2);
  CHECK_TRUE(!mr.enabled);
}

// The UTF-8 -> SQLWCHAR encoder, in the unit width this test was built with: two-byte
// units carry UTF-16 (a non-BMP character is a surrogate pair), four-byte units
// (unixODBC's SQL_WCHART_CONVERT, which is iODBC's width) carry one code point each.
static void TestWideEncoding(void) {
  const char* s = "h\xc3\xa9llo \xf0\x9f\x9a\x80";  // "héllo 🚀": 7 code points
  int64_t n = (int64_t)strlen(s);
  SQLWCHAR w[16];
  int64_t units = OdbcUtf8ToUtf16Into(w, s, n, false);
  CHECK_I64(Utf16Units(s, n, false), units);
  if (sizeof(SQLWCHAR) < 4) {
    CHECK_I64(units, 8);
    CHECK_I64((int64_t)w[1], 0xE9);
    CHECK_I64((int64_t)w[6], 0xD83D);
    CHECK_I64((int64_t)w[7], 0xDE80);
  } else {
    CHECK_I64(units, 7);
    CHECK_I64((int64_t)w[1], 0xE9);
    CHECK_I64((int64_t)w[6], 0x1F680);
    // The wide_utf16_pairs quirk: surrogate pairs in four-byte units.
    units = OdbcUtf8ToUtf16Into(w, s, n, true);
    CHECK_I64(Utf16Units(s, n, true), units);
    CHECK_I64(units, 8);
    CHECK_I64((int64_t)w[6], 0xD83D);
    CHECK_I64((int64_t)w[7], 0xDE80);
  }
  CHECK_I64((int64_t)w[units], 0);
}

int main(void) {
  TestWideEncoding();
  TestStandardForm();
  TestInsertAllForm();
  TestUnionForm();
  TestParameterCount();
  TestEnabledOnlyForIngest();
  if (adbc_test_failures) {
    fprintf(stderr, "%d failure(s)\n", adbc_test_failures);
    return 1;
  }
  printf("test_multirow: OK\n");
  return 0;
}
