// SPDX-License-Identifier: Apache-2.0
// Raw-ODBC "floor" for the read benchmark.
//
// Binds every column of a result set the same way src/odbc_reader.c does
// (SQLBindCol + a block cursor of `batch_size` rows) and then drains the result
// set with SQLFetch, touching each value but converting nothing. Whatever this
// costs is time our driver cannot avoid: it is the unixODBC driver manager plus
// the underlying ODBC driver plus SQLite. The difference between this and
// fetch_arrow_table() is the cost of our Arrow conversion.
//
// Build:  cc -O2 -o odbc_floor odbc_floor.c -lodbc
// Run:    ./odbc_floor "Driver=...;Database=...;" "SELECT ..." 8192
// Prints one JSON object on stdout.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sql.h>
#include <sqlext.h>

#define MAX_COLS 64

static void die(const char* what, SQLSMALLINT type, SQLHANDLE h) {
  SQLCHAR state[6] = {0}, msg[1024] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  SQLGetDiagRec(type, h, 1, state, &native, msg, sizeof(msg), &len);
  fprintf(stderr, "%s failed: [%s] %s\n", what, (char*)state, (char*)msg);
  exit(1);
}

static double now_sec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <connstr> <query> <batch_size>\n", argv[0]);
    return 2;
  }
  const char* connstr = argv[1];
  const char* query = argv[2];
  SQLULEN batch = (SQLULEN)strtoul(argv[3], NULL, 10);
  if (batch < 1) batch = 1;

  SQLHENV env = NULL;
  SQLHDBC dbc = NULL;
  SQLHSTMT stmt = NULL;

  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)))
    die("SQLAllocHandle(ENV)", SQL_HANDLE_ENV, env);
  SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc)))
    die("SQLAllocHandle(DBC)", SQL_HANDLE_ENV, env);
  if (!SQL_SUCCEEDED(SQLDriverConnect(dbc, NULL, (SQLCHAR*)connstr, SQL_NTS, NULL, 0, NULL,
                                      SQL_DRIVER_NOPROMPT)))
    die("SQLDriverConnect", SQL_HANDLE_DBC, dbc);
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt)))
    die("SQLAllocHandle(STMT)", SQL_HANDLE_DBC, dbc);

  double t0 = now_sec();

  if (!SQL_SUCCEEDED(SQLExecDirect(stmt, (SQLCHAR*)query, SQL_NTS)))
    die("SQLExecDirect", SQL_HANDLE_STMT, stmt);

  double t_exec = now_sec() - t0;

  SQLSMALLINT ncols = 0;
  if (!SQL_SUCCEEDED(SQLNumResultCols(stmt, &ncols))) die("SQLNumResultCols", SQL_HANDLE_STMT, stmt);
  if (ncols > MAX_COLS) ncols = MAX_COLS;

  void* bufs[MAX_COLS] = {0};
  SQLLEN* inds[MAX_COLS] = {0};
  SQLLEN elem[MAX_COLS] = {0};

  SQLSetStmtAttr(stmt, SQL_ATTR_ROW_BIND_TYPE, (SQLPOINTER)SQL_BIND_BY_COLUMN, 0);
  if (!SQL_SUCCEEDED(SQLSetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)batch, 0))) {
    batch = 1;
    SQLSetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)1, 0);
  }
  SQLUSMALLINT* row_status = calloc(batch, sizeof(SQLUSMALLINT));
  SQLULEN fetched = 0;
  SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, row_status, 0);
  SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0);

  for (SQLSMALLINT i = 0; i < ncols; i++) {
    SQLCHAR name[256];
    SQLSMALLINT name_len = 0, sql_type = 0, digits = 0, nullable = 0;
    SQLULEN col_size = 0;
    if (!SQL_SUCCEEDED(SQLDescribeCol(stmt, (SQLUSMALLINT)(i + 1), name, sizeof(name), &name_len,
                                      &sql_type, &col_size, &digits, &nullable)))
      die("SQLDescribeCol", SQL_HANDLE_STMT, stmt);

    SQLSMALLINT c_type;
    switch (sql_type) {
      case SQL_TINYINT:  c_type = SQL_C_STINYINT; elem[i] = sizeof(SQLSCHAR); break;
      case SQL_SMALLINT: c_type = SQL_C_SSHORT;   elem[i] = sizeof(SQLSMALLINT); break;
      case SQL_INTEGER:  c_type = SQL_C_SLONG;    elem[i] = sizeof(SQLINTEGER); break;
      case SQL_BIGINT:   c_type = SQL_C_SBIGINT;  elem[i] = sizeof(SQLBIGINT); break;
      case SQL_REAL:     c_type = SQL_C_FLOAT;    elem[i] = sizeof(SQLREAL); break;
      case SQL_FLOAT:
      case SQL_DOUBLE:   c_type = SQL_C_DOUBLE;   elem[i] = sizeof(SQLDOUBLE); break;
      case SQL_TYPE_DATE:
      case SQL_DATE:     c_type = SQL_C_TYPE_DATE; elem[i] = sizeof(DATE_STRUCT); break;
      case SQL_TYPE_TIMESTAMP:
      case SQL_TIMESTAMP: c_type = SQL_C_TYPE_TIMESTAMP; elem[i] = sizeof(TIMESTAMP_STRUCT); break;
      default:
        // Same sizing rule as src/odbc_reader.c: chars * 4 + 1 for UTF-8.
        c_type = SQL_C_CHAR;
        elem[i] = (SQLLEN)col_size * 4 + 1;
        if (elem[i] < 2) elem[i] = 256;
        break;
    }
    bufs[i] = calloc(batch, (size_t)elem[i]);
    inds[i] = calloc(batch, sizeof(SQLLEN));
    if (!bufs[i] || !inds[i]) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    if (!SQL_SUCCEEDED(SQLBindCol(stmt, (SQLUSMALLINT)(i + 1), c_type, bufs[i], elem[i], inds[i])))
      die("SQLBindCol", SQL_HANDLE_STMT, stmt);
  }

  double t_fetch0 = now_sec();
  long long rows = 0;
  unsigned long long checksum = 0;  // keeps the reads from being optimized away
  long long fetch_calls = 0;
  for (;;) {
    fetched = 0;
    fetch_calls++;
    SQLRETURN ret = SQLFetch(stmt);
    if (ret == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(ret)) die("SQLFetch", SQL_HANDLE_STMT, stmt);
    for (SQLULEN r = 0; r < fetched; r++) {
      for (SQLSMALLINT i = 0; i < ncols; i++) {
        if (inds[i][r] == SQL_NULL_DATA) continue;
        const unsigned char* p = (const unsigned char*)bufs[i] + (size_t)r * (size_t)elem[i];
        checksum += p[0];
      }
      rows++;
    }
  }

  double t_end = now_sec();
  double seconds = t_end - t0;

  printf("{\"seconds\": %.6f, \"exec_seconds\": %.6f, \"fetch_seconds\": %.6f, "
         "\"rows\": %lld, \"cols\": %d, \"batch_size\": %lu, \"fetch_calls\": %lld, "
         "\"checksum\": %llu}\n",
         seconds, t_exec, t_end - t_fetch0, rows, (int)ncols, (unsigned long)batch, fetch_calls,
         checksum);

  for (SQLSMALLINT i = 0; i < ncols; i++) {
    free(bufs[i]);
    free(inds[i]);
  }
  free(row_status);
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  SQLDisconnect(dbc);
  SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);
  return 0;
}
