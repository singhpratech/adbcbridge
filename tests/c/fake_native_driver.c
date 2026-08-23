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

// A stand-in "native" ADBC driver for the delegation tests: it accepts any
// option, remembers it, and hands it back.  Copy it under a name that names a
// database family (libadbc_driver_fake_postgres.so, ..._fake_sqlite.so) and
// point adbc.odbc.delegate.driver at it to see exactly which URI and options
// adbcbridge would have given the real driver.
//
//   SELECT uri            -> one row, one column: the uri option as received
//   SELECT option:<key>   -> one row, one column: that option as received
//   SELECT options        -> one row per option, "key=value"
//
// Setting the option "fake.fail_init" makes AdbcDatabaseInit fail with that
// message (and status ADBC_STATUS_IO, as a connection failure would).

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arrow-adbc/adbc.h>

#include "nanoarrow/nanoarrow.h"

#if defined(_WIN32)
#define FAKE_EXPORT __declspec(dllexport)
#else
#define FAKE_EXPORT __attribute__((visibility("default")))
#endif

struct FakeOptions {
  char** keys;
  char** values;
  size_t count;
};

struct FakeDatabase {
  struct FakeOptions options;
  bool initialized;
};

struct FakeConnection {
  struct FakeDatabase* db;
  // Options set before AdbcConnectionInit; folded into the database's list with
  // a "conn:" prefix at init, so a test can see that they were replayed.
  struct FakeOptions pre;
};

struct FakeStatement {
  struct FakeDatabase* db;
  char* query;
};

static void FakeReleaseError(struct AdbcError* error) {
  free(error->message);
  error->message = NULL;
  error->release = NULL;
}

static void FakeError(struct AdbcError* error, const char* message) {
  if (!error) return;
  if (error->release) error->release(error);
  error->message = strdup(message);
  error->release = FakeReleaseError;
  error->vendor_code = 0;
  memset(error->sqlstate, 0, sizeof(error->sqlstate));
}

static void FakeOptionsSet(struct FakeOptions* opts, const char* key, const char* value) {
  for (size_t i = 0; i < opts->count; i++) {
    if (strcmp(opts->keys[i], key) == 0) {
      free(opts->values[i]);
      opts->values[i] = strdup(value ? value : "");
      return;
    }
  }
  char** keys = realloc(opts->keys, (opts->count + 1) * sizeof(char*));
  if (!keys) return;
  opts->keys = keys;
  char** values = realloc(opts->values, (opts->count + 1) * sizeof(char*));
  if (!values) return;
  opts->values = values;
  opts->keys[opts->count] = strdup(key);
  opts->values[opts->count] = strdup(value ? value : "");
  opts->count++;
}

static const char* FakeOptionsGet(const struct FakeOptions* opts, const char* key) {
  for (size_t i = 0; i < opts->count; i++) {
    if (strcmp(opts->keys[i], key) == 0) return opts->values[i];
  }
  return NULL;
}

static void FakeOptionsFree(struct FakeOptions* opts) {
  for (size_t i = 0; i < opts->count; i++) {
    free(opts->keys[i]);
    free(opts->values[i]);
  }
  free(opts->keys);
  free(opts->values);
  memset(opts, 0, sizeof(*opts));
}

// --- database ---------------------------------------------------------------

static AdbcStatusCode FakeDatabaseNew(struct AdbcDatabase* database, struct AdbcError* error) {
  struct FakeDatabase* db = calloc(1, sizeof(struct FakeDatabase));
  if (!db) return ADBC_STATUS_INTERNAL;
  database->private_data = db;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeDatabaseSetOption(struct AdbcDatabase* database, const char* key,
                                            const char* value, struct AdbcError* error) {
  struct FakeDatabase* db = (struct FakeDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, "fake.reject") == 0) {
    FakeError(error, "the fake driver rejects the option \"fake.reject\"");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (strcmp(key, "username") == 0 || strcmp(key, "password") == 0) {
    // Like adbc_driver_postgresql and adbc_driver_sqlite: only "uri" is taken,
    // unless the test asked for the option to be accepted.
    if (!FakeOptionsGet(&db->options, "fake.accept_credentials")) {
      FakeError(error, "Unknown database option username/password");
      FakeOptionsSet(&db->options, "fake.rejected_credentials", "true");
      return ADBC_STATUS_NOT_IMPLEMENTED;
    }
  }
  FakeOptionsSet(&db->options, key, value);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeDatabaseInit(struct AdbcDatabase* database, struct AdbcError* error) {
  struct FakeDatabase* db = (struct FakeDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  const char* fail = FakeOptionsGet(&db->options, "fake.fail_init");
  if (fail) {
    FakeError(error, fail);
    return ADBC_STATUS_IO;
  }
  db->initialized = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeDatabaseRelease(struct AdbcDatabase* database,
                                          struct AdbcError* error) {
  struct FakeDatabase* db = (struct FakeDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  FakeOptionsFree(&db->options);
  free(db);
  database->private_data = NULL;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeDatabaseGetOption(struct AdbcDatabase* database, const char* key,
                                            char* value, size_t* length,
                                            struct AdbcError* error) {
  struct FakeDatabase* db = (struct FakeDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  const char* v = FakeOptionsGet(&db->options, key);
  if (!v) {
    FakeError(error, "the fake driver has no such option");
    return ADBC_STATUS_NOT_FOUND;
  }
  size_t n = strlen(v) + 1;
  if (*length >= n) memcpy(value, v, n);
  *length = n;
  return ADBC_STATUS_OK;
}

// --- connection -------------------------------------------------------------

static AdbcStatusCode FakeConnectionNew(struct AdbcConnection* connection,
                                        struct AdbcError* error) {
  struct FakeConnection* conn = calloc(1, sizeof(struct FakeConnection));
  if (!conn) return ADBC_STATUS_INTERNAL;
  connection->private_data = conn;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeConnectionSetOption(struct AdbcConnection* connection, const char* key,
                                              const char* value, struct AdbcError* error) {
  struct FakeConnection* conn = (struct FakeConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->db) {
    FakeOptionsSet(&conn->db->options, key, value);
  } else {
    FakeOptionsSet(&conn->pre, key, value);
  }
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeConnectionInit(struct AdbcConnection* connection,
                                         struct AdbcDatabase* database,
                                         struct AdbcError* error) {
  struct FakeConnection* conn = (struct FakeConnection*)connection->private_data;
  struct FakeDatabase* db = (struct FakeDatabase*)database->private_data;
  if (!conn || !db || !db->initialized) {
    FakeError(error, "the fake driver's database is not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  conn->db = db;
  for (size_t i = 0; i < conn->pre.count; i++) {
    char key[256];
    snprintf(key, sizeof(key), "conn:%s", conn->pre.keys[i]);
    FakeOptionsSet(&db->options, key, conn->pre.values[i]);
  }
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeConnectionRelease(struct AdbcConnection* connection,
                                            struct AdbcError* error) {
  struct FakeConnection* conn = (struct FakeConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  FakeOptionsFree(&conn->pre);
  free(conn);
  connection->private_data = NULL;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStringBatch(const char** values, size_t count,
                                      struct ArrowArrayStream* out) {
  struct ArrowSchema schema;
  ArrowSchemaInit(&schema);
  if (ArrowSchemaSetTypeStruct(&schema, 1) != NANOARROW_OK) return ADBC_STATUS_INTERNAL;
  ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING);
  ArrowSchemaSetName(schema.children[0], "value");
  struct ArrowArray array;
  if (ArrowArrayInitFromSchema(&array, &schema, NULL) != NANOARROW_OK) {
    schema.release(&schema);
    return ADBC_STATUS_INTERNAL;
  }
  ArrowArrayStartAppending(&array);
  for (size_t i = 0; i < count; i++) {
    ArrowArrayAppendString(array.children[0], ArrowCharView(values[i]));
  }
  array.length = (int64_t)count;
  if (ArrowArrayFinishBuildingDefault(&array, NULL) != NANOARROW_OK) {
    array.release(&array);
    schema.release(&schema);
    return ADBC_STATUS_INTERNAL;
  }
  if (ArrowBasicArrayStreamInit(out, &schema, 1) != NANOARROW_OK) {
    array.release(&array);
    schema.release(&schema);
    return ADBC_STATUS_INTERNAL;
  }
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

// --- statement --------------------------------------------------------------

static AdbcStatusCode FakeStatementNew(struct AdbcConnection* connection,
                                       struct AdbcStatement* statement,
                                       struct AdbcError* error) {
  struct FakeConnection* conn = (struct FakeConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  struct FakeStatement* stmt = calloc(1, sizeof(struct FakeStatement));
  if (!stmt) return ADBC_STATUS_INTERNAL;
  stmt->db = conn->db;
  statement->private_data = stmt;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStatementRelease(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  struct FakeStatement* stmt = (struct FakeStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  free(stmt->query);
  free(stmt);
  statement->private_data = NULL;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStatementSetSqlQuery(struct AdbcStatement* statement, const char* query,
                                               struct AdbcError* error) {
  struct FakeStatement* stmt = (struct FakeStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  free(stmt->query);
  stmt->query = strdup(query);
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStatementSetOption(struct AdbcStatement* statement, const char* key,
                                             const char* value, struct AdbcError* error) {
  struct FakeStatement* stmt = (struct FakeStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->db) FakeOptionsSet(&stmt->db->options, key, value);
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStatementPrepare(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  (void)statement;
  (void)error;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode FakeStatementExecuteQuery(struct AdbcStatement* statement,
                                                struct ArrowArrayStream* out,
                                                int64_t* rows_affected,
                                                struct AdbcError* error) {
  struct FakeStatement* stmt = (struct FakeStatement*)statement->private_data;
  if (!stmt || !stmt->db) return ADBC_STATUS_INVALID_STATE;
  if (rows_affected) *rows_affected = -1;
  if (!out) return ADBC_STATUS_OK;
  const char* query = stmt->query ? stmt->query : "";
  if (strcmp(query, "options") == 0) {
    size_t count = stmt->db->options.count;
    const char** rows = calloc(count ? count : 1, sizeof(char*));
    char** joined = calloc(count ? count : 1, sizeof(char*));
    if (!rows || !joined) {
      free(rows);
      free(joined);
      return ADBC_STATUS_INTERNAL;
    }
    for (size_t i = 0; i < count; i++) {
      size_t n = strlen(stmt->db->options.keys[i]) + strlen(stmt->db->options.values[i]) + 2;
      joined[i] = malloc(n);
      snprintf(joined[i], n, "%s=%s", stmt->db->options.keys[i], stmt->db->options.values[i]);
      rows[i] = joined[i];
    }
    AdbcStatusCode status = FakeStringBatch(rows, count, out);
    for (size_t i = 0; i < count; i++) free(joined[i]);
    free(joined);
    free(rows);
    return status;
  }
  const char* key = query;
  if (strncmp(query, "option:", 7) == 0) key = query + 7;
  const char* value = FakeOptionsGet(&stmt->db->options, key);
  if (!value) {
    FakeError(error, "the fake driver has no such option");
    return ADBC_STATUS_NOT_FOUND;
  }
  return FakeStringBatch(&value, 1, out);
}

// --- driver init ------------------------------------------------------------

FAKE_EXPORT
AdbcStatusCode AdbcDriverInit(int version, void* raw_driver, struct AdbcError* error) {
  if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0) {
    FakeError(error, "the fake driver only implements ADBC 1.0.0 and 1.1.0");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  struct AdbcDriver* driver = (struct AdbcDriver*)raw_driver;
  memset(driver, 0,
         version == ADBC_VERSION_1_0_0 ? ADBC_DRIVER_1_0_0_SIZE : ADBC_DRIVER_1_1_0_SIZE);
  driver->DatabaseNew = FakeDatabaseNew;
  driver->DatabaseSetOption = FakeDatabaseSetOption;
  driver->DatabaseInit = FakeDatabaseInit;
  driver->DatabaseRelease = FakeDatabaseRelease;
  driver->ConnectionNew = FakeConnectionNew;
  driver->ConnectionSetOption = FakeConnectionSetOption;
  driver->ConnectionInit = FakeConnectionInit;
  driver->ConnectionRelease = FakeConnectionRelease;
  driver->StatementNew = FakeStatementNew;
  driver->StatementRelease = FakeStatementRelease;
  driver->StatementSetSqlQuery = FakeStatementSetSqlQuery;
  driver->StatementSetOption = FakeStatementSetOption;
  driver->StatementPrepare = FakeStatementPrepare;
  driver->StatementExecuteQuery = FakeStatementExecuteQuery;
  if (version >= ADBC_VERSION_1_1_0) { driver->DatabaseGetOption = FakeDatabaseGetOption; }
  return ADBC_STATUS_OK;
}
