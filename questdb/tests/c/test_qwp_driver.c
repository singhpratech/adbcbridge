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

// Smoke test for adbc_driver_questdb: dlopen the library, resolve
// AdbcDriverInit, connect to a QuestDB 10 server over QWP and run a query.
//
//   argv[1]       path to libadbc_driver_questdb.so
//   QUESTDB_URI   ws://host:port (default ws://127.0.0.1:9000)
//
// Without a reachable server the test still checks that the library loads
// and that both ADBC revisions initialise, then exits 0 with a notice.

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"

typedef AdbcStatusCode (*AdbcDriverInitFunc)(int, void*, struct AdbcError*);

static struct AdbcDriver driver;
static int failures = 0;

static void Fail(const char* what, AdbcStatusCode status, struct AdbcError* error) {
  fprintf(stderr, "FAIL %s: status %d: %s\n", what, (int)status, error && error->message ? error->message : "");
  if (error && error->release) error->release(error);
  failures++;
}

#define CHECK(what, expr)                                     \
  do {                                                        \
    struct AdbcError error_;                                  \
    memset(&error_, 0, sizeof(error_));                       \
    AdbcStatusCode st_ = (expr);                              \
    if (st_ != ADBC_STATUS_OK) { Fail(what, st_, &error_); return 1; } \
    printf("ok   %s\n", what);                                \
  } while (0)

int main(int argc, char** argv) {
  const char* lib = argc > 1 ? argv[1] : getenv("ADBC_QUESTDB_DRIVER");
  if (!lib) { fprintf(stderr, "usage: %s <libadbc_driver_questdb.so>\n", argv[0]); return 2; }
  void* handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  AdbcDriverInitFunc init = (AdbcDriverInitFunc)dlsym(handle, "AdbcDriverInit");
  if (!init) { fprintf(stderr, "dlsym(AdbcDriverInit) failed\n"); return 1; }

  struct AdbcError error;
  memset(&error, 0, sizeof(error));
  struct AdbcDriver d10;
  memset(&d10, 0, sizeof(d10));
  if (init(ADBC_VERSION_1_0_0, &d10, &error) != ADBC_STATUS_OK) { Fail("init 1.0.0", 1, &error); return 1; }
  d10.release(&d10, NULL);
  memset(&driver, 0, sizeof(driver));
  if (init(ADBC_VERSION_1_1_0, &driver, &error) != ADBC_STATUS_OK) { Fail("init 1.1.0", 1, &error); return 1; }
  if (!driver.StatementCancel || !driver.ConnectionGetOption) { fprintf(stderr, "1.1.0 table incomplete\n"); return 1; }
  printf("ok   AdbcDriverInit 1.0.0 and 1.1.0\n");

  const char* uri = getenv("QUESTDB_URI");
  if (!uri) uri = "ws://127.0.0.1:9000";

  struct AdbcDatabase database;
  memset(&database, 0, sizeof(database));
  CHECK("DatabaseNew", driver.DatabaseNew(&database, &error_));
  CHECK("DatabaseSetOption(uri)", driver.DatabaseSetOption(&database, "uri", uri, &error_));
  CHECK("DatabaseInit", driver.DatabaseInit(&database, &error_));

  struct AdbcConnection connection;
  memset(&connection, 0, sizeof(connection));
  CHECK("ConnectionNew", driver.ConnectionNew(&connection, &error_));
  {
    struct AdbcError e;
    memset(&e, 0, sizeof(e));
    AdbcStatusCode st = driver.ConnectionInit(&connection, &database, &e);
    if (st != ADBC_STATUS_OK) {
      printf("skip no QuestDB at %s (%s); library load and init verified\n", uri, e.message ? e.message : "");
      if (e.release) e.release(&e);
      driver.ConnectionRelease(&connection, NULL);
      driver.DatabaseRelease(&database, NULL);
      driver.release(&driver, NULL);
      return 0;
    }
    printf("ok   ConnectionInit %s\n", uri);
  }

  struct AdbcStatement statement;
  memset(&statement, 0, sizeof(statement));
  CHECK("StatementNew", driver.StatementNew(&connection, &statement, &error_));
  CHECK("SetSqlQuery", driver.StatementSetSqlQuery(&statement, "SELECT x, x * 1.5 AS d, 'a' || x AS s FROM long_sequence(5)", &error_));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  int64_t rows = -1;
  CHECK("ExecuteQuery", driver.StatementExecuteQuery(&statement, &stream, &rows, &error_));
  struct ArrowSchema schema;
  if (stream.get_schema(&stream, &schema) != 0) { fprintf(stderr, "get_schema failed\n"); return 1; }
  if (schema.n_children != 3) { fprintf(stderr, "expected 3 columns, got %lld\n", (long long)schema.n_children); return 1; }
  printf("ok   schema: %s:%s %s:%s %s:%s\n", schema.children[0]->name, schema.children[0]->format,
         schema.children[1]->name, schema.children[1]->format, schema.children[2]->name, schema.children[2]->format);
  schema.release(&schema);
  int64_t total = 0;
  for (;;) {
    struct ArrowArray batch;
    if (stream.get_next(&stream, &batch) != 0) { fprintf(stderr, "get_next: %s\n", stream.get_last_error(&stream)); return 1; }
    if (!batch.release) break;
    total += batch.length;
    batch.release(&batch);
  }
  stream.release(&stream);
  if (total != 5) { fprintf(stderr, "expected 5 rows, got %lld\n", (long long)total); return 1; }
  printf("ok   5 rows through the Arrow stream\n");

  // ExecuteUpdate on DDL + DML.
  CHECK("drop", driver.StatementSetSqlQuery(&statement, "DROP TABLE IF EXISTS adbc_qwp_smoke", &error_));
  CHECK("drop exec", driver.StatementExecuteQuery(&statement, NULL, &rows, &error_));
  CHECK("create", driver.StatementSetSqlQuery(&statement, "CREATE TABLE adbc_qwp_smoke (id LONG, ts TIMESTAMP) TIMESTAMP(ts) PARTITION BY DAY WAL", &error_));
  CHECK("create exec", driver.StatementExecuteQuery(&statement, NULL, &rows, &error_));
  CHECK("insert", driver.StatementSetSqlQuery(&statement, "INSERT INTO adbc_qwp_smoke SELECT x, timestamp_sequence(0, 1000000) FROM long_sequence(3)", &error_));
  CHECK("insert exec", driver.StatementExecuteQuery(&statement, NULL, &rows, &error_));
  printf("ok   insert rows_affected=%lld\n", (long long)rows);
  if (rows != 3) { fprintf(stderr, "expected rows_affected 3\n"); failures++; }

  // Bound parameter.
  CHECK("bind sql", driver.StatementSetSqlQuery(&statement, "SELECT $1::long * x AS v FROM long_sequence(2)", &error_));
  {
    struct ArrowSchema ps;
    struct ArrowArray pa;
    ArrowSchemaInit(&ps);
    ArrowSchemaSetTypeStruct(&ps, 1);
    ArrowSchemaSetType(ps.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(ps.children[0], "p");
    ArrowArrayInitFromSchema(&pa, &ps, NULL);
    ArrowArrayStartAppending(&pa);
    ArrowArrayAppendInt(pa.children[0], 10);
    ArrowArrayFinishElement(&pa);
    ArrowArrayFinishBuildingDefault(&pa, NULL);
    CHECK("Bind", driver.StatementBind(&statement, &pa, &ps, &error_));
    ps.release(&ps);
  }
  CHECK("ExecuteQuery(bound)", driver.StatementExecuteQuery(&statement, &stream, &rows, &error_));
  total = 0;
  int64_t sum = 0;
  for (;;) {
    struct ArrowArray batch;
    if (stream.get_next(&stream, &batch) != 0) { fprintf(stderr, "get_next: %s\n", stream.get_last_error(&stream)); return 1; }
    if (!batch.release) break;
    for (int64_t i = 0; i < batch.length; i++) sum += ((const int64_t*)batch.children[0]->buffers[1])[batch.children[0]->offset + i];
    total += batch.length;
    batch.release(&batch);
  }
  stream.release(&stream);
  if (total != 2 || sum != 30) { fprintf(stderr, "bound query: rows=%lld sum=%lld\n", (long long)total, (long long)sum); failures++; }
  else printf("ok   bound parameter: 10*1 + 10*2 = 30\n");

  CHECK("cleanup", driver.StatementSetSqlQuery(&statement, "DROP TABLE IF EXISTS adbc_qwp_smoke", &error_));
  CHECK("cleanup exec", driver.StatementExecuteQuery(&statement, NULL, &rows, &error_));

  CHECK("StatementRelease", driver.StatementRelease(&statement, &error_));
  CHECK("ConnectionRelease", driver.ConnectionRelease(&connection, &error_));
  CHECK("DatabaseRelease", driver.DatabaseRelease(&database, &error_));
  driver.release(&driver, NULL);
  dlclose(handle);
  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  printf("PASS\n");
  return 0;
}
