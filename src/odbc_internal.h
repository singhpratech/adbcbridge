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

// adbcbridge: an ADBC driver that bridges any ODBC driver to Apache Arrow.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"
#include "utils.h"

// --- SQL type codes not present in every driver-manager header ---------------
// Microsoft SQL Server extended types (msodbcsql.h).
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2 (-154)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET (-155)
#endif
// ODBC 4.0 datetime-with-timezone types.
#ifndef SQL_TYPE_TIME_WITH_TIMEZONE
#define SQL_TYPE_TIME_WITH_TIMEZONE 94
#endif
#ifndef SQL_TYPE_TIMESTAMP_WITH_TIMEZONE
#define SQL_TYPE_TIMESTAMP_WITH_TIMEZONE 95
#endif

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
/// Bind Arrow batches as ODBC parameter arrays (one execute per batch) when the
/// driver supports it.  "true"/"false"; default true.
#define ADBC_ODBC_OPTION_ARRAY_BINDING "adbc.odbc.array_binding"

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
  // Driver quirk: bind boolean parameters as integers (DuckDB rejects SQL_BIT params).
  bool bool_param_as_int;
  // Driver quirk: no SQL_C_SBIGINT parameter support (Oracle); send 64-bit ints as numeric text.
  bool bigint_param_as_string;
  // Driver quirk: bind decimal parameters as VARCHAR text instead of SQL_DECIMAL (DuckDB).
  bool decimal_param_as_varchar;
  // Driver quirk: bind NULL parameters as a NULL VARCHAR regardless of Arrow type
  // (clickhouse-odbc cannot encode typed NULLs for numeric parameters).
  bool null_param_as_varchar;
  // Driver quirk: DDL type wrapper for nullable columns, e.g. "Nullable(%s)" (ClickHouse).
  const char* nullable_type_format;
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
  /// Try column-wise parameter arrays before falling back to row-at-a-time.
  bool array_binding;
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
