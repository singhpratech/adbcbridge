// SPDX-License-Identifier: Apache-2.0
// adbc-odbc: ADBC driver entry points backed by ODBC (unixODBC / iODBC / Windows DM).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// ---------------------------------------------------------------------------
// Database

struct OdbcDatabase {
  SQLHENV henv;
  char* connection_string;  // explicit "uri" / adbc.odbc.connection_string
  char* dsn;
  char* username;
  char* password;
  struct OdbcReaderOptions reader_opts;
};

static AdbcStatusCode SetString(char** dst, const char* value) {
  free(*dst);
  *dst = value ? strdup(value) : NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseNew(struct AdbcDatabase* database, struct AdbcError* error) {
  struct OdbcDatabase* db = calloc(1, sizeof(struct OdbcDatabase));
  if (!db) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  db->reader_opts.batch_size = ADBC_ODBC_DEFAULT_BATCH_SIZE;
  db->reader_opts.max_bind_bytes = ADBC_ODBC_DEFAULT_MAX_BIND_BYTES;
  database->private_data = db;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseSetOption(struct AdbcDatabase* database, const char* key,
                                            const char* value, struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!db) return ADBC_STATUS_INVALID_STATE;
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
  } else if (strcmp(key, ADBC_ODBC_OPTION_MAX_BIND_BYTES) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) {
      InternalAdbcSetError(error, "%s must be a positive integer", key);
      return ADBC_STATUS_INVALID_ARGUMENT;
    }
    db->reader_opts.max_bind_bytes = v;
    return ADBC_STATUS_OK;
  } else if (strcmp(key, ADBC_ODBC_OPTION_DECIMAL_AS_STRING) == 0) {
    db->reader_opts.decimal_as_string = (strcmp(value, ADBC_OPTION_VALUE_ENABLED) == 0);
    return ADBC_STATUS_OK;
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
  if (db->henv) SQLFreeHandle(SQL_HANDLE_ENV, db->henv);
  free(db->connection_string);
  free(db->dsn);
  free(db->username);
  free(db->password);
  free(db);
  database->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcDatabaseGetOption(struct AdbcDatabase* database, const char* key,
                                            char* value, size_t* length,
                                            struct AdbcError* error) {
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  const char* v = NULL;
  if (strcmp(key, ADBC_OPTION_URI) == 0) v = db->connection_string;
  else if (strcmp(key, ADBC_ODBC_OPTION_DSN) == 0) v = db->dsn;
  else if (strcmp(key, ADBC_OPTION_USERNAME) == 0) v = db->username;
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
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) { *value = db->reader_opts.batch_size; return ADBC_STATUS_OK; }
  if (strcmp(key, ADBC_ODBC_OPTION_MAX_BIND_BYTES) == 0) { *value = db->reader_opts.max_bind_bytes; return ADBC_STATUS_OK; }
  InternalAdbcSetError(error, "Unknown database option %s", key);
  return ADBC_STATUS_NOT_FOUND;
}

static AdbcStatusCode OdbcDatabaseSetOptionInt(struct AdbcDatabase* database, const char* key,
                                               int64_t value, struct AdbcError* error) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return OdbcDatabaseSetOption(database, key, buf, error);
}

// ---------------------------------------------------------------------------
// Connection

struct OdbcConnection {
  struct OdbcDatabase* db;
  SQLHDBC hdbc;
  bool connected;
  bool autocommit;
  struct OdbcReaderOptions reader_opts;
};

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

static AdbcStatusCode OdbcConnectionSetOption(struct AdbcConnection* connection, const char* key,
                                              const char* value, struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
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
  } else if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0) {
    if (!conn->connected) return ADBC_STATUS_INVALID_STATE;
    ODBC_CHECK(SQLSetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, (SQLPOINTER)value, SQL_NTS),
               SQL_HANDLE_DBC, conn->hdbc, "SQLSetConnectAttr(SQL_ATTR_CURRENT_CATALOG)", error);
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown connection option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionInit(struct AdbcConnection* connection,
                                         struct AdbcDatabase* database,
                                         struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  struct OdbcDatabase* db = (struct OdbcDatabase*)database->private_data;
  if (!conn || !db || !db->henv) {
    InternalAdbcSetError(error, "Database not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  conn->db = db;
  conn->reader_opts = db->reader_opts;

  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_DBC, db->henv, &conn->hdbc), SQL_HANDLE_ENV, db->henv,
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

  SQLRETURN ret = SQLDriverConnect(conn->hdbc, NULL, (SQLCHAR*)sb.buffer, SQL_NTS, NULL, 0, NULL,
                                   SQL_DRIVER_NOPROMPT);
  InternalAdbcStringBuilderReset(&sb);
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_DBC, conn->hdbc, "SQLDriverConnect", error);
    SQLFreeHandle(SQL_HANDLE_DBC, conn->hdbc);
    conn->hdbc = NULL;
    return s;
  }
  conn->connected = true;
  if (!conn->autocommit) RAISE_ADBC(OdbcConnectionSetAutocommit(conn, false, error));
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionRelease(struct AdbcConnection* connection,
                                            struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn) return ADBC_STATUS_INVALID_STATE;
  if (conn->hdbc) {
    if (conn->connected) SQLDisconnect(conn->hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, conn->hdbc);
  }
  free(conn);
  connection->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionCommit(struct AdbcConnection* connection,
                                           struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
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
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (conn->autocommit) {
    InternalAdbcSetError(error, "Cannot rollback when autocommit is enabled");
    return ADBC_STATUS_INVALID_STATE;
  }
  ODBC_CHECK(SQLEndTran(SQL_HANDLE_DBC, conn->hdbc, SQL_ROLLBACK), SQL_HANDLE_DBC, conn->hdbc,
             "SQLEndTran(SQL_ROLLBACK)", error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcConnectionGetInfo(struct AdbcConnection* connection,
                                            const uint32_t* info_codes, size_t info_codes_length,
                                            struct ArrowArrayStream* out,
                                            struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
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
        if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DBMS_NAME, buf, sizeof(buf), &len))) {
          RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], (const char*)buf, error));
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
      case ADBC_INFO_DRIVER_NAME: {
        // Include the underlying ODBC driver name for diagnostics.
        char name[1200];
        if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_DRIVER_NAME, buf, sizeof(buf), &len))) {
          snprintf(name, sizeof(name), ADBC_ODBC_DRIVER_NAME " (%s)", (const char*)buf);
        } else {
          snprintf(name, sizeof(name), ADBC_ODBC_DRIVER_NAME);
        }
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], name, error));
        break;
      }
      case ADBC_INFO_DRIVER_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], ADBC_ODBC_DRIVER_VERSION, error));
        break;
      case ADBC_INFO_DRIVER_ARROW_VERSION:
        RAISE_ADBC(InternalAdbcConnectionGetInfoAppendString(&array, info_codes[i], "nanoarrow " NANOARROW_VERSION, error));
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
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle(SQL_HANDLE_STMT)", error);
  SQLRETURN ret = SQLTables(hstmt, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0, (SQLCHAR*)"", 0,
                            (SQLCHAR*)SQL_ALL_TABLE_TYPES, SQL_NTS);
  if (!SQL_SUCCEEDED(ret)) {
    AdbcStatusCode s = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLTables(SQL_ALL_TABLE_TYPES)", error);
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    return s;
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
  SQLLEN ind;
  int64_t n = 0;
  while (SQL_SUCCEEDED(SQLFetch(hstmt))) {
    if (SQL_SUCCEEDED(SQLGetData(hstmt, 4, SQL_C_CHAR, buf, sizeof(buf), &ind)) && ind != SQL_NULL_DATA) {
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
  if (!conn || !conn->connected) return ADBC_STATUS_INVALID_STATE;
  if (!table_name) {
    InternalAdbcSetError(error, "table_name must not be NULL");
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  // Use the driver's identifier quote char to build SELECT * FROM ... WHERE 1=0.
  SQLCHAR q[8] = "\"";
  SQLSMALLINT qlen = 0;
  if (SQL_SUCCEEDED(SQLGetInfo(conn->hdbc, SQL_IDENTIFIER_QUOTE_CHAR, q, sizeof(q), &qlen))) {
    if (qlen == 0 || q[0] == ' ') q[0] = '\0';
  }
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
  (void)connection; (void)error;
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcConnectionGetOption(struct AdbcConnection* connection, const char* key,
                                              char* value, size_t* length,
                                              struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  const char* v = NULL;
  SQLCHAR buf[1024];
  SQLINTEGER outlen = 0;
  if (strcmp(key, ADBC_CONNECTION_OPTION_AUTOCOMMIT) == 0) {
    v = conn->autocommit ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED;
  } else if (strcmp(key, ADBC_CONNECTION_OPTION_CURRENT_CATALOG) == 0 && conn->connected) {
    ODBC_CHECK(SQLGetConnectAttr(conn->hdbc, SQL_ATTR_CURRENT_CATALOG, buf, sizeof(buf), &outlen),
               SQL_HANDLE_DBC, conn->hdbc, "SQLGetConnectAttr(SQL_ATTR_CURRENT_CATALOG)", error);
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

// ---------------------------------------------------------------------------
// Statement

struct OdbcStatement {
  struct OdbcConnection* conn;
  struct OdbcHandleRef* ref;
  char* query;
  bool prepared;
  struct OdbcReaderOptions reader_opts;
};

static AdbcStatusCode OdbcStatementNew(struct AdbcConnection* connection,
                                       struct AdbcStatement* statement, struct AdbcError* error) {
  struct OdbcConnection* conn = (struct OdbcConnection*)connection->private_data;
  if (!conn || !conn->connected) {
    InternalAdbcSetError(error, "Connection not initialized");
    return ADBC_STATUS_INVALID_STATE;
  }
  struct OdbcStatement* stmt = calloc(1, sizeof(struct OdbcStatement));
  if (!stmt) {
    InternalAdbcSetError(error, "out of memory");
    return ADBC_STATUS_INTERNAL;
  }
  stmt->conn = conn;
  stmt->reader_opts = conn->reader_opts;
  statement->private_data = stmt;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementRelease(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  OdbcHandleRefRelease(stmt->ref);
  free(stmt->query);
  free(stmt);
  statement->private_data = NULL;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementSetSqlQuery(struct AdbcStatement* statement, const char* query,
                                               struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  free(stmt->query);
  stmt->query = strdup(query);
  stmt->prepared = false;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementSetOption(struct AdbcStatement* statement, const char* key,
                                             const char* value, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) {
    long v = strtol(value, NULL, 10);
    if (v <= 0) return ADBC_STATUS_INVALID_ARGUMENT;
    stmt->reader_opts.batch_size = v;
    return ADBC_STATUS_OK;
  }
  InternalAdbcSetError(error, "Unknown statement option %s", key);
  return ADBC_STATUS_NOT_IMPLEMENTED;
}

static AdbcStatusCode OdbcStatementSetOptionInt(struct AdbcStatement* statement, const char* key,
                                                int64_t value, struct AdbcError* error) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return OdbcStatementSetOption(statement, key, buf, error);
}

// Ensure we own a fresh, idle statement handle.
static AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt,
                                                struct AdbcError* error) {
  if (stmt->ref && stmt->ref->refcount > 1) {
    // A previous result stream still owns this handle; detach and allocate a new one.
    OdbcHandleRefRelease(stmt->ref);
    stmt->ref = NULL;
    stmt->prepared = false;
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
  } else {
    SQLCloseCursor(stmt->ref->hstmt);  // ignore "no cursor" errors
  }
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementPrepare(struct AdbcStatement* statement,
                                           struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  ODBC_CHECK(SQLPrepare(stmt->ref->hstmt, (SQLCHAR*)stmt->query, SQL_NTS), SQL_HANDLE_STMT,
             stmt->ref->hstmt, "SQLPrepare", error);
  stmt->prepared = true;
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementExecuteQuery(struct AdbcStatement* statement,
                                                struct ArrowArrayStream* out,
                                                int64_t* rows_affected,
                                                struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt) return ADBC_STATUS_INVALID_STATE;
  if (!stmt->query) {
    InternalAdbcSetError(error, "Must call StatementSetSqlQuery first");
    return ADBC_STATUS_INVALID_STATE;
  }
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
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
    SQLLEN count = -1;
    SQLRowCount(hstmt, &count);
    if (rows_affected) *rows_affected = (int64_t)count;
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
    SQLLEN count = -1;
    SQLRowCount(hstmt, &count);
    if (rows_affected) *rows_affected = (int64_t)count;
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
  if (!stmt->prepared) RAISE_ADBC(OdbcStatementPrepare(statement, error));
  return OdbcDescribeResultSchema(stmt->ref->hstmt, &stmt->reader_opts, schema, error);
}

static AdbcStatusCode OdbcStatementCancel(struct AdbcStatement* statement,
                                          struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (!stmt || !stmt->ref) return ADBC_STATUS_INVALID_STATE;
  ODBC_CHECK(SQLCancel(stmt->ref->hstmt), SQL_HANDLE_STMT, stmt->ref->hstmt, "SQLCancel", error);
  return ADBC_STATUS_OK;
}

static AdbcStatusCode OdbcStatementGetOptionInt(struct AdbcStatement* statement, const char* key,
                                                int64_t* value, struct AdbcError* error) {
  struct OdbcStatement* stmt = (struct OdbcStatement*)statement->private_data;
  if (strcmp(key, ADBC_ODBC_OPTION_BATCH_SIZE) == 0) { *value = stmt->reader_opts.batch_size; return ADBC_STATUS_OK; }
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

  driver->StatementExecuteQuery = OdbcStatementExecuteQuery;
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
    driver->ConnectionCancel = OdbcConnectionCancel;
    driver->ConnectionGetOption = OdbcConnectionGetOption;
    driver->StatementCancel = OdbcStatementCancel;
    driver->StatementExecuteSchema = OdbcStatementExecuteSchema;
    driver->StatementGetOptionInt = OdbcStatementGetOptionInt;
    driver->StatementSetOptionInt = OdbcStatementSetOptionInt;
  }
  return ADBC_STATUS_OK;
}

ADBC_ODBC_EXPORT
AdbcStatusCode AdbcDriverInit(int version, void* driver, struct AdbcError* error) {
  return AdbcDriverOdbcInit(version, driver, error);
}
