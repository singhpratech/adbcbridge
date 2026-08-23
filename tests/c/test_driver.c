// SPDX-License-Identifier: Apache-2.0
//
// Dependency-free C smoke test for the ADBC ODBC driver.
//
// The test dlopen()s the built driver, resolves AdbcDriverInit, fills an
// ADBC 1.1.0 AdbcDriver vtable and drives it directly (no driver manager):
//
//   * database / connection / statement lifecycle
//   * SELECT with ints, strings and NULLs verified through ArrowArrayView
//   * long values, which take the chunked SQLGetData path in the reader
//   * a 3000 row query, to check batching and the batch_size option
//   * StatementExecuteSchema
//   * error propagation: bad SQL -> non-OK status, message and SQLSTATE
//   * handle release ordering: release the statement (or execute a second
//     query on it) while a result stream is still open, then read the stream
//
// Usage: test_driver [path/to/libadbc_driver_odbc.so]
// Environment:
//   ADBC_ODBC_DRIVER    path to the driver library (if argv[1] is absent)
//   SQLITE_ODBC_DRIVER  path/name of the SQLite ODBC driver (default SQLite3)
//
// This file is POSIX-only (dlopen/dlsym, mkdtemp). CMake builds it only when
// ADBC_ODBC_BUILD_TESTS is ON and the target is not Windows; the driver
// library itself has no dependency on this test.

#if defined(_WIN32)
#error "tests/c/test_driver.c is POSIX-only; it is not built on Windows"
#endif

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arrow-adbc/adbc.h>

#include "nanoarrow/nanoarrow.h"

// ---------------------------------------------------------------------------
// Tiny assertion helpers

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    g_checks++;                                                       \
    if (!(cond)) {                                                    \
      g_failures++;                                                   \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                 \
  } while (0)

#define CHECK_EQ_INT(actual, expected)                                   \
  do {                                                                   \
    g_checks++;                                                          \
    long long a_ = (long long)(actual);                                  \
    long long e_ = (long long)(expected);                                \
    if (a_ != e_) {                                                      \
      g_failures++;                                                      \
      fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, \
              __LINE__, #actual, #expected, a_, e_);                     \
    }                                                                    \
  } while (0)

#define CHECK_EQ_STR(actual, expected)                                     \
  do {                                                                     \
    g_checks++;                                                            \
    const char* a_ = (actual);                                             \
    const char* e_ = (expected);                                           \
    if (!a_ || !e_ || strcmp(a_, e_) != 0) {                               \
      g_failures++;                                                        \
      fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__, \
              __LINE__, #actual, e_ ? e_ : "(null)", a_ ? a_ : "(null)");  \
    }                                                                      \
  } while (0)

static void Fatal(const char* what) {
  fprintf(stderr, "FATAL: %s\n", what);
  exit(1);
}

static void ReleaseError(struct AdbcError* error) {
  if (error->release) error->release(error);
  memset(error, 0, sizeof(*error));
  error->vendor_code = ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA;
}

static bool AdbcOkImpl(AdbcStatusCode status, const char* expr, const char* file, int line,
                       struct AdbcError* error) {
  g_checks++;
  if (status != ADBC_STATUS_OK) {
    g_failures++;
    fprintf(stderr, "FAIL %s:%d: %s -> status %d: %s\n", file, line, expr, (int)status,
            error->message ? error->message : "(no message)");
    ReleaseError(error);
    return false;
  }
  return true;
}

// Check that a driver call succeeded; keep going on failure.
#define ADBC_OK(expr, error) AdbcOkImpl((expr), #expr, __FILE__, __LINE__, (error))
// Check that a driver call succeeded; abort the test run on failure.
#define ADBC_MUST(expr, error)                                     \
  do {                                                             \
    if (!ADBC_OK((expr), (error))) Fatal("unrecoverable: " #expr); \
  } while (0)

#define NA_MUST(expr)                                                     \
  do {                                                                    \
    int na_code_ = (expr);                                                \
    if (na_code_ != NANOARROW_OK) Fatal("nanoarrow call failed: " #expr); \
  } while (0)

static void Section(const char* name) { printf("-- %s\n", name); }

// ---------------------------------------------------------------------------
// Test fixture

struct Fixture {
  struct AdbcDriver driver;
  struct AdbcDatabase database;
  struct AdbcConnection connection;
  char db_dir[512];
  char db_path[600];
  char uri[1400];
  void* handle;
};

typedef AdbcStatusCode (*AdbcDriverInitFunc)(int, void*, struct AdbcError*);

// Run a statement that produces no result set (DDL/DML).
static int64_t RunUpdate(struct Fixture* fx, const char* sql) {
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  int64_t rows = -1;
  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
  ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, sql, &error), &error);
  if (!AdbcOkImpl(fx->driver.StatementExecuteQuery(&statement, NULL, &rows, &error), sql,
                  __FILE__, __LINE__, &error)) {
    fx->driver.StatementRelease(&statement, &error);
    ReleaseError(&error);
    Fatal("setup statement failed");
  }
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  return rows;
}

// ---------------------------------------------------------------------------
// Stream helpers

struct BatchStats {
  int64_t rows;
  int batches;
};

// Drain a stream, counting rows and batches. Returns false on stream error.
static bool DrainStream(struct ArrowArrayStream* stream, struct BatchStats* stats) {
  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  stats->rows = 0;
  stats->batches = 0;
  int rc = stream->get_schema(stream, &schema);
  if (rc != 0) {
    const char* msg = stream->get_last_error ? stream->get_last_error(stream) : NULL;
    fprintf(stderr, "get_schema failed: %s\n", msg ? msg : "(no message)");
    return false;
  }
  while (1) {
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    rc = stream->get_next(stream, &array);
    if (rc != 0) {
      const char* msg = stream->get_last_error ? stream->get_last_error(stream) : NULL;
      fprintf(stderr, "get_next failed: %s\n", msg ? msg : "(no message)");
      ArrowSchemaRelease(&schema);
      return false;
    }
    if (array.release == NULL) break;
    stats->rows += array.length;
    stats->batches++;
    ArrowArrayRelease(&array);
  }
  // Stream contract: an exhausted stream keeps reporting end-of-stream, and
  // get_schema still hands out an (independently owned) schema copy.
  {
    struct ArrowArray extra;
    memset(&extra, 0, sizeof(extra));
    CHECK_EQ_INT(stream->get_next(stream, &extra), 0);
    CHECK(extra.release == NULL);
    if (extra.release != NULL) ArrowArrayRelease(&extra);

    struct ArrowSchema again;
    memset(&again, 0, sizeof(again));
    CHECK_EQ_INT(stream->get_schema(stream, &again), 0);
    if (again.release != NULL) ArrowSchemaRelease(&again);
  }
  ArrowSchemaRelease(&schema);
  return true;
}

// ---------------------------------------------------------------------------
// Tests

// Statement lifecycle corner cases.
static void TestStatementLifecycle(struct Fixture* fx) {
  Section("statement lifecycle");
  struct AdbcError error = ADBC_ERROR_INIT;

  // New + Release without ever setting a query.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    AdbcStatusCode status = fx->driver.StatementExecuteQuery(&statement, NULL, NULL, &error);
    CHECK(status == ADBC_STATUS_INVALID_STATE);
    CHECK(error.message != NULL);
    ReleaseError(&error);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  }

  // Prepared statement executed twice.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(
        fx->driver.StatementSetSqlQuery(&statement, "SELECT i FROM t ORDER BY rowid", &error),
        &error);
    ADBC_MUST(fx->driver.StatementPrepare(&statement, &error), &error);
    for (int i = 0; i < 2; i++) {
      struct ArrowArrayStream stream;
      memset(&stream, 0, sizeof(stream));
      ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);
      struct BatchStats stats;
      CHECK(DrainStream(&stream, &stats));
      CHECK_EQ_INT(stats.rows, 4);
      stream.release(&stream);
    }
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  }

  // Prepared but never executed.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELECT i FROM t", &error), &error);
    ADBC_MUST(fx->driver.StatementPrepare(&statement, &error), &error);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  }

  // A result-producing query with out == NULL discards the result set.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELECT x FROM big", &error), &error);
    int64_t rows = 0;
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, NULL, &rows, &error), &error);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  }

  // A non-result-producing statement with out != NULL yields an empty stream.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(fx->driver.StatementSetSqlQuery(
                  &statement, "CREATE TABLE IF NOT EXISTS scratch (a INTEGER)", &error),
              &error);
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);
    struct BatchStats stats;
    CHECK(DrainStream(&stream, &stats));
    CHECK_EQ_INT(stats.rows, 0);
    stream.release(&stream);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  }
  ReleaseError(&error);
}

// SELECT with ints, strings and NULLs, verified via ArrowArrayView.
static void TestSelectValues(struct Fixture* fx) {
  Section("SELECT values (int / string / NULL)");
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));

  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
  ADBC_MUST(
      fx->driver.StatementSetSqlQuery(&statement, "SELECT i, s FROM t ORDER BY rowid", &error),
      &error);
  int64_t rows_affected = 0;
  ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, &rows_affected, &error),
            &error);

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  CHECK_EQ_INT(stream.get_schema(&stream, &schema), 0);
  CHECK_EQ_STR(schema.format, "+s");
  CHECK_EQ_INT(schema.n_children, 2);
  if (schema.n_children != 2) Fatal("unexpected result schema");
  CHECK_EQ_STR(schema.children[0]->name, "i");
  CHECK_EQ_STR(schema.children[1]->name, "s");
  // SQLite reports INTEGER as a 32-bit int; accept any signed integer width.
  CHECK(strcmp(schema.children[0]->format, "i") == 0 ||
        strcmp(schema.children[0]->format, "l") == 0);
  CHECK_EQ_STR(schema.children[1]->format, "u");

  struct ArrowArrayView view;
  struct ArrowError na_error;
  NA_MUST(ArrowArrayViewInitFromSchema(&view, &schema, &na_error));

  static const int64_t kExpectInts[] = {1, 0, -7, 2147483647};
  static const char* const kExpectStrs[] = {"one", "", "", "h\xc3\xa9llo"};
  static const bool kExpectNull[] = {false, true, false, false};

  int64_t row = 0;
  while (1) {
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    CHECK_EQ_INT(stream.get_next(&stream, &array), 0);
    if (array.release == NULL) break;
    NA_MUST(ArrowArrayViewSetArray(&view, &array, &na_error));
    for (int64_t i = 0; i < view.length; i++, row++) {
      if (row >= 4) {
        CHECK(row < 4);  // more rows than expected
        break;
      }
      if (kExpectNull[row]) {
        CHECK(ArrowArrayViewIsNull(view.children[0], i));
        CHECK(ArrowArrayViewIsNull(view.children[1], i));
        continue;
      }
      CHECK(!ArrowArrayViewIsNull(view.children[0], i));
      CHECK(!ArrowArrayViewIsNull(view.children[1], i));
      CHECK_EQ_INT(ArrowArrayViewGetIntUnsafe(view.children[0], i), kExpectInts[row]);
      struct ArrowStringView sv = ArrowArrayViewGetStringUnsafe(view.children[1], i);
      size_t expect_len = strlen(kExpectStrs[row]);
      CHECK_EQ_INT(sv.size_bytes, (int64_t)expect_len);
      if (sv.size_bytes == (int64_t)expect_len && expect_len > 0) {
        CHECK(memcmp(sv.data, kExpectStrs[row], expect_len) == 0);
      }
    }
    ArrowArrayRelease(&array);
  }
  CHECK_EQ_INT(row, 4);

  ArrowArrayViewReset(&view);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
  CHECK(stream.release == NULL);
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  CHECK(statement.private_data == NULL);
  ReleaseError(&error);
}

// Long values force the chunked SQLGetData path in the reader.
static void TestLargeStrings(struct Fixture* fx) {
  Section("large strings (chunked SQLGetData)");
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));

  // Open a second database/connection with a tiny bind budget so that every
  // variable-length column is read with the chunked SQLGetData path instead of
  // a bound buffer. (On the default connection this driver clamps values that
  // are longer than the bound buffer - see AppendValue() in odbc_reader.c -
  // so long values must go through SQLGetData to come back intact.)
  struct AdbcDatabase database;
  struct AdbcConnection connection;
  memset(&database, 0, sizeof(database));
  memset(&connection, 0, sizeof(connection));
  ADBC_MUST(fx->driver.DatabaseNew(&database, &error), &error);
  ADBC_MUST(fx->driver.DatabaseSetOption(&database, "uri", fx->uri, &error), &error);
  ADBC_MUST(fx->driver.DatabaseSetOption(&database, "adbc.odbc.max_bind_bytes", "8", &error),
            &error);
  ADBC_MUST(fx->driver.DatabaseInit(&database, &error), &error);
  ADBC_MUST(fx->driver.ConnectionNew(&connection, &error), &error);
  ADBC_MUST(fx->driver.ConnectionInit(&connection, &database, &error), &error);

  ADBC_MUST(fx->driver.StatementNew(&connection, &statement, &error), &error);
  ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELECT h, n FROM bigtext ORDER BY n",
                                            &error),
            &error);
  ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  CHECK_EQ_INT(stream.get_schema(&stream, &schema), 0);
  CHECK_EQ_INT(schema.n_children, 2);
  if (schema.n_children != 2) Fatal("unexpected result schema");
  struct ArrowArrayView view;
  struct ArrowError na_error;
  NA_MUST(ArrowArrayViewInitFromSchema(&view, &schema, &na_error));

  int64_t rows = 0;
  while (1) {
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    CHECK_EQ_INT(stream.get_next(&stream, &array), 0);
    if (array.release == NULL) break;
    NA_MUST(ArrowArrayViewSetArray(&view, &array, &na_error));
    for (int64_t i = 0; i < view.length; i++, rows++) {
      struct ArrowStringView sv = ArrowArrayViewGetStringUnsafe(view.children[0], i);
      int64_t expected = ArrowArrayViewGetIntUnsafe(view.children[1], i);
      CHECK_EQ_INT(sv.size_bytes, expected);
      // Touch every byte so ASan sees any out-of-bounds buffer.
      int64_t hex_digits = 0;
      for (int64_t b = 0; b < sv.size_bytes; b++) {
        char c = sv.data[b];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) hex_digits++;
      }
      CHECK_EQ_INT(hex_digits, sv.size_bytes);
    }
    ArrowArrayRelease(&array);
  }
  CHECK_EQ_INT(rows, 3);

  ArrowArrayViewReset(&view);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  ADBC_MUST(fx->driver.ConnectionRelease(&connection, &error), &error);
  ADBC_MUST(fx->driver.DatabaseRelease(&database, &error), &error);
  ReleaseError(&error);
}

// A 3000-row query with an explicit batch size, to check batching.
static void TestBatching(struct Fixture* fx) {
  Section("batching over 3000 rows");
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));

  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
  ADBC_OK(fx->driver.StatementSetOptionInt(&statement, "adbc.odbc.batch_size", 1000, &error),
          &error);
  int64_t batch_size = 0;
  ADBC_OK(
      fx->driver.StatementGetOptionInt(&statement, "adbc.odbc.batch_size", &batch_size, &error),
      &error);
  CHECK_EQ_INT(batch_size, 1000);

  ADBC_MUST(
      fx->driver.StatementSetSqlQuery(&statement, "SELECT x, name FROM big ORDER BY x", &error),
      &error);
  int64_t rows_affected = 0;
  ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, &rows_affected, &error),
            &error);

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  CHECK_EQ_INT(stream.get_schema(&stream, &schema), 0);
  CHECK_EQ_INT(schema.n_children, 2);
  if (schema.n_children != 2) Fatal("unexpected result schema");
  struct ArrowArrayView view;
  struct ArrowError na_error;
  NA_MUST(ArrowArrayViewInitFromSchema(&view, &schema, &na_error));

  int64_t total = 0;
  int batches = 0;
  int64_t last_value = -1;
  char last_name[64];
  last_name[0] = '\0';
  while (1) {
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    CHECK_EQ_INT(stream.get_next(&stream, &array), 0);
    if (array.release == NULL) break;
    NA_MUST(ArrowArrayViewSetArray(&view, &array, &na_error));
    for (int64_t i = 0; i < view.length; i++) {
      int64_t v = ArrowArrayViewGetIntUnsafe(view.children[0], i);
      if (v != total + i + 1) {
        CHECK_EQ_INT(v, total + i + 1);
        break;
      }
      last_value = v;
    }
    if (view.length > 0) {
      struct ArrowStringView sv =
          ArrowArrayViewGetStringUnsafe(view.children[1], view.length - 1);
      snprintf(last_name, sizeof(last_name), "%.*s", (int)sv.size_bytes, sv.data);
    }
    total += array.length;
    batches++;
    ArrowArrayRelease(&array);
  }
  CHECK_EQ_INT(total, 3000);
  CHECK_EQ_INT(batches, 3);
  CHECK_EQ_INT(last_value, 3000);
  CHECK_EQ_STR(last_name, "row3000");

  ArrowArrayViewReset(&view);
  ArrowSchemaRelease(&schema);
  stream.release(&stream);
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  ReleaseError(&error);
}

static void TestExecuteSchema(struct Fixture* fx) {
  Section("StatementExecuteSchema");
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
  ADBC_MUST(
      fx->driver.StatementSetSqlQuery(&statement, "SELECT i, s FROM t WHERE i > 0", &error),
      &error);
  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  if (ADBC_OK(fx->driver.StatementExecuteSchema(&statement, &schema, &error), &error)) {
    CHECK_EQ_STR(schema.format, "+s");
    CHECK_EQ_INT(schema.n_children, 2);
    if (schema.n_children == 2) {
      CHECK_EQ_STR(schema.children[0]->name, "i");
      CHECK_EQ_STR(schema.children[1]->name, "s");
      CHECK_EQ_STR(schema.children[1]->format, "u");
    }
    ArrowSchemaRelease(&schema);
  }
  // The statement is still usable after ExecuteSchema.
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  int64_t rows_affected = 0;
  if (ADBC_OK(fx->driver.StatementExecuteQuery(&statement, &stream, &rows_affected, &error),
              &error)) {
    struct BatchStats stats;
    CHECK(DrainStream(&stream, &stats));
    CHECK_EQ_INT(stats.rows, 2);
    stream.release(&stream);
  }
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  ReleaseError(&error);
}

static void TestErrorPropagation(struct Fixture* fx) {
  Section("error propagation");
  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);

  // Unknown table.
  ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELECT * FROM no_such_table", &error),
            &error);
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  AdbcStatusCode status = fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error);
  CHECK(status != ADBC_STATUS_OK);
  CHECK(error.message != NULL && error.message[0] != '\0');
  CHECK(error.sqlstate[0] != '\0');
  CHECK(stream.release == NULL);
  printf("   unknown table: status=%d sqlstate=%.5s message=%.90s\n", (int)status,
         error.sqlstate, error.message ? error.message : "(none)");
  ReleaseError(&error);

  // Syntax error.
  ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELEC bogus FROM", &error), &error);
  memset(&stream, 0, sizeof(stream));
  status = fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error);
  CHECK(status != ADBC_STATUS_OK);
  CHECK(error.message != NULL && error.message[0] != '\0');
  CHECK(error.sqlstate[0] != '\0');
  CHECK(stream.release == NULL);
  printf("   syntax error: status=%d sqlstate=%.5s\n", (int)status, error.sqlstate);
  ReleaseError(&error);

  // Errors do not poison the statement: a valid query still works afterwards.
  ADBC_MUST(
      fx->driver.StatementSetSqlQuery(&statement, "SELECT i FROM t ORDER BY rowid", &error),
      &error);
  memset(&stream, 0, sizeof(stream));
  if (ADBC_OK(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error)) {
    struct BatchStats stats;
    CHECK(DrainStream(&stream, &stats));
    CHECK_EQ_INT(stats.rows, 4);
    stream.release(&stream);
  }

  // Unknown option -> non-OK status with a message.
  status = fx->driver.StatementSetOption(&statement, "adbc.odbc.not_an_option", "x", &error);
  CHECK(status != ADBC_STATUS_OK);
  CHECK(error.message != NULL);
  ReleaseError(&error);

  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  ReleaseError(&error);
}

// Releasing the statement (or reusing it) must not invalidate a result stream
// that is still open.
static void TestReleaseOrdering(struct Fixture* fx) {
  Section("handle release ordering");
  struct AdbcError error = ADBC_ERROR_INIT;

  // (1) Release the statement while the stream is open, then read the stream.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(
        fx->driver.StatementSetSqlQuery(&statement, "SELECT x FROM big ORDER BY x", &error),
        &error);
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
    struct BatchStats stats;
    CHECK(DrainStream(&stream, &stats));
    CHECK_EQ_INT(stats.rows, 3000);
    CHECK(stats.batches >= 2);
    stream.release(&stream);
  }

  // (2) Release the statement while the stream is open, then release the
  //     stream without reading it at all.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    struct ArrowArrayStream stream;
    memset(&stream, 0, sizeof(stream));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(
        fx->driver.StatementSetSqlQuery(&statement, "SELECT x FROM big ORDER BY x", &error),
        &error);
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
    stream.release(&stream);
    CHECK(stream.release == NULL);
  }

  // (3) Execute a second query on the same statement while the first stream is
  //     open; both streams must remain independently readable.
  {
    struct AdbcStatement statement;
    memset(&statement, 0, sizeof(statement));
    struct ArrowArrayStream first;
    struct ArrowArrayStream second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
    ADBC_MUST(
        fx->driver.StatementSetSqlQuery(&statement, "SELECT x FROM big ORDER BY x", &error),
        &error);
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &first, NULL, &error), &error);
    ADBC_MUST(
        fx->driver.StatementSetSqlQuery(&statement, "SELECT i FROM t ORDER BY rowid", &error),
        &error);
    ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &second, NULL, &error), &error);

    struct BatchStats stats;
    CHECK(DrainStream(&second, &stats));
    CHECK_EQ_INT(stats.rows, 4);
    CHECK(DrainStream(&first, &stats));
    CHECK_EQ_INT(stats.rows, 3000);
    second.release(&second);
    // Release the statement before the remaining stream.
    ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
    first.release(&first);
  }

  // The connection is still usable afterwards.
  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  ADBC_MUST(fx->driver.StatementNew(&fx->connection, &statement, &error), &error);
  ADBC_MUST(fx->driver.StatementSetSqlQuery(&statement, "SELECT 1 AS one", &error), &error);
  ADBC_MUST(fx->driver.StatementExecuteQuery(&statement, &stream, NULL, &error), &error);
  struct BatchStats stats;
  CHECK(DrainStream(&stream, &stats));
  CHECK_EQ_INT(stats.rows, 1);
  stream.release(&stream);
  ADBC_MUST(fx->driver.StatementRelease(&statement, &error), &error);
  ReleaseError(&error);
}

static void TestDriverInitVersions(void* handle) {
  Section("AdbcDriverInit versions");
  AdbcDriverInitFunc init = (AdbcDriverInitFunc)dlsym(handle, "AdbcDriverInit");
  if (!init) Fatal("dlsym(AdbcDriverInit)");
  struct AdbcError error = ADBC_ERROR_INIT;

  struct AdbcDriver v10;
  memset(&v10, 0, sizeof(v10));
  ADBC_OK(init(ADBC_VERSION_1_0_0, &v10, &error), &error);
  CHECK(v10.DatabaseNew != NULL);
  CHECK(v10.StatementExecuteQuery != NULL);
  // 1.1.0-only entry points must be left untouched for a 1.0.0 caller.
  CHECK(v10.StatementExecuteSchema == NULL);
  CHECK(v10.ErrorGetDetailCount == NULL);
  if (v10.release) ADBC_OK(v10.release(&v10, &error), &error);

  struct AdbcDriver bogus;
  memset(&bogus, 0, sizeof(bogus));
  AdbcStatusCode status = init(42, &bogus, &error);
  CHECK(status != ADBC_STATUS_OK);
  CHECK(error.message != NULL);
  ReleaseError(&error);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  const char* lib = NULL;
  if (argc > 1) {
    lib = argv[1];
  } else {
    lib = getenv("ADBC_ODBC_DRIVER");
  }
  if (!lib || !*lib) lib = "build/libadbc_driver_odbc.so";

  const char* sqlite_driver = getenv("SQLITE_ODBC_DRIVER");
  if (!sqlite_driver || !*sqlite_driver) sqlite_driver = "SQLite3";

  printf("driver library: %s\n", lib);
  printf("SQLITE_ODBC_DRIVER: %s\n", sqlite_driver);

  struct Fixture fx;
  memset(&fx, 0, sizeof(fx));

  fx.handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!fx.handle) {
    fprintf(stderr, "dlopen(%s) failed: %s\n", lib, dlerror());
    return 1;
  }
  dlerror();
  AdbcDriverInitFunc init = (AdbcDriverInitFunc)dlsym(fx.handle, "AdbcDriverInit");
  const char* dlsym_error = dlerror();
  if (!init || dlsym_error) {
    fprintf(stderr, "dlsym(AdbcDriverInit) failed: %s\n",
            dlsym_error ? dlsym_error : "not found");
    return 1;
  }

  struct AdbcError error = ADBC_ERROR_INIT;
  Section("AdbcDriverInit(1.1.0)");
  ADBC_MUST(init(ADBC_VERSION_1_1_0, &fx.driver, &error), &error);
  CHECK(fx.driver.DatabaseNew != NULL);
  CHECK(fx.driver.DatabaseInit != NULL);
  CHECK(fx.driver.DatabaseSetOption != NULL);
  CHECK(fx.driver.DatabaseGetOption != NULL);
  CHECK(fx.driver.DatabaseRelease != NULL);
  CHECK(fx.driver.ConnectionNew != NULL);
  CHECK(fx.driver.ConnectionInit != NULL);
  CHECK(fx.driver.ConnectionRelease != NULL);
  CHECK(fx.driver.ConnectionGetObjects != NULL);
  CHECK(fx.driver.ConnectionGetTableSchema != NULL);
  CHECK(fx.driver.ConnectionGetTableTypes != NULL);
  CHECK(fx.driver.ConnectionGetInfo != NULL);
  CHECK(fx.driver.StatementNew != NULL);
  CHECK(fx.driver.StatementSetSqlQuery != NULL);
  CHECK(fx.driver.StatementPrepare != NULL);
  CHECK(fx.driver.StatementBind != NULL);
  CHECK(fx.driver.StatementBindStream != NULL);
  CHECK(fx.driver.StatementExecuteQuery != NULL);
  CHECK(fx.driver.StatementRelease != NULL);
  CHECK(fx.driver.StatementExecuteSchema != NULL);
  CHECK(fx.driver.StatementSetOptionInt != NULL);
  CHECK(fx.driver.StatementGetOptionInt != NULL);
  CHECK(fx.driver.ErrorGetDetailCount != NULL);
  CHECK(fx.driver.ErrorGetDetail != NULL);
  CHECK(fx.driver.release != NULL);

  // Scratch database file.
  const char* tmpdir = getenv("TMPDIR");
  if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
  snprintf(fx.db_dir, sizeof(fx.db_dir), "%s/adbc_odbc_ctest_XXXXXX", tmpdir);
  if (!mkdtemp(fx.db_dir)) Fatal("mkdtemp failed");
  snprintf(fx.db_path, sizeof(fx.db_path), "%s/test.db", fx.db_dir);

  snprintf(fx.uri, sizeof(fx.uri), "Driver=%s;Database=%s;", sqlite_driver, fx.db_path);

  Section("database / connection lifecycle");
  ADBC_MUST(fx.driver.DatabaseNew(&fx.database, &error), &error);
  // Init without a URI/DSN must fail cleanly.
  {
    struct AdbcDatabase empty;
    memset(&empty, 0, sizeof(empty));
    ADBC_MUST(fx.driver.DatabaseNew(&empty, &error), &error);
    AdbcStatusCode status = fx.driver.DatabaseInit(&empty, &error);
    CHECK(status == ADBC_STATUS_INVALID_ARGUMENT);
    CHECK(error.message != NULL);
    ReleaseError(&error);
    ADBC_MUST(fx.driver.DatabaseRelease(&empty, &error), &error);
    CHECK(empty.private_data == NULL);
  }
  ADBC_MUST(fx.driver.DatabaseSetOption(&fx.database, "uri", fx.uri, &error), &error);
  {
    char value[1400];
    size_t length = sizeof(value);
    if (ADBC_OK(fx.driver.DatabaseGetOption(&fx.database, "uri", value, &length, &error),
                &error)) {
      CHECK_EQ_INT(length, (int64_t)strlen(fx.uri) + 1);
      CHECK_EQ_STR(value, fx.uri);
    }
  }
  ADBC_MUST(fx.driver.DatabaseInit(&fx.database, &error), &error);
  ADBC_MUST(fx.driver.ConnectionNew(&fx.connection, &error), &error);
  {
    AdbcStatusCode status = fx.driver.ConnectionInit(&fx.connection, &fx.database, &error);
    if (status != ADBC_STATUS_OK) {
      fprintf(stderr, "could not connect using ODBC driver \"%s\": %s\n", sqlite_driver,
              error.message ? error.message : "(no message)");
      ReleaseError(&error);
      if (!getenv("SQLITE_ODBC_DRIVER")) {
        // No SQLite ODBC driver was pointed at: report "skipped" to ctest.
        fprintf(stderr, "set SQLITE_ODBC_DRIVER to run this test; skipping\n");
        return 77;
      }
      return 1;
    }
    g_checks++;
  }

  Section("fixture data");
  RunUpdate(&fx, "CREATE TABLE t (i INTEGER, s TEXT)");
  CHECK_EQ_INT(RunUpdate(&fx, "INSERT INTO t VALUES (1, 'one')"), 1);
  RunUpdate(&fx, "INSERT INTO t VALUES (NULL, NULL)");
  RunUpdate(&fx, "INSERT INTO t VALUES (-7, '')");
  RunUpdate(&fx, "INSERT INTO t VALUES (2147483647, 'h\xc3\xa9llo')");
  RunUpdate(&fx,
            "CREATE TABLE big AS WITH RECURSIVE c(x) AS "
            "(SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 3000) "
            "SELECT x, 'row'||x AS name FROM c");
  RunUpdate(&fx,
            "CREATE TABLE bigtext AS "
            "SELECT hex(randomblob(7)) AS h, 14 AS n UNION ALL "
            "SELECT hex(randomblob(2000)), 4000 UNION ALL "
            "SELECT hex(randomblob(100000)), 200000");

  TestStatementLifecycle(&fx);
  TestSelectValues(&fx);
  TestLargeStrings(&fx);
  TestBatching(&fx);
  TestExecuteSchema(&fx);
  TestErrorPropagation(&fx);
  TestReleaseOrdering(&fx);
  TestDriverInitVersions(fx.handle);

  Section("teardown");
  ADBC_MUST(fx.driver.ConnectionRelease(&fx.connection, &error), &error);
  CHECK(fx.connection.private_data == NULL);
  ADBC_MUST(fx.driver.DatabaseRelease(&fx.database, &error), &error);
  CHECK(fx.database.private_data == NULL);
  ADBC_MUST(fx.driver.release(&fx.driver, &error), &error);
  ReleaseError(&error);

  unlink(fx.db_path);
  rmdir(fx.db_dir);

#if !defined(ADBC_ODBC_TEST_ASAN)
  // Under a sanitizer we keep the library mapped so reports stay symbolized.
  dlclose(fx.handle);
#endif

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures != 0) {
    printf("FAILED\n");
    return 1;
  }
  printf("C SMOKE OK\n");
  return 0;
}
