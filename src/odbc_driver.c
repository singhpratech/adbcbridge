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

// adbcbridge: ADBC driver entry points backed by ODBC (unixODBC / iODBC / Windows DM).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odbc_delegate.h"
#include "odbc_internal.h"

// ---------------------------------------------------------------------------
// Database

struct OdbcDatabase {
  SQLHENV henv;
  char* connection_string;  // explicit "uri" / adbc.odbc.connection_string
  char* dsn;
  char* username;
  char* password;
  // ADBC_ODBC_OPTION_TUNE: may adbcbridge add connection keywords of its own?
  bool tune;
  struct OdbcReaderOptions reader_opts;
  struct OdbcDelegateOptions delegate;
  // Non-NULL when a native ADBC driver serves this database: every call is
  // forwarded to it and the ODBC environment above is never opened.
  struct OdbcDelegateProxy* proxy;
};

static void OdbcDatabaseFree(struct OdbcDatabase* db) {
  if (!db) return;
  OdbcDelegateProxyRelease(db->proxy);
  if (db->henv) SQLFreeHandle(SQL_HANDLE_ENV, db->henv);
  free(db->connection_string);
  free(db->dsn);
  free(db->username);
  free(db->password);
  OdbcDelegateOptionsRelease(&db->delegate);
  free(db);
}

static AdbcStatusCode SetString(char** dst, const char* value) {
  free(*dst);
  *dst = value ? strdup(value) : NULL;
  return ADBC_STATUS_OK;
}

// Parse `adbc.odbc.prefetch`: rowsets to keep in flight on the fetch thread.
static AdbcStatusCode OdbcParsePrefetchOption(const char* key, const char* value, int64_t* out,
                                              struct AdbcError* error) {
  char* end = NULL;
  long long v = strtoll(value, &end, 10);
  if (end == value || (end && *end) || v < 0 || v > ADBC_ODBC_MAX_PREFETCH) {
    InternalAdbcSetError(error,
                         "Invalid value \"%s\" for %s (expected 0 to disable, or up to %d "
                         "rowsets in flight)",
                         value, key, ADBC_ODBC_MAX_PREFETCH);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  *out = (int64_t)v;
  return ADBC_STATUS_OK;
}

// Parse a "true"/"false" option that pins a quirk otherwise chosen by autodetection.
static AdbcStatusCode OdbcParseBoolOption(const char* key, const char* value, bool* out,
                                          bool* forced, struct AdbcError* error) {
  if (value && (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0 || strcmp(value, "1") == 0)) {
    *out = true;
  } else if (value && (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0 || strcmp(value, "0") == 0)) {
    *out = false;
  } else {
    InternalAdbcSetError(error, "Invalid value \"%s\" for %s (expected true/false)",
                         value ? value : "(null)", key);
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  if (forced) *forced = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseNew(struct AdbcDatabase* database, struct AdbcError* error) {
  struct OdbcDatabase* db = calloc(1, sizeof(struct OdbcDatabase));
  if (!db) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  db->tune = true;
  db->reader_opts.batch_size = ADBC_ODBC_DEFAULT_BATCH_SIZE;
  db->reader_opts.max_bind_bytes = ADBC_ODBC_DEFAULT_MAX_BIND_BYTES;
  db->reader_opts.long_bind_bytes = ADBC_ODBC_DEFAULT_LONG_BIND_BYTES;
  db->reader_opts.rowset_bytes = ADBC_ODBC_DEFAULT_ROWSET_BYTES;
  OdbcDelegateOptionsInit(&db->delegate);
  database->private_data = db;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseSetOption(struct AdbcDatabase* database, const char* key,
                                            const char* value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  AdbcStatusCode delegate_status = ADBC_STATUS_OK;
  if (OdbcDelegateSetOption(&db->delegate, key, value, &delegate_status, error)) {
    return delegate_status;
  }
  // A delegated database is the native driver's: ODBC-only options (batch_size,
  // ...) are meaningless to it and it says so itself.
  if (db->proxy) return OdbcProxyDatabaseSetOption(db->proxy, key, value, error);
  if (strcmp(key, ADBC_OPTION_URI) == 0 || strcmp(key, ADBC_ODBC_OPTION_CONNECTION_STRING) == 0) {
    return SetString(&db->connection_string, value);
  } else if (strcmp(key, ADBC_ODBC_OPTION_DSN) == 0) {
    return SetString(&db->dsn, value);
  } else if (strcmp(key, ADBC_OPTION_USERNAME) == 0) {
    return SetString(&db->username, value);
  } else if (strcmp(key, ADBC_OPTION_PASSWORD) == 0) {
    return SetString(&db->password, value);
  } else if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) {
      InternalAdbcSetError(error, "%s must be a positive integer", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    db->reader_opts.batch_size = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_PREFETCH) == 0) {
    return OdbcParsePrefetchOption(key, value, &db->reader_opts.prefetch, error);
  } else if (strcmp(key, ADBC_ODBC_OPTION_MAX_BIND_BYTES) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) {
      InternalAdbcSetError(error, "%s must be a positive integer", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    db->reader_opts.max_bind_bytes = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_LONG_BIND_BYTES) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) {
      InternalAdbcSetError(error, "%s must be a positive integer", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    db->reader_opts.long_bind_bytes = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_ROWSET_BYTES) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) {
      InternalAdbcSetError(error, "%s must be a positive integer", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    db->reader_opts.rowset_bytes = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_DECIMAL_AS_STRING) == 0) {
    db->reader_opts.decimal_as_string = (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0);
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) {
    return OdbcParseBoolOption(key, value, &db->reader_opts.sqllen_32bit,
                               &db->reader_opts.sqllen_32bit_forced, error);
  } else if (strcmp(key, ADBC_ODBC_OPTION_TUNE) == 0) {
    return OdbcParseBoolOption(key, value, &db->tune, NULL, error);
  }
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcDatabaseInit(struct AdbcDatabase* database, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (!db->connection_string && !db->dsn) {
    InternalAdbcSetError(error,
                         "Must set option \"" ADBC_OPTION_URI "\" (an ODBC connection string, "
                         "e.g. \"Driver=SQLite3;Database=test.db\") or \"" ADBC_ODBC_OPTION_DSN
                         "\"");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }

  // If a native ADBC driver fits this target, let it serve the database and
  // never open ODBC at all.  Any failure in "auto" mode falls through here with
  // a note in adbc.odbc.delegate.last_error.
  struct OdbcDelegateTarget target = {db->connection_string, db->dsn, db->username,
                                      db->password};
  AdbcStatusCode delegate_status =
      OdbcDelegateTryInit(database, OdbcDatabaseInit, &target, &db->delegate, &db->proxy, error);
  if (delegate_status != ADBC_STATUS_OK) return delegate_status;
  if (db->proxy) return ADBC_STATUS_OK;

  // Options meant for a native driver were accepted while delegation was still
  // possible.  It did not happen, so they would be silently dropped: say so.
  const char* held = OdbcDelegateHeldOption(&db->delegate);
  if (held) {
    InternalAdbcSetError(error,
                         "Unknown database option %s (it is only understood by a native ADBC "
                         "driver, and this connection is served by ODBC: %s)",
                         held,
                         db->delegate.last_error && *db->delegate.last_error
                             ? db->delegate.last_error
                             : "delegation was not attempted");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }

  // "postgresql://..." is not an ODBC connection string; unixODBC answers a
  // bare "[IM002] Data source name not found" for it, which says nothing about
  // the real problem.  Translate it for an installed ODBC driver, or explain.
  if (OdbcDelegateIsNativeUri(db->connection_string)) {
    char* translated = NULL;
    RAISE_ADBC(OdbcDelegateNativeUriToOdbc(db->connection_string, db->delegate.last_error,
                                           &translated, error));
    if (translated) {
      free(db->connection_string);
      db->connection_string = translated;
    }
  }

  SQLRETURN ret = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &db->henv);
  if (!SQL_SUCCEEDED(ret)) {
    InternalAdbcSetError(error, "SQLAllocHandle(SQL_HANDLE_ENV) failed");
    return ADBC_STATUS_IO;
  }
  ODBC_CHECK(SQLSetEnvAttr(db->henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0),
             SQL_HANDLE_ENV, db->henv, "SQLSetEnvAttr(SQL_OV_ODBC3)", error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseRelease(struct AdbcDatabase* database,
                                          struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  (void)error;
  OdbcDatabaseFree(db);
  database->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseGetOption(struct AdbcDatabase* database, const char* key,
                                            char* value, size_t* length,
                                            struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  const char* v = NULL;
  if (OdbcDelegateGetOption(&db->delegate, key, &v)) {
    // fall through to the copy-out below
  } else if (db->proxy) {
    return OdbcProxyDatabaseGetOption(db->proxy, key, value, length, error);
  } else if (strcmp(key, ADBC_OPTION_URI) == 0) v = db->connection_string;
  else if (strcmp(key, ADBC_ODBC_OPTION_DSN) == 0) v = db->dsn;
  else if (strcmp(key, ADBC_OPTION_USERNAME) == 0) v = db->username;
  else if (strcmp(key, ADBC_ODBC_OPTION_TUNE) == 0) {
    v = db->tune ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  }
  else {
    InternalAdbcSetError(error, "Unknown database option %s", key);
    return ADBC_STATUS_NOT_FOUND;
  }
  if (!v) v = "";
  size_t n = strlen(v) + 1;
  if (*length >= n) memcpy(value, v, n);
  *length = n;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseGetOptionInt(struct AdbcDatabase* database, const char* key,
                                               int64_t* value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseGetOptionInt(db->proxy, key, value, error);
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) { *value = db->reader_opts.batch_size; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_MAX_BIND_BYTES) == 0) { *value = db->reader_opts.max_bind_bytes; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_LONG_BIND_BYTES) == 0) { *value = db->reader_opts.long_bind_bytes; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_ROWSET_BYTES) == 0) { *value = db->reader_opts.rowset_bytes; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) { *value = db->reader_opts.sqllen_32bit ? 1 : 0; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_TUNE) == 0) { *value = db->tune ? 1 : 0; return ADBC_STATUS_OK; }
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcDatabaseSetOptionInt(struct AdbcDatabase* database, const char* key,
                                               int64_t value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseSetOptionInt(db->proxy, key, value, error);
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return OdbcDatabaseSetOption(database, key, buf, error);
}

// The remaining ADBC 1.1.0 database entry points: ODBC has nothing to say about
// them, but a delegated database is the native driver's, and it may well have.
static AdbcStatusCode OdbcDatabaseSetOptionDouble(struct AdbcDatabase* database, const char* key,
                                                  double value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseSetOptionDouble(db->proxy, key, value, error);
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcDatabaseSetOptionBytes(struct AdbcDatabase* database, const char* key,
                                                 const uint8_t* value, size_t length,
                                                 struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseSetOptionBytes(db->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcDatabaseGetOptionDouble(struct AdbcDatabase* database, const char* key,
                                                  double* value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseGetOptionDouble(db->proxy, key, value, error);
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcDatabaseGetOptionBytes(struct AdbcDatabase* database, const char* key,
                                                 uint8_t* value, size_t* length,
                                                 struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
  if (db->proxy) return OdbcProxyDatabaseGetOptionBytes(db->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Connection

static AdbcStatusCode OdbcConnectionNew(struct AdbcConnection* connection,
                                        struct AdbcError* error) {
  struct OdbcConnection* conn = calloc(1, sizeof(struct OdbcConnection));
  if (!conn) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  conn->autocommit = true;
  connection->private_data = conn;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionSetAutocommit(struct OdbcConnection* conn, bool on,
                                                  struct AdbcError* error) {
  if (conn->connected) {
    ODBC_CHECK(SQLSetConnectAttr(conn->hdbc, SQL_ATTR_AUTOCOMMIT,
                                 (SQLPOINTER)(uintptr_t)(on ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF),
                                 0),
               SQL_HANDLE_DBC, conn->hdbc, "SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)", error);
  }
  conn->autocommit = on;
  return ADBC_STATUS_OK;
}

// Remember an option set before AdbcConnectionInit: if the database turns out
// to be delegated, the native connection has to be told about it too.  Returns
// the slot to fill in (emptied of any previous value for `key`), or NULL when
// the option is not one to keep.
static struct OdbcPreOption* OdbcConnectionPreOption(struct OdbcConnection* conn,
                                                     const char* key) {
  if (conn->connected) return NULL;
  if (strncmp(key, "adbc.odbc.", 10) == 0) return NULL;  // ours, never a native driver's
  for (size_t i = 0; i < conn->pre_count; i++) {
    if (strcmp(conn->pre[i].key, key) == 0) {
      free(conn->pre[i].value);
      free(conn->pre[i].bytes);
      char* keep = conn->pre[i].key;
      memset(&conn->pre[i], 0, sizeof(conn->pre[i]));
      conn->pre[i].key = keep;
      return &conn->pre[i];
    }
  }
  struct OdbcPreOption* bigger = realloc(conn->pre, (conn->pre_count + 1) * sizeof(*bigger));
  if (!bigger) return NULL;
  conn->pre = bigger;
  struct OdbcPreOption* slot = &conn->pre[conn->pre_count];
  memset(slot, 0, sizeof(*slot));
  slot->key = strdup(key);
  if (!slot->key) return NULL;
  conn->pre_count++;
  return slot;
}

static void OdbcConnectionRecordPreOption(struct OdbcConnection* conn, const char* key,
                                          const char* value) {
  struct OdbcPreOption* slot = OdbcConnectionPreOption(conn, key);
  if (!slot) return;
  slot->type = ODBC_PRE_OPTION_STRING;
  slot->value = value ? strdup(value) : NULL;
}

// Would an option the ODBC path just refused still make sense to a native
// driver?  Before AdbcConnectionInit there is no database to ask -- conn->proxy
// only comes into existence there -- so such an option is held rather than
// refused: it is replayed on the native connection at init
// (OdbcProxyConnectionInit) and reported as unknown only if the connection ends
// up on ODBC after all (OdbcConnectionInit).  This mirrors what the database
// does with the adbc.* options set before AdbcDatabaseInit.
static bool OdbcConnectionCanHold(const struct OdbcConnection* conn, const char* key,
                                  AdbcStatusCode odbc_status) {
  return odbc_status == ADBC_STATUS_NOT_IMPLEMENTED && !conn->connected && !conn->proxy &&
         strncmp(key, "adbc.odbc.", 10) != 0;
}

// Note `key` as held and drop the ODBC path's "unknown option" complaint, which
// is not the answer until the connection has been initialized.
static AdbcStatusCode OdbcConnectionHeld(struct OdbcConnection* conn, const char* key,
                                         struct AdbcError* error) {
  if (!conn->held_option) conn->held_option = strdup(key);
  if (error && error->release) error->release(error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionSetOptionOdbc(struct AdbcConnection* connection,
                                                  const char* key, const char* value,
                                                  struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
    if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) return OdbcConnectionSetAutocommit(conn, true, error);
    if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) return OdbcConnectionSetAutocommit(conn, false, error);
    InternalAdbcSetError(error, "Invalid value for %s: %s", key, value);
    return ADBC_STATUS_INVALID_ARGUMENT;
  } else if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) return ADBC_STATUS_INVALID_ARGUMENT;
    conn->reader_opts.batch_size = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_PREFETCH) == 0) {
    return OdbcParsePrefetchOption(key, value, &conn->reader_opts.prefetch, error);
  } else if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) {
    return OdbcParseBoolOption(key, value, &conn->reader_opts.sqllen_32bit,
                               &conn->reader_opts.sqllen_32bit_forced, error);
  } else if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0) {
    if (!conn->connected) return ADBC_STATUS_INVALID_STATE;
    ODBC_CHECK(SQLSetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, (SQLPOINTER)value, SQL_NTS),
               SQL_HANDLE_DBC, conn->hdbc, "SQLSetConnectAttr(SQL_ATTR_CURRENT_CATALOG)", error);
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionSetOption(struct AdbcConnection* connection, const char* key,
                                              const char* value, struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionSetOption(conn->proxy, key, value, error);
  AdbcStatusCode status = OdbcConnectionSetOptionOdbc(connection, key, value, error);
  if (status == ADBC_STATUS_OK) {
    OdbcConnectionRecordPreOption(conn, key, value);
    return status;
  }
  if (!OdbcConnectionCanHold(conn, key, status)) return status;
  OdbcConnectionRecordPreOption(conn, key, value);
  return OdbcConnectionHeld(conn, key, error);
}

// Lowercased first column of the first row of `sql`, or "" if it cannot be had.  One
// driver can front many different servers -- psqlodbc drives every PostgreSQL-wire
// backend and reports the same SQL_DRIVER_NAME and SQL_DBMS_NAME for all of them -- so
// where the driver name says nothing, ask the server itself.  Errors are swallowed: a
// server that does not understand the query simply is not the one being looked for.
static void OdbcServerScalarString(SQLHDBC hdbc, const char* sql, char* out, size_t out_size) {
  out[0] = '\0';
  SQLHSTMT hstmt = NULL;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return;
  if (SQL_SUCCEEDED(SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS)) && SQL_SUCCEEDED(SQLFetch(hstmt))) {
    SQLLEN ind = 0;
    if (!SQL_SUCCEEDED(SQLGetData(hstmt, 1, SQL_C_CHAR, out, (SQLLEN)out_size, &ind)) ||
        ind == SQL_NULL_DATA) {
      out[0] = '\0';
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  for (char* c = out; *c; c++) {
    if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
  }
}

// Lowercased SELECT version() of the server behind the connection, or "" if it cannot
// be had.
static void OdbcServerVersionString(SQLHDBC hdbc, char* out, size_t out_size) {
  OdbcServerScalarString(hdbc, "SELECT version()", out, out_size);
}

// Did the last call on this handle leave a diagnostic record?
static bool OdbcHasDiag(SQLHDBC hdbc) {
  SQLCHAR state[7] = {0}, msg[8] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  // A tiny message buffer is enough: only the record's existence is being asked about,
  // and truncation answers SQL_SUCCESS_WITH_INFO, which still counts as one.  Reading a
  // record does not clear the queue, so OdbcSetError still finds it afterwards.
  return SQL_SUCCEEDED(
      SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, state, &native, msg, sizeof(msg), &len));
}

// Retry a connect that failed *silently* through the driver's wide entry point.
//
// This driver connects with the narrow SQLDriverConnect, which unixODBC hands straight
// to a driver that exports one.  A driver may implement only its wide connect properly:
// the OpenSearch SQL ODBC driver's CC_connect() asks the server for the "SQL_ASCII"
// client encoding unless SQLDriverConnectW marked the connection as running in the
// Unicode driver -- and the only encoding it supports is UTF8 -- so its ANSI connect
// always fails.  It fails through a path that logs rather than sets an error, so
// SQL_ERROR comes back with an empty diagnostic queue.  A quirk cannot help: quirks are
// detected on a live connection, and this is what fails to make one.
//
// The empty diagnostic queue is also the guard.  A connect that failed and said why --
// bad credentials, no such host -- is a real answer and is left alone rather than
// attempted a second time, which for a locking-out server would cost a second bad
// login; only a driver that refused without a word is asked again the other way.
//
// `s` is the UTF-8 connection string; the caller keeps ownership.
static SQLRETURN OdbcDriverConnectWide(SQLHDBC hdbc, const char* s) {
  int64_t n = (int64_t)strlen(s);
  SQLWCHAR* w = (SQLWCHAR*)malloc((size_t)(n + 1) * sizeof(SQLWCHAR));
  if (!w) return SQL_ERROR;
  int64_t units = OdbcUtf8ToUtf16Into(w, s, n);
  SQLRETURN ret =
      SQLDriverConnectW(hdbc, NULL, w, (SQLSMALLINT)units, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
  free(w);
  return ret;
}

// Per-driver workarounds, keyed on SQL_DRIVER_NAME (or SQL_DBMS_NAME for a driver that
// does not implement it), plus the capability probes the reader needs.
static void OdbcDetectQuirks(struct OdbcConnection* conn) {
  // Capabilities the driver reports for itself.
  {
    SQLUINTEGER parc = 0;
    SQLUSMALLINT txn = 0;
    SQLSMALLINT n = 0;
    conn->reader_opts.param_array_row_counts = SQL_PARC_BATCH;
    if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_PARAM_ARRAY_ROW_COUNTS, &parc, sizeof(parc), &n)) &&
        parc == SQL_PARC_NO_BATCH) {
      conn->reader_opts.param_array_row_counts = SQL_PARC_NO_BATCH;
    }
    conn->reader_opts.txn_capable = true;
    if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_TXN_CAPABLE, &txn, sizeof(txn), &n)) &&
        txn == SQL_TC_NONE) {
      conn->reader_opts.txn_capable = false;
    }
    // How long a statement the driver will take, for the multi-row INSERT ingest path.
    // ODBC has no "maximum parameters" info type at all and most drivers answer 0 =
    // unknown even for this one, so it is an upper bound where it is given and nothing
    // where it is not; the real ceiling is probed (see MultiRowSetup).
    SQLUINTEGER stmt_len = 0;
    if (SQL_SUCCEEDED(
            SQLGetInfo(conn->hdbc, SQL_MAX_STATEMENT_LEN, &stmt_len, sizeof(stmt_len), &n))) {
      conn->reader_opts.max_statement_len = (int64_t)stmt_len;
    }
  }

  SQLCHAR name[256] = {0};
  SQLSMALLINT len = 0;

  // Can SQLGetData re-read a bound column of an arbitrary row of a block cursor?  If
  // so the reader can bind long columns and repair only the truncated values.
  SQLUINTEGER gd = 0;
  if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_GETDATA_EXTENSIONS, &gd, sizeof(gd), NULL))) {
    const SQLUINTEGER need = SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER;
    conn->reader_opts.getdata_repair = (gd & need) == need;
    conn->reader_opts.getdata_bound = (gd & SQL_GD_BOUND) != 0;
  }

  // Can an earlier row of this cursor be read again?  SQL_CA1_ABSOLUTE on the
  // forward-only cursor -- the cursor type the reader uses -- says SQLFetchScroll can
  // reposition without asking for a scrollable (and, on a client/server driver, far
  // more expensive) cursor type.
  SQLUINTEGER ca1 = 0;
  if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, &ca1,
                               sizeof(ca1), NULL))) {
    conn->reader_opts.refetch_repair = (ca1 & SQL_CA1_ABSOLUTE) != 0;
  }

  if (!SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DRIVER_NAME, name, sizeof(name), &len))) {
    // MDB Tools does not implement SQL_DRIVER_NAME at all (SQLGetInfo returns SQL_ERROR),
    // so fall back to the DBMS name to identify it. Drivers that answer SQL_DRIVER_NAME
    // are still keyed on that.
    name[0] = 0;
    len = 0;
    if (!SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DBMS_NAME, name, sizeof(name), &len))) return;
  }
  if (len > (SQLSMALLINT)sizeof(name)) len = (SQLSMALLINT)sizeof(name);
  for (SQLSMALLINT i = 0; i < len && i < (SQLSMALLINT)sizeof(name); i++) {
    if (name[i] >= 'A' && name[i] <= 'Z') name[i] = (SQLCHAR)(name[i] - 'A' + 'a');
  }
  if (strstr((const char*)name, "duckdb")) {
    // DuckDB ODBC writes a full 2048-row vector into bound buffers regardless of
    // SQL_ATTR_ROW_ARRAY_SIZE (heap overflow otherwise) and misaligns rows when the
    // array size is not a multiple of 2048.
    conn->reader_opts.min_buffer_rows = 2048;
    // DuckDB reports SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER but rejects
    // SQLSetPos(SQL_POSITION) outright, so SQLGetData cannot re-read a row of a block
    // cursor: it answers for whichever row its chunk cursor sits on and SQL_NO_DATA for
    // the rest.  A clipped value is not recoverable here, so wide columns stay unbound.
    conn->reader_opts.getdata_repair = false;
    conn->reader_opts.getdata_bound = false;
    conn->reader_opts.bool_param_as_int = true;
    conn->reader_opts.decimal_param_as_varchar = true;
    // SQLDescribeParam throws an uncaught duckdb::BinderException -- which aborts the
    // whole process, not just the call -- for any parameter whose type the binder
    // cannot infer, e.g. the "?" in "SELECT 1 + ?".
    conn->reader_opts.no_describe_param = true;
    // DuckDB accepts SQL_ATTR_PARAMSET_SIZE but ignores the indicator array that goes
    // with a column-wise parameter array: NULL parameter sets land as zeros and the
    // values of the sets around them are dropped.  Row-at-a-time only.
    conn->reader_opts.no_param_arrays = true;
  }
  if (strstr((const char*)name, "sqlite3odbc")) {
    // SQLiteODBC leaves SQL_CA1_ABSOLUTE out of SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1 and
    // claims it only for the static cursor, but its result set is materialised in memory
    // and SQLFetchScroll(SQL_FETCH_ABSOLUTE) re-reads any row of a forward-only cursor
    // correctly, with a plain SQLFetch resuming after it.  Saying so lets the reader bind
    // the 65,536-character width it reports for every TEXT column: without a way back to
    // a truncated row a long column cannot be bound at all, and one unbound column costs
    // the whole result set its block cursor.
    conn->reader_opts.refetch_repair = true;
  }
  if (strstr((const char*)name, "clickhouse")) {
    conn->reader_opts.null_param_as_varchar = true;
    conn->reader_opts.nullable_type_format = "Nullable(%s)";
    // clickhouse-odbc applies only the first few sets of a parameter array and never
    // writes SQL_ATTR_PARAMS_PROCESSED_PTR, so there is no way to tell what ran.
    conn->reader_opts.no_param_arrays = true;
    // clickhouse-odbc reports only the whole-second Time for SQL_TYPE_TIME, with no
    // CREATE_PARAMS; a "13:45:10.123456" parameter bound into such a column is stored as
    // NULL without a diagnostic.  Time64(n) holds the fractional seconds (maximum 9).
    conn->reader_opts.fractional_time_type_format = "Time64(%d)";
    conn->reader_opts.fractional_time_max_digits = 9;
  }
  if (strstr((const char*)name, "maodbc")) {
    // The one driver whose parameter arrays beat a multi-row INSERT: maodbc sends a whole
    // bound array as a single COM_STMT_BULK_EXECUTE, which the server applies natively.
    // Interleaved 20,000-row ingests here: arrays 103k rows/s median against 72k for the
    // multi-row form, so keep arrays ahead of it (the multi-row form is still what runs
    // when the caller turns array binding off).
    conn->reader_opts.prefer_param_arrays = true;
    // MariaDB Connector/ODBC reports SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER but
    // ignores SQLSetPos(SQL_POSITION): SQLGetData answers for the first row of the rowset
    // and returns SQL_NO_DATA for every other row, so re-reading a clipped value where it
    // sits would blank it.  It does support SQLFetchScroll(SQL_FETCH_ABSOLUTE), which is
    // how the reader repairs such a rowset instead.
    conn->reader_opts.getdata_repair = false;
    // MariaDB Connector/ODBC reports TIME for SQL_TYPE_TIME with no CREATE_PARAMS, and a
    // bare TIME column is TIME(0): fractional seconds are silently truncated on insert.
    // MariaDB's maximum TIME scale is 6.
    conn->reader_opts.fractional_time_type_format = "TIME(%d)";
    conn->reader_opts.fractional_time_max_digits = 6;
    // MariaDB ColumnStore is a storage engine inside an ordinary MariaDB server, so the
    // driver name and version() are a plain MariaDB's and say nothing about it; ask the
    // server whether the engine is there instead.  Its DDL parser accepts only its own
    // list of type names, and two of the names maodbc's SQLGetTypeInfo answers with are
    // not on it: SQL_LONGVARCHAR is "LONG VARCHAR" and SQL_BIT is "BIT", both refused
    // with "The syntax or the data type(s) is not supported by Columnstore" even though
    // the types themselves (MEDIUMTEXT, TINYINT) exist there.  The standard spellings
    // TEXT and BOOLEAN are accepted, so spell generated ingest DDL in those -- which
    // plain MariaDB accepts just as well, so this costs an InnoDB table nothing.
    char engines[64];
    OdbcServerScalarString(conn->hdbc,
                           "SELECT COUNT(*) FROM information_schema.engines"
                           " WHERE engine = 'Columnstore' AND support IN ('YES', 'DEFAULT')",
                           engines, sizeof(engines));
    if (engines[0] != '\0' && engines[0] != '0') conn->reader_opts.ansi_ddl_type_names = true;
  }
  if (strstr((const char*)name, "odbcfb")) {
    // Firebird's OdbcFb sizes SQL_C_WCHAR buffers in wchar_t (4 bytes) while unixODBC
    // hands it UTF-16: bound strings lose three quarters of their characters and fetched
    // SQL_WVARCHAR columns come back as UTF-32. Stay on the narrow (UTF-8) path.
    conn->reader_opts.wchar_as_utf8 = true;
    // OdbcFb accepts SQL_ATTR_PARAMSET_SIZE and returns success, but executes only the
    // first parameter set and writes neither SQL_ATTR_PARAMS_PROCESSED_PTR nor the
    // parameter-status array -- five bound rows insert one, silently.
    conn->reader_opts.no_param_arrays = true;
    // With arrays gone and no multi-row VALUES in Firebird's dialect either (-104 "Token
    // unknown" at the second row-group's comma), bulk ingest would be one round trip per
    // row.  Firebird's spelling of one INSERT carrying many rows is a UNION ALL of
    // one-row SELECTs over RDB$DATABASE, the system table that has exactly one row.  A
    // parameter alone in a select list is untyped and refused, so the form only works
    // with a CAST around each one -- see MultiRowCastTypes for where the types come from
    // and why the cast cannot lose anything.  Probed like every other form, and only
    // after the standard one has been refused.
    conn->reader_opts.multirow_union_from = "RDB$DATABASE";
  }
  if (strstr((const char*)name, "ignite")) {
    // Apache Ignite's ODBC driver (SQL_DRIVER_NAME "Apache Ignite") has no wide SQL type
    // at all: SQLBindParameter answers HYC00 "Data type is not supported. [typeId=-9]"
    // for SQL_WVARCHAR, before any value is looked at.  Its SQL_C_WCHAR buffers are also
    // sized in wchar_t (4 bytes on Linux) where unixODBC passes UTF-16, the same way
    // Firebird's OdbcFb sizes them.  Its narrow path is UTF-8 -- Ignite stores strings as
    // UTF-8 and the driver hands the bytes straight through -- so use it.
    conn->reader_opts.wchar_as_utf8 = true;
    // Column-wise parameter arrays are accepted and executed, but the NULL indicator is
    // read from the wrong row: Parameter::Write() tests `buffer.GetInputSize()` on the
    // whole bound array -- element offset 0 -- and only then copies the buffer and points
    // it at the row being written.  So every row of a chunk takes row 0's NULL-ness: a
    // NULL in any later row is sent as whatever bytes sit in that row's data slot, which
    // for a character or binary column is a length the server cannot parse -- it drops the
    // connection mid-batch ("Failed to establish connection with any provided hosts" on
    // the next statement).  One execute per row instead; there the indicator is element 0.
    conn->reader_opts.no_param_arrays = true;
  }
  if (strstr((const char*)name, "virtodbc")) {
    // OpenLink Virtuoso ships both an ANSI driver (virtodbc.so) and a Unicode one
    // (virtodbcu.so); both answer SQL_DRIVER_NAME "virtodbc.so", so this keys on either.
    // The ANSI driver has no SQL_C_WCHAR support worth the name: a bound SQL_C_WCHAR
    // parameter is read as if it were narrow, so "héllo 🚀" stores as its first byte pair
    // ("0\0"). Its narrow path is UTF-8 already -- Virtuoso's own charsets are all
    // single-byte, and an unqualified connection passes narrow bytes through -- so stay
    // on it.
    conn->reader_opts.wchar_as_utf8 = true;
    // SQL_C_SBIGINT parameters are read as 0 without a diagnostic (the driver's
    // conversion table has no 64-bit integer); numeric text converts exactly.
    conn->reader_opts.bigint_param_as_string = true;
    // virtodbc accepts SQL_ATTR_PARAMSET_SIZE and reports the right number of affected
    // rows, but binds SQL_C_TYPE_DATE from the first parameter set only: every row of a
    // bound array gets row 0's date, silently.  One execute per row instead.
    conn->reader_opts.no_param_arrays = true;
  }
  if (strstr((const char*)name, "monetdb")) {
    // MonetDBODBClib accepts SQL_ATTR_PARAMSET_SIZE, executes only the first parameter
    // set, reports one affected row and writes neither SQL_ATTR_PARAMS_PROCESSED_PTR nor
    // the parameter-status array -- seven bound rows insert one, silently.
    conn->reader_opts.no_param_arrays = true;
  }
  if (strstr((const char*)name, "verticaodbc")) {
    // The second driver whose parameter arrays beat a multi-row INSERT (after maodbc):
    // Vertica's own client driver turns a bound array into one native bulk load, while a
    // multi-row INSERT stays one row-store insert per statement -- which a column store
    // is the worst case for.  10,000-row ingests here: arrays 148-163k rows/s against
    // 17-20k for the multi-row form, so keep arrays ahead of it.  (The multi-row form is
    // still what runs when the caller turns array binding off.)
    conn->reader_opts.prefer_param_arrays = true;
  }
  if (strstr((const char*)name, "psqlodbc")) {
    // psqlodbc is the driver for every PostgreSQL-wire server (PostgreSQL itself,
    // CockroachDB, YugabyteDB, TimescaleDB, QuestDB, ...), so its name says nothing
    // about the server behind it and no quirk may be keyed on the name alone: it would
    // fire on real PostgreSQL too.  Ask the server who it is instead -- one small query,
    // and only for this one driver.
    char version[256];
    OdbcServerVersionString(conn->hdbc, version, sizeof(version));
    // Bulk ingest may send one array parameter per column instead of K*ncols bound cells
    // (reader_opts.pg_array_ingest).  PostgreSQL itself is the only server here that
    // form is claimed for: it is PostgreSQL's multi-argument unnest, PostgreSQL's array
    // literal syntax and PostgreSQL's assignment casts all at once, and a server that
    // merely speaks the wire protocol owes us none of them.  So: version() must be a
    // PostgreSQL banner ("postgresql <n>...") and must not carry a fork's own marker.
    // TimescaleDB and Citus are extensions on stock PostgreSQL and name themselves
    // nowhere in version(), which is right -- the server underneath is PostgreSQL.
    // Anything else -- CockroachDB, CrateDB, GreptimeDB, Databend, a server not tried
    // here at all -- fails the test and keeps the multi-row INSERT path.
    if (strncmp(version, "postgresql ", 11) == 0) {
      static const char* const kForks[] = {
          "-yb-",        // YugabyteDB ("postgresql 11.2-yb-2.20.1.3-b0 on ...")
          "yugabyte",    //
          "cloudberry",  // Apache Cloudberry, and Greenplum which it forks
          "greenplum",   //
          "opengauss",   // openGauss ("postgresql 9.2.4 (opengauss 5.0.0 ...)")
          "risingwave",  // RisingWave ("postgresql 13.14.0-risingwave-2.0.0 ...")
          "questdb",     // handled below as well; listed so the order does not matter
          "arcadedb",    //
          "materialize", // Materialize
          "cockroach",   // CockroachDB, in case it ever fronts a PostgreSQL banner
          "crate",       // CrateDB
          "greptime",    // GreptimeDB
          "ydb",         // YDB, which otherwise answers with a plain PostgreSQL banner
      };
      bool fork = false;
      for (size_t i = 0; i < sizeof(kForks) / sizeof(*kForks); i++) {
        if (strstr(version, kForks[i])) fork = true;
      }
      conn->reader_opts.pg_array_ingest = !fork;
    }
    if (strstr(version, "questdb")) {
      // QuestDB speaks the PostgreSQL wire protocol over its own time-series engine and
      // its own type system.  psqlodbc answers SQLGetTypeInfo with PostgreSQL's internal
      // type names ("int8", "float8", "bool"), which QuestDB rejects with "unsupported
      // column type"; it does accept the standard spellings (BIGINT, DOUBLE PRECISION,
      // BOOLEAN).  And it parses a boolean parameter only from the words "true"/"false",
      // while psqlodbc sends SQL_BIT as "1"/"0" -- stored as false, silently.
      conn->reader_opts.ansi_ddl_type_names = true;
      conn->reader_opts.bool_param_as_varchar = true;
      // psqlodbc executes a parameter array by inlining the values into one
      // "BEGIN;INSERT ...;INSERT ..." string, where every non-numeric value becomes a
      // string literal ('\x0102' for bytes, '2024-02-29' for a date).  PostgreSQL types
      // those literals from the target column; QuestDB does not convert them at all
      // ("inconvertible types: STRING -> BINARY").  One execute per row instead, which
      // psqlodbc sends as a typed PQexecPrepared.
      conn->reader_opts.no_param_arrays = true;
    } else if (strstr(version, "arcadedb")) {
      // ArcadeDB serves the PostgreSQL wire protocol over its own multi-model engine and
      // emulates enough of pg_catalog for psqlodbc's SQLTables, but not for its
      // SQLColumns: that query nests its joins in parentheses -- "((pg_class c inner join
      // pg_namespace n on ...) inner join pg_attribute a on ...)" -- which ArcadeDB's SQL
      // parser does not take as a query at all, and it calls pg_get_expr(), which ArcadeDB
      // does not have.  The statement fails on the server, psqlodbc still answers
      // SQL_SUCCESS, and the result set is empty: every table looks like it has no
      // columns.  An empty result carries no return code to fall back on, so skip the
      // call and describe "SELECT * FROM <table> WHERE 1=0" instead.
      conn->reader_opts.no_sql_columns = true;
    } else {
      // YDB is the one PostgreSQL-wire server here that version() does not name: it
      // answers with a plain "PostgreSQL 16.10 on x86_64-pc-linux-gnu, compiled by
      // clang ..." banner, indistinguishable from a real PostgreSQL's.  It does name
      // itself in the server_version *parameter status* of the startup handshake
      // ("14.5 (ydb stable-23-4)"), but psqlodbc keeps that to itself -- SQL_DBMS_VER
      // is the bare "14.0.5".  What YDB does do differently is map the server_version
      // *setting* to version() itself, so "SHOW server_version" hands back that whole
      // banner, where PostgreSQL -- and every other server reached over this wire --
      // answers with a bare version number ("16.10").  So: ask, and compare.  One
      // small query, and only when version() matched no other marker.
      char setting[256];
      OdbcServerScalarString(conn->hdbc, "SHOW server_version", setting, sizeof(setting));
      if (setting[0] != '\0' && strcmp(setting, version) == 0) {
        // Every YDB table must have a PRIMARY KEY -- a CREATE TABLE without one is
        // refused outright ("Primary key is required for ydb tables") -- and an ingest
        // payload need not carry a column that could be one: the key may not be NULL,
        // and any ingested column may be.  Add one the server fills in itself, the way
        // GreptimeDB's mandatory TIME INDEX column is added below; the ingested columns
        // are left exactly as they were.
        conn->reader_opts.ddl_extra_column = "adbc_pk SERIAL PRIMARY KEY";
        // Its version() banner is a plain PostgreSQL's, so the array-ingest test above
        // passed it; it is not PostgreSQL and does not get the form.
        conn->reader_opts.pg_array_ingest = false;
        // YDB lists its tables in pg_catalog.pg_class but leaves pg_catalog.pg_attribute
        // empty, so psqlodbc's SQLColumns -- which joins the two -- answers SQL_SUCCESS
        // with a zero-row result set and every table looks like it has no columns.  Same
        // shape as ArcadeDB above, and the same fix: describe "SELECT * FROM <table>
        // WHERE 1=0" instead.
        conn->reader_opts.no_sql_columns = true;
      }
    }
    // Google Cloud Spanner, reached through PGAdapter (the PostgreSQL-wire proxy Google
    // ships for it).  version() is PGAdapter's own claim ("PostgreSQL 14.1", and the
    // -v flag can make it anything), so it identifies nothing; ask for a setting only
    // PGAdapter has instead.  current_setting(..., missing_ok) answers NULL rather than
    // raising on a real PostgreSQL, so this costs one scalar query and no error.
    char ddl_mode[64];
    OdbcServerScalarString(conn->hdbc, "SELECT current_setting('spanner.ddl_transaction_mode', true)",
                           ddl_mode, sizeof(ddl_mode));
    if (ddl_mode[0] != '\0') {
      // PGAdapter's version() is its own claim ("postgresql 14.1"), which the
      // array-ingest test above cannot tell from a real PostgreSQL's; Spanner is not
      // PostgreSQL and does not get the form.
      conn->reader_opts.pg_array_ingest = false;
      // Spanner has no TIMESTAMP WITHOUT TIME ZONE -- its one timestamp type is
      // timestamptz -- and psqlodbc executes a parameter array by inlining the values
      // into one string, where a SQL_TYPE_TIMESTAMP parameter becomes
      // '2024-02-29 13:45:10.123456'::timestamp.  Spanner refuses the cast ("The
      // Postgres Type is not supported: timestamp without time zone") and the whole
      // batch fails.  A batch that binds a timestamp goes one row at a time instead,
      // which psqlodbc sends as a typed parameter Spanner converts to its own timestamp;
      // every other batch -- a bulk ingest of ordinary columns, say -- keeps its
      // parameter array.
      conn->reader_opts.no_timestamp_param_arrays = true;
      // Every Spanner table must have a primary key, and an ingested column cannot be
      // one (it may repeat a value or be NULL), so generated ingest DDL adds a
      // surrogate key column that Spanner fills in itself -- ddl_extra_column, the same
      // mechanism YDB above uses for the same requirement.
      conn->reader_opts.ddl_extra_column =
          "\"adbc_ingest_key\" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY";
      // Spanner allows at most 950 parameters in one statement, and it is the one
      // ceiling multi-row INSERT batching cannot find by asking: PGAdapter prepares a
      // statement with more without complaint and then, at SQLExecute, closes the
      // connection (08S01 "connection lost"), so the halving search has no connection
      // left to halve on and the whole ingest fails.  Measured exactly here -- 948
      // parameters go through, 952 drop the connection -- and it matches the documented
      // limit.  Declaring it keeps the batching on, at 237 four-column rows per INSERT.
      conn->reader_opts.max_statement_params = 950;
    }
  }
  if (strstr((const char*)name, "myodbc") && !conn->reader_opts.txn_capable) {
    // MySQL Connector/ODBC against a server that reports SQL_TC_NONE: not a MySQL or a
    // MariaDB (both are transactional) but one of the analytic warehouses that speak the
    // MySQL wire protocol -- Databend is the verified one.  Those have no prepared
    // statements, so the connector has to run with NO_SSPS=1 and substitute parameters
    // into the SQL text, where it writes dates, timestamps and binaries as MySQL
    // charset-introducer literals (`_binary'...'`) that only MySQL and MariaDB parse.
    // Send those parameters as ordinary quoted text instead.
    conn->reader_opts.temporal_binary_param_as_varchar = true;
    // Its SQLGetTypeInfo answers with MySQL's type system whatever the server is, so
    // ingest DDL has to fall back to portable type names.
    conn->reader_opts.ansi_ddl_type_names = true;
    // The MongoDB BI Connector (mongosqld), which serves the MySQL wire over a MongoDB.
    // Its version() is a bare "5.7.12", so it is identified by SQL_DBMS_VER -- which the
    // connector takes from the handshake, "5.7.12 mongosqld v2.14.22" -- and not by a
    // query.  Its information_schema.columns leaves NUMERIC_PRECISION, NUMERIC_SCALE and
    // CHARACTER_OCTET_LENGTH NULL for every column, and Connector/ODBC 9 builds
    // SQLColumns entirely from information_schema: for a DECIMAL column it runs strtol()
    // on that NULL pointer and segfaults (catalog.cc get_buffer_length), taking the
    // process with it.  A table with no DECIMAL column comes back fine, so nothing in the
    // return code marks the difference -- skip the call and describe a zero-row SELECT.
    SQLCHAR dbms_ver[128] = {0};
    SQLSMALLINT dbms_ver_len = 0;
    if (SQL_SUCCEEDED(
            SQLGetInfo(conn->hdbc, SQL_DBMS_VER, dbms_ver, sizeof(dbms_ver), &dbms_ver_len)) &&
        strstr((const char*)dbms_ver, "mongosqld")) {
      conn->reader_opts.no_sql_columns = true;
    }
    // Which of those warehouses is behind the connector?  Only two need more, and only
    // they pay for the extra query -- GreptimeDB's version() is "8.4.2-GreptimeDB-1.1.4".
    char version[256];
    OdbcServerVersionString(conn->hdbc, version, sizeof(version));
    if (!version[0] || !strstr(version, "greptimedb")) {
      // Apache Doris answers version() with a bare MySQL number ("5.7.99") that says
      // nothing about it, so ask for the one variable that does: @@version_comment is
      // "Doris version doris-2.1.0-...".  Only a server version() did not already
      // identify pays for this second query.
      char comment[256];
      OdbcServerScalarString(conn->hdbc, "SELECT @@version_comment", comment, sizeof(comment));
      if (strstr(comment, "doris")) {
        // Doris is an MPP warehouse: every OLAP table has to say how its rows are
        // spread over the backends, and a CREATE TABLE that does not is refused
        // outright ("Create olap table should contain distribution desc").  Random
        // distribution with an automatic bucket count is the neutral choice for a
        // table whose columns adbc_ingest picks from the payload.
        //   The property matters just as much: without a key clause Doris makes a
        // duplicate-key table out of the *leading* columns, and a table whose first
        // column is a string, float or double is then refused as well ("The olap table
        // first column could not be float, double, string or array, struct, map").
        // enable_duplicate_without_keys_by_default asks for a duplicate table with no
        // key columns at all, so any column order and any column type ingests.
        conn->reader_opts.ddl_table_options =
            "DISTRIBUTED BY RANDOM BUCKETS AUTO"
            " PROPERTIES (\"enable_duplicate_without_keys_by_default\" = \"true\")";
      }
    }
    if (strstr(version, "greptimedb")) {
      // GreptimeDB is a time-series store: every table must declare exactly one TIME
      // INDEX column, which has to be a NOT NULL TIMESTAMP ("Missing time index
      // constraint" otherwise), and an ingest payload need not carry a timestamp at
      // all.  Add one the server fills in itself, and create the table in append mode
      // -- without it GreptimeDB merges rows that share a time index, so rows ingested
      // within the same millisecond would collapse into one.
      conn->reader_opts.ddl_extra_column =
          "greptime_timestamp TIMESTAMP(3) TIME INDEX DEFAULT CURRENT_TIMESTAMP";
      conn->reader_opts.ddl_table_options = "WITH ('append_mode'='true')";
    }
  }
  if (strstr((const char*)name, "arrow flight")) {
    // The Arrow Flight SQL ODBC driver (SQL_DRIVER_NAME "Arrow Flight ODBC Driver"), the
    // one ODBC driver for any Arrow Flight SQL server.  Its SQLColumns builds a result
    // set and describes it, then segfaults inside the first SQLFetch on it -- with no
    // bound columns at all -- so GetObjects has to skip SQLColumns and describe a
    // zero-row SELECT instead.
    conn->reader_opts.no_sql_columns = true;
  }
  if (strstr((const char*)name, "taos_odbc")) {
    // TDengine's own ODBC driver (SQL_DRIVER_NAME "libtaos_odbc.so").  It describes a
    // TIMESTAMP column as SQL_TYPE_TIMESTAMP -- with TIMESTAMP_AS_IS=1, which the
    // matrix entry sets -- but implements no TIMESTAMP_STRUCT conversion for it:
    // SQLBindCol answers "Column converstion to `SQL_C_TYPE_TIMESTAMP[0x5d/93]` not
    // implemented yet" and the whole result set fails.  Its text form is the ISO-8601
    // the timestamp reader already parses.
    conn->reader_opts.timestamp_as_text = true;
    // Same table, same story for a boolean parameter: SQL_C_BIT -> SQL_BIT is "not
    // implemented yet" and the only route into a TDengine BOOL column is an integer
    // described as SQL_TINYINT.
    conn->reader_opts.bool_param_as_tinyint = true;
  }
  if (strstr((const char*)name, "sqora")) {
    // Oracle Instant Client ODBC rejects SQL_C_SBIGINT parameters without a diagnostic.
    conn->reader_opts.bigint_param_as_string = true;
    // Oracle has no multi-row VALUES clause ("INSERT INTO t VALUES (1),(2)" is ORA-00933,
    // "SQL command not properly ended"); its spelling of the same thing is INSERT ALL.
    // Only consulted once the plain form has actually been refused, so a future Oracle
    // that grows one would simply use it.
    conn->reader_opts.multirow_insert_all = true;
  }
  if (strstr((const char*)name, "msodbcsql")) {
    // SQLGetTypeInfo(SQL_LONGVARCHAR) answers "text", which is what generated ingest DDL
    // would otherwise give an Arrow string column -- a type Microsoft deprecated in SQL
    // Server 2005 and that cannot be sorted, grouped, de-duplicated or compared.
    // See ddl_string_type_name.
    conn->reader_opts.ddl_string_type_name = "NVARCHAR(MAX)";
  }
  if (strstr((const char*)name, "db2")) {
    // IBM's CLI driver ("libdb2.a") speaks DRDA to Db2 *and* to Informix, whose DRDA
    // alias is a second listener on the same server, so the driver name says nothing
    // about which of the two is behind it.  Ask the server: Informix answers
    // SQL_DBMS_NAME "IDS/<platform>" ("IDS/UNIX64"), Db2 answers "DB2/LINUXX8664".
    SQLCHAR dbms[64] = {0};
    SQLSMALLINT dbms_len = 0;
    if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DBMS_NAME, dbms, sizeof(dbms), &dbms_len)) &&
        strncmp((const char*)dbms, "IDS", 3) == 0) {
      // Informix converts a SQL_C_WCHAR parameter from UTF-16 in the server and gives up
      // on a surrogate pair: an INSERT of "hello <U+1F680>" fails outright with -415,
      // "Data conversion error".  The same parameter on the narrow path -- which is
      // UTF-8, the database locale being en_us.utf8 -- stores and reads back unchanged.
      conn->reader_opts.wchar_as_utf8 = true;
      // A SQL_C_BIT parameter breaks the DRDA conversation itself: SQL30020N, "syntax
      // error in the communication data stream", after which the connection is dead.
      // Informix describes its BOOLEAN as SMALLINT over DRDA anyway, and an integer
      // parameter stores into one correctly.
      conn->reader_opts.bool_param_as_int = true;
    } else {
      // Db2 proper.  SQLGetTypeInfo(SQL_LONGVARCHAR) names LONG VARCHAR, which IBM
      // deprecated in Db2 9 and which has no bulk-insert path at all: 20,000 rows of
      // (INTEGER, DOUBLE, <string>, DATE) straight through the ODBC API, medians of 3,
      // go in at 737 rows/s as LONG VARCHAR against 516,459 as VARCHAR(32672) -- while
      // VARCHAR(20) manages 429,865 and even CLOB(1M) 402,356, so it is that one type
      // and not the server's write path.  Generated ingest DDL takes the widest VARCHAR
      // instead; see ddl_string_as_max_varchar.
      conn->reader_opts.ddl_string_as_max_varchar = true;
    }
  }
  if (!conn->reader_opts.sqllen_32bit_forced) {
    // IBM Db2's freely downloadable CLI driver package ("linuxx64_odbc_cli.tar.gz")
    // ships a libdb2.so built with 32-bit SQLLEN/SQLULEN even on 64-bit Linux; it
    // reports SQL_DRIVER_NAME "libdb2.a".  The 64-bit-SQLLEN build is the separate
    // libdb2o.so ("libdb2o.a"), which needs no quirk.
    // MDB Tools writes bound-column indicators the same way: a NULL column's low four
    // bytes come back 0xffffffff with the high half untouched.  It is identified through
    // the SQL_DBMS_NAME fallback above, having no SQL_DRIVER_NAME of its own.
    const char* n = (const char*)name;
    conn->reader_opts.sqllen_32bit = (strstr(n, "db2") != NULL && strstr(n, "libdb2o") == NULL &&
                                      strstr(n, "db2o.") == NULL) ||
                                     strstr(n, "mdbtools") != NULL;
  }
}

// --- Connection-keyword auto-tuning (ADBC_ODBC_OPTION_TUNE) -----------------
//
// A few ODBC drivers have connection keywords whose good value depends on how the
// application reads a result set -- something the driver cannot know and the caller
// should not have to.  Where the target driver is recognised, adbcbridge fills those in
// itself, under three rules:
//
//   * a keyword the caller set, in the connection string or in the DSN, is never
//     overridden -- the caller's value wins even where it is the slow one;
//   * nothing that changes what a query returns is ever set.  On psqlodbc that rules out
//     TrueIsMinus1 and LFConversion (both rewrite values), ByteaAsLongVarBinary,
//     TextAsLongVarchar, MaxVarcharSize and UnknownSizes (all change described types or
//     widths, and so the Arrow schema and the DDL bulk ingest generates), and
//     UseDeclareFetch and Protocol themselves (server-side cursors and per-statement
//     SAVEPOINTs are transaction semantics, and not every PostgreSQL-wire server behind
//     psqlodbc has either);
//   * "adbc.odbc.tune=false" turns the whole thing off.
//
// Keep every addition short and worth it.  The LENGTH of a psqlodbc connection string
// moves its fetch loop by up to 10% on its own -- padding one with semantically empty
// ';' characters moves a 1,000,000-row read from 0.485 s to 0.537 s as it crosses a
// malloc size-class boundary -- so a keyword that does not buy a measurable win is not
// free, it is a loss.
static bool OdbcConnKeywordSet(const char* conn, const char* dsn, const char* key) {
  char* v = OdbcConnStringKeyword(conn, dsn, key);
  bool set = v != NULL;
  free(v);
  return set;
}

// Is a numeric psqlodbc keyword on?  psqlodbc reads all of these with atoi().
static bool OdbcConnKeywordIsOn(const char* conn, const char* dsn, const char* key) {
  char* v = OdbcConnStringKeyword(conn, dsn, key);
  bool on = v && atoi(v) != 0;
  free(v);
  return on;
}

static void OdbcTuneConnectionString(const struct OdbcDatabase* db,
                                     struct InternalAdbcStringBuilder* sb) {
  if (!db->tune) return;
  const char* conn = db->connection_string;
  char* own_dsn = NULL;
  const char* dsn = db->dsn;
  if (!dsn) {
    own_dsn = OdbcConnStringKeyword(conn, NULL, "DSN");
    dsn = own_dsn;
  }

  // psqlodbc -- PostgreSQL, and the ten other PostgreSQL-wire servers it drives.
  // "UseDeclareFetch" is psqlodbc's keyword and nobody else's, so a caller having set it
  // identifies the driver by itself, even behind a DSN whose Driver entry names something
  // unrecognisable.
  if (OdbcConnKeywordIsOn(conn, dsn, "UseDeclareFetch") &&
      !OdbcConnKeywordSet(conn, dsn, "Fetch")) {
    // The caller has asked psqlodbc to stream the result set through a server-side cursor
    // instead of materialising all of it client-side (422 MB of peak process RSS for a
    // 1M-row read of a 65 MB table, against 158 MB streaming).  Each FETCH then brings
    // back max(Fetch, SQL_ATTR_ROW_ARRAY_SIZE) rows (qresult.c:977 in psqlodbc 16), so
    // psqlodbc's default Fetch of 100 is inert -- our rowset always wins it -- and the
    // cursor round-trips once per rowset, which costs a quarter of the read.  1M rows of
    // (int4, float8, varchar(20), date) from PostgreSQL 16 at batch_size 1024, medians of
    // 7 interleaved runs: 0.724 s at the default Fetch, 0.578 s at Fetch=8192, 0.564 s at
    // Fetch=32768 -- against 0.577 s for the same read not streaming at all.  Ask for
    // eight rowsets per round trip, bounded so that psqlodbc's own tuple store stays
    // small, which is the point of streaming in the first place.
    int64_t fetch = 8192;
    if (db->reader_opts.batch_size > 8192) {
      fetch = 65536;
    } else if (db->reader_opts.batch_size > 1024) {
      fetch = db->reader_opts.batch_size * 8;
    }
    InternalAdbcStringBuilderAppend(sb, "Fetch=%lld;", (long long)fetch);
  }

  free(own_dsn);
}

// Allocate an SQLHDBC from the database's environment and drive SQLDriverConnect with
// the connection string every connection to this database gets.  Factored out of
// OdbcConnectionInit so that parallel bulk ingest can raise its own worker connections
// (src/odbc_bind.c) without duplicating the string assembly or the wide-connect retry --
// and so a worker connection is, byte for byte, the same connection the caller has.
AdbcStatusCode OdbcOpenHdbc(struct OdbcDatabase* db, SQLHDBC* out, struct AdbcError* error) {
  *out = NULL;
  SQLHDBC hdbc = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_DBC, db->henv, &hdbc), SQL_HANDLE_ENV, db->henv,
             "SQLAllocHandle(SQL_HANDLE_DBC)", error);

  // Assemble the connection string.
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 256);
  if (db->connection_string) {
    InternalAdbcStringBuilderAppend(&sb, "%s", db->connection_string);
    size_t len = strlen(db->connection_string);
    if (len > 0 && db->connection_string[len - 1] != ';') InternalAdbcStringBuilderAppend(&sb, ";");
  }
  if (db->dsn) InternalAdbcStringBuilderAppend(&sb, "DSN=%s;", db->dsn);
  if (db->username) InternalAdbcStringBuilderAppend(&sb, "UID=%s;", db->username);
  if (db->password) InternalAdbcStringBuilderAppend(&sb, "PWD=%s;", db->password);
  OdbcTuneConnectionString(db, &sb);

  SQLRETURN ret =
      SQLDriverConnect(hdbc, NULL, (SQLCHAR*)sb.buffer, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
  if (!SQL_SUCCEEDED(ret) && !OdbcHasDiag(hdbc)) {
    ret = OdbcDriverConnectWide(hdbc, sb.buffer);
  }
  InternalAdbcStringBuilderReset(&sb);
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_DBC, hdbc, "SQLDriverConnect", error);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    return s;
  }
  *out = hdbc;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionInit(struct AdbcConnection* connection,
                                         struct AdbcDatabase* database,
                                         struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!conn || !db) {
    InternalAdbcSetError(error, "Database not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (db->proxy) {
    // A native driver serves this database: stand its connection up instead,
    // replaying whatever was set on this one before init.
    RAISE_ADBC(OdbcProxyConnectionInit(db->proxy, conn->pre, conn->pre_count, &conn->proxy,
                                       error));
    conn->db = db;
    return ADBC_STATUS_OK;
  }
  // Options were held while it was still unknown who would serve this
  // connection (OdbcConnectionCanHold).  ODBC does, and ODBC does not
  // understand them: report the first rather than dropping it, exactly as
  // AdbcDatabaseInit does for a held database option.
  if (conn->held_option) {
    InternalAdbcSetError(error,
                         "Unknown connection option %s (it is only understood by a native ADBC "
                         "driver, and this connection is served by ODBC: %s)",
                         conn->held_option,
                         db->delegate.last_error && *db->delegate.last_error
                             ? db->delegate.last_error
                             : "delegation was not attempted");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  if (!db->henv) {
    InternalAdbcSetError(error, "Database not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  conn->db = db;
  conn->reader_opts = db->reader_opts;

  RAISE_ADBC(OdbcOpenHdbc(db, &conn->hdbc, error));
  conn->connected = true;
  OdbcDetectQuirks(conn);
  if (!conn->autocommit) RAISE_ADBC(OdbcConnectionSetAutocommit(conn, false, error));
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionRelease(struct AdbcConnection* connection,
                                            struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  AdbcStatusCode status = ADBC_STATUS_OK;
  if (conn->proxy) status = OdbcProxyConnectionRelease(conn->proxy, error);
  if (conn->hdbc) {
    if (conn->connected) {
      // ODBC forbids SQLDisconnect while a transaction is open (25000). Releasing a
      // connection discards uncommitted work, so roll back first; otherwise drivers such
      // as sqliteodbc refuse the disconnect and the underlying handle -- and its locks --
      // leak for the rest of the process.
      if (!conn->autocommit) SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, SQL_ROLLBACK);
      if (!SQL_SUCCEEDED(SQLDisconnect(conn->hdbc))) {
        SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, SQL_ROLLBACK);
        SQLDisconnect(conn->hdbc);
      }
    }
    SQLFreeHandle(SQL_HANDLE_DBC, conn->hdbc);
  }
  for (size_t i = 0; i < conn->pre_count; i++) {
    free(conn->pre[i].key);
    free(conn->pre[i].value);
    free(conn->pre[i].bytes);
  }
  free(conn->pre);
  free(conn->held_option);
  free(conn);
  connection->private_data = NULL;
  return status;
}

static AdbcStatusCode OdbcConnectionCommit(struct AdbcConnection* connection,
                                           struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) return OdbcProxyConnectionCommit(conn->proxy, error);
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (conn->autocommit) {
    InternalAdbcSetError(error, "Cannot commit when autocommit is enabled");
    return ADBC_STATUS_INVALID_STATE;
  }
  ODBC_CHECK(SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, SQL_COMMIT), SQL_HANDLE_DBC, conn->hdbc,
             "SQLEndTran(SQL_COMMIT)", error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionRollback(struct AdbcConnection* connection,
                                             struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) return OdbcProxyConnectionRollback(conn->proxy, error);
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (conn->autocommit) {
    InternalAdbcSetError(error, "Cannot rollback when autocommit is enabled");
    return ADBC_STATUS_INVALID_STATE;
  }
  ODBC_CHECK(SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, SQL_ROLLBACK), SQL_HANDLE_DBC, conn->hdbc,
             "SQLEndTran(SQL_ROLLBACK)", error);
  conn->rollback_epoch++;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionGetInfo(struct AdbcConnection* connection,
                                            const uint32_t* info_codes, size_t info_codes_length,
                                            struct ArrowArrayStream* out,
                                            struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) {
    return OdbcProxyConnectionGetInfo(conn->proxy, info_codes, info_codes_length, out, error);
  }
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;

  static const uint32_t kAll[] = {
      ADBC_INFO_VENDOR_NAME,    ADBC_INFO_VENDOR_VERSION,     ADBC_INFO_VENDOR_SQL,
      ADBC_INFO_DRIVER_NAME,    ADBC_INFO_DRIVER_VERSION,     ADBC_INFO_DRIVER_ARROW_VERSION,
      ADBC_INFO_DRIVER_ADBC_VERSION};
  if (!info_codes) {
    info_codes = kAll;
    info_codes_length = sizeof(kAll) / sizeof(kAll[0]);
  }

  struct ArrowSchema schema = {0};
  struct ArrowArray array = {0};
  RAISE_ADBC(InternalAdbcInitConnectionGetInfoSchema(&schema, &array, error));

  SQLCHAR buf[1024];
  SQLSMALLINT len = 0;
  for (size_t i = 0; i < info_codes_length; i++) {
    switch (info_codes[i]) {
      case ADBC_INFO_VENDOR_NAME:
        // The vendor is the DBMS behind the ODBC driver; the "(via ODBC)" suffix is
        // where the fact that adbcbridge is a bridge is reported, so that
        // ADBC_INFO_DRIVER_NAME can stay a stable identity for adbcbridge itself.
        if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DBMS_NAME, buf, sizeof(buf), &len))) {
          char vendor[1100];
          snprintf(vendor, sizeof(vendor), "%s (via ODBC)", (const char*)buf);
          RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], vendor, error));
        }
        break;
      case ADBC_INFO_VENDOR_VERSION:
        if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DBMS_VER, buf, sizeof(buf), &len))) {
          RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], (const char*)buf, error));
        }
        break;
      case ADBC_INFO_VENDOR_SQL:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendInt(&array, info_codes[i], 1, error));
        break;
      case ADBC_INFO_DRIVER_NAME:
        // A stable identity for adbcbridge: it must not vary with the backing ODBC
        // driver, or no quirks file can declare it.  The underlying SQL_DRIVER_NAME
        // is available through the ADBC_ODBC_OPTION_DRIVER_NAME connection option.
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], ADBC_ODBC_DRIVER_NAME, error));
        break;
      case ADBC_INFO_DRIVER_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], ADBC_ODBC_DRIVER_VERSION, error));
        break;
      case ADBC_INFO_DRIVER_ARROW_VERSION:
        // A bare version string ("vX.Y.Z"), not a "<library> <version>" phrase.
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "v" NANOARROW_VERSION, error));
        break;
      case ADBC_INFO_DRIVER_ADBC_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendInt(&array, info_codes[i], ADBC_VERSION_1_1_0, error));
        break;
      default:
        break;
    }
  }
  array.length = array.children[0]->length;
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionGetTableTypes(struct AdbcConnection* connection,
                                                  struct ArrowArrayStream* out,
                                                  struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) return OdbcProxyConnectionGetTableTypes(conn->proxy, out, error);
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle(SQL_HANDLE_STMT)", error);
  SQLRETURN ret = SQLTables(hstmt, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0,
                            (SQLCHAR*)SQL_ALL_TABLE_TYPES, SQL_NTS);
  // The type enumeration is a query of the driver's own making, and a server can reject
  // it while answering an ordinary table listing perfectly well: psqlodbc builds it as
  // "select NULL, NULL, relkind from (select 'r' as relkind union select 'v' ...) as a",
  // which ArcadeDB's SQL parser refuses.  Fall back to the types the server's own tables
  // actually have -- one plain SQLTables listing, deduplicated below.
  bool from_listing = false;
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLTables(SQL_ALL_TABLE_TYPES)", error);
    SQLFreeStmt(hstmt, SQL_CLOSE);
    if (!SQL_SUCCEEDED(SQLTables(hstmt, NULL, 0, NULL, 0, NULL, 0, NULL, 0))) {
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      return s;
    }
    if (error && error->release) error->release(error);
    from_listing = true;
  }
  // Collect column 4 (TABLE_TYPE) into a single-column "table_type" batch.
  struct ArrowSchema schema;
  ArrowSchemaInit(&schema);
  CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&schema, 1), error);
  CHECK_NA(INTERNAL, ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING), error);
  CHECK_NA(INTERNAL, ArrowSchemaSetName(schema.children[0], "table_type"), error);
  schema.children[0]->flags &= ~ARROW_FLAG_NULLABLE;
  struct ArrowArray array;
  CHECK_NA(INTERNAL, ArrowArrayInitFromSchema(&array, &schema, NULL), error);
  CHECK_NA(INTERNAL, ArrowArrayStartAppending(&array), error);
  SQLCHAR buf[256];
  SQLLEN ind = 0;
  int64_t n = 0;
  // Distinct types seen so far, only needed on the listing fallback (where every table
  // repeats its type).  ODBC defines a handful of type names; the cap just bounds the
  // scan on a server that invents its own.
  char seen[16][sizeof(buf)];
  int seen_count = 0;
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    if (SQL_SUCCEEDED(OdbcGetData(hstmt, 4, SQL_C_CHAR, buf, sizeof(buf), &ind,
                                  conn->reader_opts.sqllen_32bit)) &&
        ind != SQL_NULL_DATA) {
      if (from_listing) {
        bool dup = false;
        for (int i = 0; i < seen_count; i++) {
          if (strcmp(seen[i], (const char*)buf) == 0) dup = true;
        }
        if (dup) continue;
        if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
          snprintf(seen[seen_count++], sizeof(seen[0]), "%s", (const char*)buf);
        }
      }
      CHECK_NA(INTERNAL, ArrowArrayAppendString(array.children[0], ArrowCharView((const char*)buf)), error);
      n++;
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  array.length = n;
  struct ArrowError na_error;
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayFinishBuildingDefault(&array, &na_error), &na_error, error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 1), error);
  ArrowBasicArrayStreamSetArray(out, 0, &array);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionGetTableSchema(struct AdbcConnection* connection,
                                                   const char* catalog, const char* db_schema,
                                                   const char* table_name,
                                                   struct ArrowSchema* schema,
                                                   struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) {
    return OdbcProxyConnectionGetTableSchema(conn->proxy, catalog, db_schema, table_name, schema,
                                             error);
  }
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (!table_name) {
    InternalAdbcSetError(error, "table_name must not be NULL");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // Use the driver's identifier quote char to build SELECT * FROM ... WHERE 1=0.
  char q[8];
  OdbcQuoteChar(conn->hdbc, q);
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 256);
  InternalAdbcStringBuilderAppend(&sb, "SELECT * FROM ");
  if (catalog && *catalog) InternalAdbcStringBuilderAppend(&sb, "%s%s%s.", (char*)q, catalog, (char*)q);
  if (db_schema && *db_schema) InternalAdbcStringBuilderAppend(&sb, "%s%s%s.", (char*)q, db_schema, (char*)q);
  InternalAdbcStringBuilderAppend(&sb, "%s%s%s WHERE 1=0", (char*)q, table_name, (char*)q);

  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle(SQL_HANDLE_STMT)", error);
  SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)sb.buffer, SQL_NTS);
  InternalAdbcStringBuilderReset(&sb);
  AdbcStatusCode s;
  if (!SQL_SUCCEEDED(ret)) {
    s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLExecDirect", error);
    if (s == ADBC_STATUS_INVALID_ARGUMENT || s == ADBC_STATUS_UNKNOWN) s = ADBC_STATUS_NOT_FOUND;
  } else {
    s = OdbcDescribeResultSchema(hstmt, &conn->reader_opts, schema, error);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return s;
}

static AdbcStatusCode OdbcConnectionCancel(struct AdbcConnection* connection,
                                           struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) return OdbcProxyConnectionCancel(conn->proxy, error);
  (void)error;
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionGetOption(struct AdbcConnection* connection, const char* key,
                                              char* value, size_t* length,
                                              struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  const char* v = NULL;
  SQLCHAR buf[1024];
  SQLINTEGER outlen = 0;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_ODBC_OPTION_DELEGATED_TO) == 0) {
    v = conn->proxy ? OdbcProxyConnectionName(conn->proxy) : ADBC_ODBC_DELEGATED_TO_ODBC;
  } else if (conn->proxy) {
    return OdbcProxyConnectionGetOption(conn->proxy, key, value, length, error);
  } else if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
    v = conn->autocommit ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) {
    v = conn->reader_opts.sqllen_32bit ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0 && conn->connected) {
    ODBC_CHECK(SQLGetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, buf, sizeof(buf), &outlen),
               SQL_HANDLE_DBC, conn->hdbc, "SQLGetConnectAttr(SQL_ATTR_CURRENT_CATALOG)", error);
    v = (const char*)buf;
  } else if (strcmp(key, ADBC_ODBC_OPTION_DRIVER_NAME) == 0 && conn->connected) {
    SQLSMALLINT slen = 0;
    ODBC_CHECK(SQLGetInfo(conn->hdbc, SQL_DRIVER_NAME, buf, sizeof(buf), &slen), SQL_HANDLE_DBC,
               conn->hdbc, "SQLGetInfo(SQL_DRIVER_NAME)", error);
    v = (const char*)buf;
  } else {
    InternalAdbcSetError(error, "Unknown connection option %s", key);
    return ADBC_STATUS_NOT_FOUND;
  }
  size_t n = strlen(v) + 1;
  if (*length >= n) memcpy(value, v, n);
  *length = n;
  return ADBC_STATUS_OK;
}

// ADBC 1.1.0 connection entry points that ODBC has no answer for, but that a
// native driver behind a delegated connection may implement.
static AdbcStatusCode OdbcConnectionGetStatistics(struct AdbcConnection* connection,
                                                  const char* catalog, const char* db_schema,
                                                  const char* table_name, char approximate,
                                                  struct ArrowArrayStream* out,
                                                  struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) {
    return OdbcProxyConnectionGetStatistics(conn->proxy, catalog, db_schema, table_name,
                                            approximate, out, error);
  }
  InternalAdbcSetError(error, "GetStatistics is not supported over ODBC");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionGetStatisticNames(struct AdbcConnection* connection,
                                                      struct ArrowArrayStream* out,
                                                      struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) return OdbcProxyConnectionGetStatisticNames(conn->proxy, out, error);
  InternalAdbcSetError(error, "GetStatisticNames is not supported over ODBC");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionReadPartition(struct AdbcConnection* connection,
                                                  const uint8_t* serialized_partition,
                                                  size_t serialized_length,
                                                  struct ArrowArrayStream* out,
                                                  struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) {
    return OdbcProxyConnectionReadPartition(conn->proxy, serialized_partition, serialized_length,
                                            out, error);
  }
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  return OdbcConnectionReadPartitionOdbc(conn, serialized_partition, serialized_length, out, error);
}

static AdbcStatusCode OdbcConnectionSetOptionInt(struct AdbcConnection* connection,
                                                 const char* key, int64_t value,
                                                 struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionSetOptionInt(conn->proxy, key, value, error);
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  AdbcStatusCode status = OdbcConnectionSetOptionOdbc(connection, key, buf, error);
  if (status == ADBC_STATUS_OK) {
    OdbcConnectionRecordPreOption(conn, key, buf);
    return status;
  }
  if (!OdbcConnectionCanHold(conn, key, status)) return status;
  // Held as an integer: a native driver that has ConnectionSetOptionInt for this
  // key may well not take the same value spelled as a string.
  struct OdbcPreOption* slot = OdbcConnectionPreOption(conn, key);
  if (!slot) return status;
  slot->type = ODBC_PRE_OPTION_INT;
  slot->number = value;
  return OdbcConnectionHeld(conn, key, error);
}

static AdbcStatusCode OdbcConnectionSetOptionDouble(struct AdbcConnection* connection,
                                                    const char* key, double value,
                                                    struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionSetOptionDouble(conn->proxy, key, value, error);
  // ODBC has no double-valued connection option of its own, so every one of
  // them is a native driver's until the connection says otherwise.
  if (OdbcConnectionCanHold(conn, key, ADBC_STATUS_NOT_IMPLEMENTED)) {
    struct OdbcPreOption* slot = OdbcConnectionPreOption(conn, key);
    if (slot) {
      slot->type = ODBC_PRE_OPTION_DOUBLE;
      slot->real = value;
      return OdbcConnectionHeld(conn, key, error);
    }
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionSetOptionBytes(struct AdbcConnection* connection,
                                                   const char* key, const uint8_t* value,
                                                   size_t length, struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionSetOptionBytes(conn->proxy, key, value, length, error);
  if (OdbcConnectionCanHold(conn, key, ADBC_STATUS_NOT_IMPLEMENTED)) {
    struct OdbcPreOption* slot = OdbcConnectionPreOption(conn, key);
    if (slot) {
      uint8_t* copy = malloc(length ? length : 1);
      if (copy) {
        if (length) memcpy(copy, value, length);
        slot->type = ODBC_PRE_OPTION_BYTES;
        slot->bytes = copy;
        slot->length = length;
        return OdbcConnectionHeld(conn, key, error);
      }
    }
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionGetOptionInt(struct AdbcConnection* connection,
                                                 const char* key, int64_t* value,
                                                 struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionGetOptionInt(conn->proxy, key, value, error);
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) {
    *value = conn->reader_opts.batch_size;
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_PREFETCH) == 0) {
    *value = conn->reader_opts.prefetch;
    return ADBC_STATUS_OK;
  }
  if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) {
    *value = conn->reader_opts.sqllen_32bit ? 1 : 0;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcConnectionGetOptionDouble(struct AdbcConnection* connection,
                                                    const char* key, double* value,
                                                    struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionGetOptionDouble(conn->proxy, key, value, error);
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcConnectionGetOptionBytes(struct AdbcConnection* connection,
                                                   const char* key, uint8_t* value, size_t* length,
                                                   struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->proxy) return OdbcProxyConnectionGetOptionBytes(conn->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcConnectionGetObjectsEntry(struct AdbcConnection* connection,
                                                    int depth, const char* catalog,
                                                    const char* db_schema, const char* table_name,
                                                    const char** table_type,
                                                    const char* column_name,
                                                    struct ArrowArrayStream* out,
                                                    struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (conn && conn->proxy) {
    return OdbcProxyConnectionGetObjects(conn->proxy, depth, catalog, db_schema, table_name,
                                         table_type, column_name, out, error);
  }
  return OdbcConnectionGetObjects(connection, depth, catalog, db_schema, table_name, table_type,
                                  column_name, out, error);
}

void OdbcQuoteChar(SQLHDBC hdbc, char* out) {
  SQLSMALLINT qlen = 0;
  strcpy(out, "\"");
  if (SQL_SUCCEEDED(SQLGetInfo(hdbc, SQL_IDENTIFIER_QUOTE_CHAR, out, 8, &qlen))) {
    if (qlen == 0 || out[0] == ' ') out[0] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Statement

static AdbcStatusCode OdbcStatementNew(struct AdbcConnection* connection,
                                       struct AdbcStatement* statement, struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn || (!conn->connected && !conn->proxy)) {
    InternalAdbcSetError(error, "Connection not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  struct OdbcStatement* stmt = calloc(1, sizeof(struct OdbcStatement));
  if (!stmt) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  if (conn->proxy) {
    AdbcStatusCode status = OdbcProxyStatementNew(conn->proxy, &stmt->proxy, error);
    if (status != ADBC_STATUS_OK) {
      free(stmt);
      return status;
    }
    statement->private_data = stmt;
    return ADBC_STATUS_OK;
  }
  stmt->conn = conn;
  stmt->reader_opts = conn->reader_opts;
  // On by default; drivers whose parameter arrays cannot be trusted opt out through
  // OdbcDetectQuirks, and "adbc.odbc.array_binding" overrides either way.
  stmt->array_binding = !conn->reader_opts.no_param_arrays;
  stmt->ingest_connections = 1;
  statement->private_data = stmt;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementRelease(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) {
    AdbcStatusCode status = OdbcProxyStatementRelease(stmt->proxy, error);
    free(stmt);
    statement->private_data = NULL;
    return status;
  }
  OdbcHandleRefRelease(stmt->ref);
  if (stmt->bind_stream.release) stmt->bind_stream.release(&stmt->bind_stream);
  free(stmt->query);
  free(stmt->ingest_table);
  free(stmt->ingest_catalog);
  free(stmt->ingest_schema);
  free(stmt->ingest_mode);
  free(stmt->ingest_into);
  free(stmt);
  statement->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementSetSqlQuery(struct AdbcStatement* statement, const char* query,
                                               struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementSetSqlQuery(stmt->proxy, query, error);
  free(stmt->query);
  stmt->query = strdup(query);
  stmt->prepared = false;
  stmt->prepare_requested = false;
  stmt->executed = false;
  free(stmt->ingest_table);
  stmt->ingest_table = NULL;
  free(stmt->ingest_into);
  stmt->ingest_into = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementSetOption(struct AdbcStatement* statement, const char* key,
                                             const char* value, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementSetOption(stmt->proxy, key, value, error);
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) return ADBC_STATUS_INVALID_ARGUMENT;
    stmt->reader_opts.batch_size = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_PREFETCH) == 0) {
    return OdbcParsePrefetchOption(key, value, &stmt->reader_opts.prefetch, error);
  } else if (strcmp(key, ADBC_ODBC_OPTION_PARTITIONS) == 0) {
    char* end = NULL;
    long long v = strtoll(value, &end, 10);
    if (end == value || (end && *end) || v < 0 || v > ADBC_ODBC_MAX_PARTITIONS) {
      InternalAdbcSetError(error,
                           "Invalid value \"%s\" for %s (expected 0 for automatic, 1 to "
                           "disable splitting, or up to %d partitions)",
                           value, key, ADBC_ODBC_MAX_PARTITIONS);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    stmt->partitions = (int64_t)v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) {
    return OdbcParseBoolOption(key, value, &stmt->reader_opts.sqllen_32bit,
                               &stmt->reader_opts.sqllen_32bit_forced, error);
  } else if (strcmp(key, ADBC_INGEST_OPTION_TARGET_TABLE) == 0) {
    free(stmt->ingest_table); stmt->ingest_table = value ? strdup(value) : NULL;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_INGEST_OPTION_TARGET_CATALOG) == 0) {
    free(stmt->ingest_catalog); stmt->ingest_catalog = value ? strdup(value) : NULL;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA) == 0) {
    free(stmt->ingest_schema); stmt->ingest_schema = value ? strdup(value) : NULL;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_INGEST_OPTION_MODE) == 0) {
    if (strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE) != 0 &&
        strcmp(value, ADBC_INGEST_OPTION_MODE_APPEND) != 0 &&
        strcmp(value, ADBC_INGEST_OPTION_MODE_REPLACE) != 0 &&
        strcmp(value, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) != 0) {
      InternalAdbcSetError(error, "Invalid ingest mode %s", value);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    free(stmt->ingest_mode); stmt->ingest_mode = strdup(value);
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_INGEST_OPTION_TEMPORARY) == 0) {
    stmt->ingest_temporary = strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_ROWS_PER_INSERT) == 0) {
    char* end = NULL;
    long long v = strtoll(value, &end, 10);
    if (end == value || (end && *end) || v < 0 || v > INT32_MAX) {
      InternalAdbcSetError(error,
                           "Invalid value \"%s\" for %s (expected 0 for automatic, 1 to "
                           "disable, or a row count)",
                           value, key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    stmt->rows_per_insert = (int64_t)v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_INGEST_CONNECTIONS) == 0) {
    char* end = NULL;
    long long v = strtoll(value, &end, 10);
    if (end == value || (end && *end) || v < 1 || v > ADBC_ODBC_MAX_INGEST_CONNECTIONS) {
      InternalAdbcSetError(error,
                           "Invalid value \"%s\" for %s (expected 1 for the caller's own "
                           "connection, or up to %d)",
                           value, key, ADBC_ODBC_MAX_INGEST_CONNECTIONS);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    stmt->ingest_connections = (int64_t)v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_ARRAY_BINDING) == 0) {
    if (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0) {  // "true"
      stmt->array_binding = true;
    } else if (strcmp(value, ADBC_OPTION_VALUE_DISABLED) == 0) {  // "false"
      stmt->array_binding = false;
    } else {
      InternalAdbcSetError(error, "Invalid value \"%s\" for %s (expected true/false)", value, key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcStatementSetOptionInt(struct AdbcStatement* statement, const char* key,
                                                int64_t value, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementSetOptionInt(stmt->proxy, key, value, error);
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return OdbcStatementSetOption(statement, key, buf, error);
}

static AdbcStatusCode OdbcStatementBindStream(struct AdbcStatement* statement,
                                              struct ArrowArrayStream* stream,
                                              struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementBindStream(stmt->proxy, stream, error);
  if (stmt->bind_stream.release) stmt->bind_stream.release(&stmt->bind_stream);
  stmt->bind_stream = *stream;
  memset(stream, 0, sizeof(*stream));
  stmt->has_bind = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementBind(struct AdbcStatement* statement, struct ArrowArray* values,
                                        struct ArrowSchema* schema, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementBind(stmt->proxy, values, schema, error);
  struct ArrowArrayStream stream;
  struct ArrowSchema schema_copy;
  CHECK_NA(INTERNAL, ArrowSchemaDeepCopy(schema, &schema_copy), error);
  CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(&stream, &schema_copy, 1), error);
  ArrowBasicArrayStreamSetArray(&stream, 0, values);
  // AdbcStatementBind consumes both `values` and `schema` (upstream's driver framework moves
  // both into its bound stream). We deep-copied the schema, so release the caller's copy --
  // otherwise its memory is never freed. Symptom: the Java driver manager exports the bound
  // VectorSchemaRoot's schema from a BufferAllocator, so tests/java saw
  // "Memory was leaked by query" when closing the RootAllocator after a parameterised query.
  if (schema->release) schema->release(schema);
  return OdbcStatementBindStream(statement, &stream, error);
}

// Does the first diagnostic left on `hstmt` carry this SQLSTATE?
static bool OdbcStmtStateIs(SQLHSTMT hstmt, const char* state) {
  SQLCHAR st[6] = {0};
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, st, &native, NULL, 0, &len))) {
    return false;
  }
  return strcmp((const char*)st, state) == 0;
}

// Ensure we own a fresh, idle statement handle.
AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt,
                                                struct AdbcError* error) {
  if (stmt->ref && stmt->ref->refcount > 1) {
    // A previous result stream still owns this handle; detach and allocate a new one.
    OdbcHandleRefRelease(stmt->ref);
    stmt->ref = NULL;
    stmt->prepared = false;
  }
  if (stmt->ref && stmt->rollback_epoch != stmt->conn->rollback_epoch) {
    // A rollback happened while this statement held its handle.  A driver whose cursor
    // state the rollback silently invalidated cannot be told about it afterwards: the
    // driver manager tracks cursor state too, so once it believes the cursor is closed it
    // answers SQLCloseCursor with 24000 itself and the driver never hears.  psqlodbc with
    // UseDeclareFetch=1 is left insisting "[HY010] The cursor is open" on every later
    // execute of that statement, for the life of the handle.  Start again with a fresh
    // one: allocating a statement handle is a local call on every driver in the matrix,
    // and this only happens on the first use after a rollback.
    OdbcHandleRefRelease(stmt->ref);
    stmt->ref = NULL;
    stmt->prepared = false;
  }
  if (stmt->ref) {
    // Reusing the handle needs it idle.  SQLCloseCursor answers 24000 ("invalid cursor
    // state") when there was no cursor to close at all, which is the ordinary case here.
    // Any other refusal means the driver will not let this handle go idle again: with
    // psqlodbc's UseDeclareFetch=1, rolling back a transaction while a cursor is still
    // open leaves the handle insisting "[HY010] The cursor is open" for the rest of its
    // life, and every later execute on that statement fails.  Take a fresh handle rather
    // than a dead one -- a statement handle is cheap, and nothing is lost with it but an
    // SQLPrepare that is re-issued on demand.
    SQLRETURN cret = SQLCloseCursor(stmt->ref->hstmt);
    if (!SQL_SUCCEEDED(cret) && !OdbcStmtStateIs(stmt->ref->hstmt, "24000") &&
        !SQL_SUCCEEDED(SQLFreeStmt(stmt->ref->hstmt, SQL_CLOSE))) {
      OdbcHandleRefRelease(stmt->ref);
      stmt->ref = NULL;
      stmt->prepared = false;
    }
  }
  if (!stmt->ref) {
    SQLHSTMT hstmt = NULL;
    ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, stmt->conn->hdbc, &hstmt), SQL_HANDLE_DBC,
               stmt->conn->hdbc, "SQLAllocHandle(SQL_HANDLE_STMT)", error);
    stmt->ref = OdbcHandleRefNew(hstmt);
    if (!stmt->ref) {
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      InternalAdbcSetError(error, "out of memory");
      return ADBC_STATUS_INTERNAL;
    }
  }
  stmt->rollback_epoch = stmt->conn->rollback_epoch;
  return ADBC_STATUS_OK;
}

// Issue the deferred SQLPrepare.
static AdbcStatusCode OdbcStatementDoPrepare(struct OdbcStatement* stmt,
                                             struct AdbcError* error) {
  if (stmt->prepared) return ADBC_STATUS_OK;
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  ODBC_CHECK(SQLPrepare(stmt->ref->hstmt, (SQLCHAR*)stmt->query, SQL_NTS), SQL_HANDLE_STMT,
             stmt->ref->hstmt, "SQLPrepare", error);
  stmt->prepared = true;
  return ADBC_STATUS_OK;
}

// SQLPrepare is deferred rather than issued here.  A prepare costs a full round trip
// on a client/server driver, and it buys nothing for the very common
// prepare-then-execute-once-with-no-parameters shape that DBAPI clients emit for every
// query: SQLExecDirect does the same work in one round trip instead of two.  The
// prepare is issued as soon as something actually needs it -- parameters get bound
// (src/odbc_bind.c), the result schema is asked for, or the statement is executed a
// second time, which is when a real prepared statement starts paying for itself.
// Syntax errors therefore surface from the execute rather than from here.
static AdbcStatusCode OdbcStatementPrepare(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementPrepare(stmt->proxy, error);
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  stmt->prepare_requested = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementExecuteQuery(struct AdbcStatement* statement,
                                                struct ArrowArrayStream* out,
                                                int64_t* rows_affected,
                                                struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementExecuteQuery(stmt->proxy, out, rows_affected, error);
  if (stmt->ingest_table) {
    if (out) {
      InternalAdbcSetError(error, "Bulk ingest does not produce a result set");
      return ADBC_STATUS_INVALID_STATE;
    }
    return OdbcStatementIngest(stmt, rows_affected, error);
  }
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  if (stmt->has_bind) return OdbcStatementExecuteBound(stmt, out, rows_affected, error);
  // Executing the same query again is the point at which a prepared statement starts
  // to pay off, so promote the deferred prepare now.
  if (stmt->prepare_requested && stmt->executed && !stmt->prepared) {
    RAISE_ADBC(OdbcStatementDoPrepare(stmt, error));
  }
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  stmt->executed = true;
  SQLHSTMT hstmt = stmt->ref->hstmt;

  SQLRETURN ret;
  if (stmt->prepared) {
    ret = SQLExecute(hstmt);
    if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA) {
      return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLExecute", error);
    }
  } else {
    ret = SQLExecDirect(hstmt, (SQLCHAR*)stmt->query, SQL_NTS);
    if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA) {
      return OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLExecDirect", error);
    }
  }

  SQLSMALLINT ncols = 0;
  SQLNumResultCols(hstmt, &ncols);
  if (ncols == 0) {
    // Not a result-producing statement.
    if (rows_affected) {
      *rows_affected = (int64_t)OdbcRowCount(hstmt, stmt->reader_opts.sqllen_32bit);
    }
    if (out) {
      // Produce an empty stream with an empty schema.
      struct ArrowSchema schema;
      ArrowSchemaInit(&schema);
      CHECK_NA(INTERNAL, ArrowSchemaSetTypeStruct(&schema, 0), error);
      CHECK_NA(INTERNAL, ArrowBasicArrayStreamInit(out, &schema, 0), error);
    }
    return ADBC_STATUS_OK;
  }
  if (!out) {
    if (rows_affected) {
      *rows_affected = (int64_t)OdbcRowCount(hstmt, stmt->reader_opts.sqllen_32bit);
    }
    SQLCloseCursor(hstmt);
    return ADBC_STATUS_OK;
  }
  if (rows_affected) *rows_affected = -1;
  return OdbcReaderInit(stmt->ref, &stmt->reader_opts, out, error);
}

static AdbcStatusCode OdbcStatementExecuteSchema(struct AdbcStatement* statement,
                                                 struct ArrowSchema* schema,
                                                 struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementExecuteSchema(stmt->proxy, schema, error);
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  RAISE_ADBC(OdbcStatementDoPrepare(stmt, error));
  return OdbcDescribeResultSchema(stmt->ref->hstmt, &stmt->reader_opts, schema, error);
}

static AdbcStatusCode OdbcStatementGetParameterSchema(struct AdbcStatement* statement,
                                                      struct ArrowSchema* schema,
                                                      struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementGetParameterSchema(stmt->proxy, schema, error);
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  // Describing parameters needs a real prepared statement, not the deferred one.
  RAISE_ADBC(OdbcStatementDoPrepare(stmt, error));
  return OdbcDescribeParameterSchema(stmt->ref->hstmt, &stmt->reader_opts, schema, error);
}

static AdbcStatusCode OdbcStatementCancel(struct AdbcStatement* statement,
                                          struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (stmt && stmt->proxy) return OdbcProxyStatementCancel(stmt->proxy, error);
  if (!stmt || !stmt->ref) return ADBC_STATUS_INVALID_STATE;
  ODBC_CHECK(SQLCancel(stmt->ref->hstmt), SQL_HANDLE_STMT, stmt->ref->hstmt, "SQLCancel", error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementGetOptionInt(struct AdbcStatement* statement, const char* key,
                                                int64_t* value, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementGetOptionInt(stmt->proxy, key, value, error);
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) { *value = stmt->reader_opts.batch_size; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_ARRAY_BINDING) == 0) { *value = stmt->array_binding ? 1 : 0; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_ROWS_PER_INSERT) == 0) { *value = stmt->rows_per_insert; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_INGEST_CONNECTIONS) == 0) { *value = stmt->ingest_connections; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_PARTITIONS) == 0) { *value = stmt->partitions; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_PREFETCH) == 0) { *value = stmt->reader_opts.prefetch; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_SQLLEN_32BIT) == 0) { *value = stmt->reader_opts.sqllen_32bit ? 1 : 0; return ADBC_STATUS_OK; }
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcStatementExecutePartitions(struct AdbcStatement* statement,
                                                     struct ArrowSchema* schema,
                                                     struct AdbcPartitions* partitions,
                                                     int64_t* rows_affected,
                                                     struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) {
    return OdbcProxyStatementExecutePartitions(stmt->proxy, schema, partitions, rows_affected,
                                               error);
  }
  if (!partitions) {
    InternalAdbcSetError(error, "ExecutePartitions requires an output partitions struct");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  memset(partitions, 0, sizeof(*partitions));
  return OdbcStatementExecutePartitionsOdbc(stmt, schema, partitions, rows_affected, error);
}

// Statement entry points ODBC does not implement, forwarded when a native driver
// is behind this statement.

static AdbcStatusCode OdbcStatementSetSubstraitPlan(struct AdbcStatement* statement,
                                                    const uint8_t* plan, size_t length,
                                                    struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (stmt && stmt->proxy) return OdbcProxyStatementSetSubstraitPlan(stmt->proxy, plan, length, error);
  InternalAdbcSetError(error, "Substrait plans are not supported over ODBC");
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcStatementSetOptionDouble(struct AdbcStatement* statement,
                                                   const char* key, double value,
                                                   struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementSetOptionDouble(stmt->proxy, key, value, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcStatementSetOptionBytes(struct AdbcStatement* statement, const char* key,
                                                  const uint8_t* value, size_t length,
                                                  struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementSetOptionBytes(stmt->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcStatementGetOption(struct AdbcStatement* statement, const char* key,
                                             char* value, size_t* length,
                                             struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementGetOption(stmt->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcStatementGetOptionDouble(struct AdbcStatement* statement,
                                                   const char* key, double* value,
                                                   struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementGetOptionDouble(stmt->proxy, key, value, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcStatementGetOptionBytes(struct AdbcStatement* statement, const char* key,
                                                  uint8_t* value, size_t* length,
                                                  struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (stmt->proxy) return OdbcProxyStatementGetOptionBytes(stmt->proxy, key, value, length, error);
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Driver init

static AdbcStatusCode OdbcDriverRelease(struct AdbcDriver* driver, struct AdbcError* error) {
  (void)error;
  driver->private_data = NULL;
  return ADBC_STATUS_OK;
}

static const struct AdbcError* OdbcErrorFromArrayStream(struct ArrowArrayStream* stream,
                                                        AdbcStatusCode* status) {
  (void)stream; (void)status;
  return NULL;
}

ADBC_ODBC_EXPORT
AdbcStatusCode AdbcDriverOdbcInit(int version, void* raw_driver, struct AdbcError* error) {
  if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0) {
    InternalAdbcSetError(error, "Only ADBC 1.0.0 and 1.1.0 are supported");
    return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  struct AdbcDriver* driver = (struct AdbcDriver*)raw_driver;
  memset(driver, 0, version == ADBC_VERSION_1_0_0 ? ADBC_DRIVER_1_0_0_SIZE : ADBC_DRIVER_1_1_0_SIZE);

  // The ADBC revision this table was initialized with; nothing else uses
  // private_data.
  driver->private_data = (void*)(intptr_t)version;
  driver->release = OdbcDriverRelease;
  driver->DatabaseInit = OdbcDatabaseInit;
  driver->DatabaseNew = OdbcDatabaseNew;
  driver->DatabaseRelease = OdbcDatabaseRelease;
  driver->DatabaseSetOption = OdbcDatabaseSetOption;

  driver->ConnectionCommit = OdbcConnectionCommit;
  driver->ConnectionGetInfo = OdbcConnectionGetInfo;
  driver->ConnectionGetTableSchema = OdbcConnectionGetTableSchema;
  driver->ConnectionGetTableTypes = OdbcConnectionGetTableTypes;
  driver->ConnectionInit = OdbcConnectionInit;
  driver->ConnectionNew = OdbcConnectionNew;
  driver->ConnectionRelease = OdbcConnectionRelease;
  driver->ConnectionRollback = OdbcConnectionRollback;
  driver->ConnectionSetOption = OdbcConnectionSetOption;

  driver->ConnectionGetObjects = OdbcConnectionGetObjectsEntry;
  driver->ConnectionReadPartition = OdbcConnectionReadPartition;
  driver->StatementBind = OdbcStatementBind;
  driver->StatementExecutePartitions = OdbcStatementExecutePartitions;
  driver->StatementGetParameterSchema = OdbcStatementGetParameterSchema;
  driver->StatementSetSubstraitPlan = OdbcStatementSetSubstraitPlan;
  driver->StatementBindStream = OdbcStatementBindStream;
  driver->StatementExecuteQuery = OdbcStatementExecuteQuery;
  driver->StatementGetParameterSchema = OdbcStatementGetParameterSchema;
  driver->StatementNew = OdbcStatementNew;
  driver->StatementPrepare = OdbcStatementPrepare;
  driver->StatementRelease = OdbcStatementRelease;
  driver->StatementSetOption = OdbcStatementSetOption;
  driver->StatementSetSqlQuery = OdbcStatementSetSqlQuery;

  if (version >= ADBC_VERSION_1_1_0) {
    driver->ErrorGetDetailCount = InternalAdbcCommonErrorGetDetailCount;
    driver->ErrorGetDetail = InternalAdbcCommonErrorGetDetail;
    driver->ErrorFromArrayStream = OdbcErrorFromArrayStream;
    driver->DatabaseGetOption = OdbcDatabaseGetOption;
    driver->DatabaseGetOptionInt = OdbcDatabaseGetOptionInt;
    driver->DatabaseSetOptionInt = OdbcDatabaseSetOptionInt;
    driver->DatabaseGetOptionBytes = OdbcDatabaseGetOptionBytes;
    driver->DatabaseGetOptionDouble = OdbcDatabaseGetOptionDouble;
    driver->DatabaseSetOptionBytes = OdbcDatabaseSetOptionBytes;
    driver->DatabaseSetOptionDouble = OdbcDatabaseSetOptionDouble;
    driver->ConnectionCancel = OdbcConnectionCancel;
    driver->ConnectionGetOption = OdbcConnectionGetOption;
    driver->ConnectionGetOptionBytes = OdbcConnectionGetOptionBytes;
    driver->ConnectionGetOptionDouble = OdbcConnectionGetOptionDouble;
    driver->ConnectionGetOptionInt = OdbcConnectionGetOptionInt;
    driver->ConnectionGetStatistics = OdbcConnectionGetStatistics;
    driver->ConnectionGetStatisticNames = OdbcConnectionGetStatisticNames;
    driver->ConnectionSetOptionBytes = OdbcConnectionSetOptionBytes;
    driver->ConnectionSetOptionDouble = OdbcConnectionSetOptionDouble;
    driver->ConnectionSetOptionInt = OdbcConnectionSetOptionInt;
    driver->StatementCancel = OdbcStatementCancel;
    driver->StatementExecuteSchema = OdbcStatementExecuteSchema;
    driver->StatementGetOption = OdbcStatementGetOption;
    driver->StatementGetOptionBytes = OdbcStatementGetOptionBytes;
    driver->StatementGetOptionDouble = OdbcStatementGetOptionDouble;
    driver->StatementGetOptionInt = OdbcStatementGetOptionInt;
    driver->StatementSetOptionBytes = OdbcStatementSetOptionBytes;
    driver->StatementSetOptionDouble = OdbcStatementSetOptionDouble;
    driver->StatementSetOptionInt = OdbcStatementSetOptionInt;
  }
  return ADBC_STATUS_OK;
}

ADBC_ODBC_EXPORT
AdbcStatusCode AdbcDriverInit(int version, void* driver, struct AdbcError* error) {
  return AdbcDriverOdbcInit(version, driver, error);
}
