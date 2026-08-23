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
// hand the whole AdbcDriver over to it and use ODBC only as the fallback.
//
// The hand-over happens in AdbcDatabaseInit: adbcbridge works out which native
// driver (if any) fits the target, loads it through the ADBC driver manager's
// own resolution, initializes a database with the translated options and -- if
// all of that worked -- overwrites the driver function table that the driver
// manager dispatches through.  Every later connection/statement call therefore
// goes straight to the native driver; adbcbridge is out of the data path.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <arrow-adbc/adbc.h>

/// "auto" (default) | "never" | "always"
#define ADBC_ODBC_OPTION_DELEGATE "adbc.odbc.delegate"
/// Force a specific native driver (name, manifest, or path to a shared library).
#define ADBC_ODBC_OPTION_DELEGATE_DRIVER "adbc.odbc.delegate.driver"
/// Extra directories to search for native drivers (path-list separated).
#define ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH "adbc.odbc.delegate.search_path"
/// Read-only: why delegation did not happen (empty if it did, or was never tried).
#define ADBC_ODBC_OPTION_DELEGATE_LAST_ERROR "adbc.odbc.delegate.last_error"
/// Read-only: the native driver that took over, or "odbc".
#define ADBC_ODBC_OPTION_DELEGATED_TO "adbc.odbc.delegated_to"

/// Environment fallback for ADBC_ODBC_OPTION_DELEGATE (auto/never/always).
#define ADBC_ODBC_DELEGATE_ENV "ADBC_ODBC_DELEGATE"
/// Environment fallback for ADBC_ODBC_OPTION_DELEGATE_SEARCH_PATH.
#define ADBC_ODBC_DELEGATE_PATH_ENV "ADBC_ODBC_DELEGATE_PATH"

/// Value reported by ADBC_ODBC_OPTION_DELEGATED_TO when ODBC is in use.
#define ADBC_ODBC_DELEGATED_TO_ODBC "odbc"

enum OdbcDelegateMode {
  ODBC_DELEGATE_AUTO = 0,
  ODBC_DELEGATE_NEVER,
  ODBC_DELEGATE_ALWAYS,
};

/// Delegation state carried by an AdbcDatabase before it is initialized.
struct OdbcDelegateOptions {
  enum OdbcDelegateMode mode;
  char* driver;       // adbc.odbc.delegate.driver
  char* search_path;  // adbc.odbc.delegate.search_path
  char* last_error;   // why we fell back to ODBC
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

/// Try to hand the database over to a native ADBC driver.
///
/// \param[in] database The database being initialized.  Delegation is only
///   possible when it is driven by the ADBC driver manager, which is detected by
///   comparing database->private_driver->DatabaseInit against self_database_init.
/// \param[in] self_database_init adbcbridge's own AdbcDatabaseInit.
/// \param[in] target The ODBC options describing the database.
/// \param[in,out] opts Delegation options; opts->last_error is updated.
/// \param[out] delegated Set to true if the native driver has taken over, in
///   which case the caller must free its own private_data (the native driver
///   owns database->private_data now) and return the status code.
/// \return ADBC_STATUS_OK when the caller should carry on with ODBC
///   (*delegated == false) or when delegation succeeded (*delegated == true);
///   an error only when mode is "always" and no native driver could be used.
AdbcStatusCode OdbcDelegateTryInit(struct AdbcDatabase* database,
                                   AdbcStatusCode (*self_database_init)(struct AdbcDatabase*,
                                                                        struct AdbcError*),
                                   const struct OdbcDelegateTarget* target,
                                   struct OdbcDelegateOptions* opts, bool* delegated,
                                   struct AdbcError* error);
