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

// The other half of the fixture for tests/test_driver_load_errors.py: a library
// that reaches its dependency's thread-local through the initial-exec model,
// the way MySQL Connector/ODBC reaches libstdc++'s.  Loading it demands an
// offset in the static TLS block for libadbc_odbc_tls_dep, so it fails with
// "cannot allocate memory in static TLS block" once AdbcOdbcTlsTouch() has been
// called.  It is not an ODBC driver and defines no ODBC entry points: the test
// only ever gets as far as the driver manager failing to load it.

extern __thread int adbc_odbc_tls_value __attribute__((tls_model("initial-exec")));

int AdbcOdbcTlsRead(void) {
  return adbc_odbc_tls_value;
}
