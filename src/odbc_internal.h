// SPDX-License-Identifier: Apache-2.0
// adbc-odbc: an ADBC driver that bridges any ODBC driver to Apache Arrow.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"
#include "utils.h"

#if defined(_WIN32)
#define ADBC_ODBC_EXPORT __declspec(dllexport)
#else
#define ADBC_ODBC_EXPORT __attribute__((visibility("default")))
#endif

#define ADBC_ODBC_DRIVER_NAME "ADBC ODBC Driver"
#define ADBC_ODBC_DRIVER_VERSION "0.1.0"

// Driver-specific options
#define ADBC_ODBC_OPTION_DSN "dsn"
#define ADBC_ODBC_OPTION_CONNECTION_STRING "adbc.odbc.connection_string"
#define ADBC_ODBC_OPTION_BATCH_SIZE "adbc.odbc.batch_size"
#define ADBC_ODBC_OPTION_MAX_BIND_BYTES "adbc.odbc.max_bind_bytes"
#define ADBC_ODBC_OPTION_DECIMAL_AS_STRING "adbc.odbc.decimal_as_string"

#define ADBC_ODBC_DEFAULT_BATCH_SIZE 1024
#define ADBC_ODBC_DEFAULT_MAX_BIND_BYTES 32768

/// A refcounted ODBC statement handle shared between an AdbcStatement and
/// the ArrowArrayStream it produced.
struct OdbcHandleRef {
  SQLHSTMT hstmt;
  int refcount;
};

struct OdbcHandleRef* OdbcHandleRefNew(SQLHSTMT hstmt);
void OdbcHandleRefRelease(struct OdbcHandleRef* ref);

/// Populate an AdbcError from ODBC diagnostics and return a mapped status code.
AdbcStatusCode OdbcSetError(SQLSMALLINT handle_type, SQLHANDLE handle, const char* context,
                            struct AdbcError* error);

/// Check an ODBC return code; on failure set error and return.
#define ODBC_CHECK(RET, HTYPE, HANDLE, CONTEXT, ERROR)                    \
  do {                                                                    \
    SQLRETURN odbc_ret_ = (RET);                                          \
    if (!SQL_SUCCEEDED(odbc_ret_)) {                                      \
      return OdbcSetError((HTYPE), (HANDLE), (CONTEXT), (ERROR));         \
    }                                                                     \
  } while (0)

struct OdbcReaderOptions {
  int64_t batch_size;
  int64_t max_bind_bytes;
  bool decimal_as_string;
  // Driver quirk: some drivers (DuckDB) write a whole internal chunk into bound
  // buffers regardless of SQL_ATTR_ROW_ARRAY_SIZE; allocate at least this many rows.
  int64_t min_buffer_rows;
};

struct OdbcDatabase;

struct OdbcConnection {
  struct OdbcDatabase* db;
  SQLHDBC hdbc;
  bool connected;
  bool autocommit;
  struct OdbcReaderOptions reader_opts;
};

struct OdbcStatement {
  struct OdbcConnection* conn;
  struct OdbcHandleRef* ref;
  char* query;
  bool prepared;
  struct OdbcReaderOptions reader_opts;
  // Bound parameters (Bind / BindStream)
  struct ArrowArrayStream bind_stream;
  bool has_bind;
  // Bulk ingest
  char* ingest_table;
  char* ingest_catalog;
  char* ingest_schema;
  char* ingest_mode;  // ADBC_INGEST_OPTION_MODE_* value
  bool ingest_temporary;
};

/// Fetch the identifier quote character ('"' default, '\0' if none) into out[8].
void OdbcQuoteChar(SQLHDBC hdbc, char* out);

AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error);

/// Execute stmt->query once per bound row (parameters from bind_stream).
AdbcStatusCode OdbcStatementExecuteBound(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                         int64_t* rows_affected, struct AdbcError* error);

/// Bulk ingest bind_stream into stmt->ingest_table.
AdbcStatusCode OdbcStatementIngest(struct OdbcStatement* stmt, int64_t* rows_affected,
                                   struct AdbcError* error);

AdbcStatusCode OdbcConnectionGetObjects(struct AdbcConnection* connection, int depth,
                                        const char* catalog, const char* db_schema,
                                        const char* table_name, const char** table_type,
                                        const char* column_name, struct ArrowArrayStream* out,
                                        struct AdbcError* error);

/// Describe the result set of an executed/prepared statement as an Arrow schema.
AdbcStatusCode OdbcDescribeResultSchema(SQLHSTMT hstmt, const struct OdbcReaderOptions* opts,
                                        struct ArrowSchema* out, struct AdbcError* error);

/// Build an ArrowArrayStream that fetches from an executed statement.
/// Takes a reference on `ref`.
AdbcStatusCode OdbcReaderInit(struct OdbcHandleRef* ref, const struct OdbcReaderOptions* opts,
                              struct ArrowArrayStream* out, struct AdbcError* error);
