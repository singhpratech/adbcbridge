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

// Native delegation: when a native ADBC driver exists for the target database,
// let it serve the connection and use ODBC only as the fallback.
//
// The decision happens in AdbcDatabaseInit: adbcbridge works out which native
// driver (if any) fits the target, loads it through the ADBC driver manager's
// loader, and initializes a native database with the translated options.  From
// then on adbcbridge keeps its own function table (the driver manager may share
// one table between every database, so overwriting it in place is not safe) and
// forwards every database/connection/statement call to the native driver -- one
// indirection per ADBC call, nothing per row: result sets are the native
// driver's own ArrowArrayStream, handed straight to the caller.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <arrow-adbc/adbc.h>

/// "auto" (default) | "never" | "always"
#define ADBC_ODBC_OPTION_DELEGATE "adbc.odbc.delegate"
/// Force a specific native driver (a bare name, or a path/manifest when paths are allowed).
#define ADBC_ODBC_OPTION_DELEGATE_DRIVER "adbc.odbc.delegate.driver"
/// Extra directories to search for native drivers (path-list separated).
#define ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH "adbc.odbc.delegate.search_path"
/// Allow ".driver"/".search_path" to name filesystem paths ("true"/"false").
#define ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS "adbc.odbc.delegate.allow_paths"
/// Read-only: why delegation did not happen (empty if it did, or was never tried).
#define ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR "adbc.odbc.delegate.last_error"
/// Read-only: the native driver that took over, or "odbc" ("" before init).
#define ADBC_ODBC_OPTION_DELEGATED_TO "adbc.odbc.delegated_to"

/// Environment fallback for ADBC_ODBC_OPTION_DELEGATE (auto/never/always).
#define ADBC_ODBC_DELEGATE_ENV "ADBC_ODBC_DELEGATE"
/// Environment fallback for ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH.
#define ADBC_ODBC_DELEGATE_PATH_ENV "ADBC_ODBC_DELEGATE_PATH"
/// Environment fallback for ADBC_ODBC_OPTION_DELEGATE_ALLOW_PATHS ("1").
#define ADBC_ODBC_DELEGATE_ALLOW_PATHS_ENV "ADBC_ODBC_DELEGATE_ALLOW_PATHS"

/// Value reported by ADBC_ODBC_OPTION_DELEGATED_TO when ODBC is in use.
#define ADBC_ODBC_DELEGATED_TO_ODBC "odbc"

enum OdbcDelegateMode {
  ODBC_DELEGATE_AUTO = 0,
  ODBC_DELEGATE_NEVER,
  ODBC_DELEGATE_ALWAYS,
};

/// Delegation state carried by an AdbcDatabase.
struct OdbcDelegateOptions {
  enum OdbcDelegateMode mode;
  char* driver;        // adbc.odbc.delegate.driver
  char* search_path;   // adbc.odbc.delegate.search_path
  char* last_error;    // why we fell back to ODBC
  char* delegated_to;  // NULL before init, then "odbc" or the native driver name
  bool allow_paths;    // adbc.odbc.delegate.allow_paths / ADBC_ODBC_DELEGATE_ALLOW_PATHS
  bool initialized;    // AdbcDatabaseInit has run: options are frozen
  // Options forwarded verbatim to the native driver ("adbc.*", except "adbc.odbc.*").
  char** pass_keys;
  char** pass_values;
  size_t pass_count;
};

/// What we know about the target database from the ODBC-flavored options.
struct OdbcDelegateTarget {
  const char* connection_string;  // "uri" / "adbc.odbc.connection_string"
  const char* dsn;
  const char* username;
  const char* password;
};

/// A native driver serving one AdbcDatabase, and the objects hanging off it.
struct OdbcDelegateProxy;
struct OdbcProxyConnection;
struct OdbcProxyStatement;

/// Set the defaults, including the ADBC_ODBC_DELEGATE environment override.
void OdbcDelegateOptionsInit(struct OdbcDelegateOptions* opts);

void OdbcDelegateOptionsRelease(struct OdbcDelegateOptions* opts);

/// Handle a database option.  Returns true if the key belongs to delegation (in
/// which case *status holds the result), false if the caller should handle it.
bool OdbcDelegateSetOption(struct OdbcDelegateOptions* opts, const char* key, const char* value,
                           AdbcStatusCode* status, struct AdbcError* error);

/// Read back a delegation option.  Returns true if the key was recognized.
bool OdbcDelegateGetOption(const struct OdbcDelegateOptions* opts, const char* key,
                           const char** out);

/// The first option held for the native driver, or NULL.  Held options are only
/// meaningful when delegation happens; if it does not, AdbcDatabaseInit reports
/// them as unknown rather than dropping them.
const char* OdbcDelegateHeldOption(const struct OdbcDelegateOptions* opts);

/// Try to hand the database over to a native ADBC driver.
///
/// \param[in] database The database being initialized (only read, never modified).
/// \param[in] self_database_init adbcbridge's own AdbcDatabaseInit, used to
///   recognize (and refuse) delegation to adbcbridge itself.
/// \param[in] target The ODBC options describing the database.
/// \param[in,out] opts Delegation options; last_error/delegated_to are updated.
/// \param[out] proxy Set to the native driver's state when delegation happened.
/// \return ADBC_STATUS_OK when the caller should carry on with ODBC (*proxy ==
///   NULL) or when delegation succeeded; an error when mode is "always" and no
///   native driver could be used, or when the native driver failed on a target
///   that ODBC cannot serve either.
AdbcStatusCode OdbcDelegateTryInit(struct AdbcDatabase* database,
                                   AdbcStatusCode (*self_database_init)(struct AdbcDatabase*,
                                                                        struct AdbcError*),
                                   const struct OdbcDelegateTarget* target,
                                   struct OdbcDelegateOptions* opts,
                                   struct OdbcDelegateProxy** proxy, struct AdbcError* error);

/// Release the native database and unload the native driver.
void OdbcDelegateProxyRelease(struct OdbcDelegateProxy* proxy);

/// Does this string look like a native ADBC URI ("postgresql://...", "sqlite:...")?
bool OdbcDelegateIsNativeUri(const char* value);

/// Translate a native ADBC URI into an ODBC connection string, for the fallback
/// path (ODBC cannot consume "postgresql://..." itself).  Returns an error with
/// a diagnostic when no installed ODBC driver can serve the URI.
AdbcStatusCode OdbcDelegateNativeUriToOdbc(const char* uri, const char* last_error, char** out,
                                           struct AdbcError* error);

// ---------------------------------------------------------------------------
// Forwarding to the native driver.  Every entry point of the ADBC 1.1.0 surface
// is here; adbcbridge's own entry points call these when a proxy is in place.

AdbcStatusCode OdbcProxyDatabaseSetOption(struct OdbcDelegateProxy* proxy, const char* key,
                                          const char* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseSetOptionInt(struct OdbcDelegateProxy* proxy, const char* key,
                                             int64_t value, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseSetOptionDouble(struct OdbcDelegateProxy* proxy, const char* key,
                                                double value, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseSetOptionBytes(struct OdbcDelegateProxy* proxy, const char* key,
                                               const uint8_t* value, size_t length,
                                               struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseGetOption(struct OdbcDelegateProxy* proxy, const char* key,
                                          char* value, size_t* length, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseGetOptionInt(struct OdbcDelegateProxy* proxy, const char* key,
                                             int64_t* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseGetOptionDouble(struct OdbcDelegateProxy* proxy, const char* key,
                                                double* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyDatabaseGetOptionBytes(struct OdbcDelegateProxy* proxy, const char* key,
                                               uint8_t* value, size_t* length,
                                               struct AdbcError* error);

/// One option set on a connection before AdbcConnectionInit, kept until it is
/// known whether the connection is served by ODBC or by a native driver.
struct OdbcPreOption {
  char* key;
  enum OdbcPreOptionType {
    ODBC_PRE_OPTION_STRING,
    ODBC_PRE_OPTION_INT,
    ODBC_PRE_OPTION_DOUBLE,
    ODBC_PRE_OPTION_BYTES,
  } type;
  char* value;     ///< ODBC_PRE_OPTION_STRING (may be NULL)
  uint8_t* bytes;  ///< ODBC_PRE_OPTION_BYTES
  size_t length;   ///< ODBC_PRE_OPTION_BYTES
  int64_t number;  ///< ODBC_PRE_OPTION_INT
  double real;     ///< ODBC_PRE_OPTION_DOUBLE
};

/// Create and initialize the native connection.  `pre` holds the options set on
/// the connection before AdbcConnectionInit, replayed here in the order they
/// were set.
AdbcStatusCode OdbcProxyConnectionInit(struct OdbcDelegateProxy* proxy,
                                       const struct OdbcPreOption* pre, size_t pre_count,
                                       struct OdbcProxyConnection** out,
                                       struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionRelease(struct OdbcProxyConnection* conn,
                                          struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionCommit(struct OdbcProxyConnection* conn,
                                         struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionRollback(struct OdbcProxyConnection* conn,
                                           struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionCancel(struct OdbcProxyConnection* conn,
                                         struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetInfo(struct OdbcProxyConnection* conn,
                                          const uint32_t* info_codes, size_t info_codes_length,
                                          struct ArrowArrayStream* out, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetObjects(struct OdbcProxyConnection* conn, int depth,
                                             const char* catalog, const char* db_schema,
                                             const char* table_name, const char** table_type,
                                             const char* column_name,
                                             struct ArrowArrayStream* out,
                                             struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetTableSchema(struct OdbcProxyConnection* conn,
                                                 const char* catalog, const char* db_schema,
                                                 const char* table_name,
                                                 struct ArrowSchema* schema,
                                                 struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetTableTypes(struct OdbcProxyConnection* conn,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetStatistics(struct OdbcProxyConnection* conn,
                                                const char* catalog, const char* db_schema,
                                                const char* table_name, char approximate,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetStatisticNames(struct OdbcProxyConnection* conn,
                                                    struct ArrowArrayStream* out,
                                                    struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionReadPartition(struct OdbcProxyConnection* conn,
                                                const uint8_t* serialized_partition,
                                                size_t serialized_length,
                                                struct ArrowArrayStream* out,
                                                struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionSetOption(struct OdbcProxyConnection* conn, const char* key,
                                            const char* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionSetOptionInt(struct OdbcProxyConnection* conn, const char* key,
                                               int64_t value, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionSetOptionDouble(struct OdbcProxyConnection* conn,
                                                  const char* key, double value,
                                                  struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionSetOptionBytes(struct OdbcProxyConnection* conn,
                                                 const char* key, const uint8_t* value,
                                                 size_t length, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetOption(struct OdbcProxyConnection* conn, const char* key,
                                            char* value, size_t* length, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetOptionInt(struct OdbcProxyConnection* conn, const char* key,
                                               int64_t* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetOptionDouble(struct OdbcProxyConnection* conn,
                                                  const char* key, double* value,
                                                  struct AdbcError* error);
AdbcStatusCode OdbcProxyConnectionGetOptionBytes(struct OdbcProxyConnection* conn,
                                                 const char* key, uint8_t* value, size_t* length,
                                                 struct AdbcError* error);
/// The native driver behind a connection, for adbc.odbc.delegated_to.
const char* OdbcProxyConnectionName(const struct OdbcProxyConnection* conn);

AdbcStatusCode OdbcProxyStatementNew(struct OdbcProxyConnection* conn,
                                     struct OdbcProxyStatement** out, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementRelease(struct OdbcProxyStatement* stmt,
                                         struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementBind(struct OdbcProxyStatement* stmt, struct ArrowArray* values,
                                      struct ArrowSchema* schema, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementBindStream(struct OdbcProxyStatement* stmt,
                                            struct ArrowArrayStream* stream,
                                            struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementExecuteQuery(struct OdbcProxyStatement* stmt,
                                              struct ArrowArrayStream* out, int64_t* rows_affected,
                                              struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementExecutePartitions(struct OdbcProxyStatement* stmt,
                                                   struct ArrowSchema* schema,
                                                   struct AdbcPartitions* partitions,
                                                   int64_t* rows_affected,
                                                   struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementExecuteSchema(struct OdbcProxyStatement* stmt,
                                               struct ArrowSchema* schema,
                                               struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementGetParameterSchema(struct OdbcProxyStatement* stmt,
                                                    struct ArrowSchema* schema,
                                                    struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementPrepare(struct OdbcProxyStatement* stmt,
                                         struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementCancel(struct OdbcProxyStatement* stmt, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetSqlQuery(struct OdbcProxyStatement* stmt, const char* query,
                                             struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetSubstraitPlan(struct OdbcProxyStatement* stmt,
                                                  const uint8_t* plan, size_t length,
                                                  struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetOption(struct OdbcProxyStatement* stmt, const char* key,
                                           const char* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetOptionInt(struct OdbcProxyStatement* stmt, const char* key,
                                              int64_t value, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetOptionDouble(struct OdbcProxyStatement* stmt, const char* key,
                                                 double value, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementSetOptionBytes(struct OdbcProxyStatement* stmt, const char* key,
                                                const uint8_t* value, size_t length,
                                                struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementGetOption(struct OdbcProxyStatement* stmt, const char* key,
                                           char* value, size_t* length, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementGetOptionInt(struct OdbcProxyStatement* stmt, const char* key,
                                              int64_t* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementGetOptionDouble(struct OdbcProxyStatement* stmt, const char* key,
                                                 double* value, struct AdbcError* error);
AdbcStatusCode OdbcProxyStatementGetOptionBytes(struct OdbcProxyStatement* stmt, const char* key,
                                                uint8_t* value, size_t* length,
                                                struct AdbcError* error);
