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

// Half of the fixture for tests/test_driver_load_errors.py.  This library
// stands in for libstdc++: it has thread-local state, and it is loaded with
// dlopen() rather than being on the executable's NEEDED list.
//
// Calling AdbcOdbcTlsTouch() reads that thread-local through the dynamic TLS
// model, which is the point of no return: glibc then records the library as
// dynamic-TLS-only for the life of the process, and can no longer give it an
// offset in the static TLS block however much surplus is available.  That is
// exactly what importing pyarrow does to libstdc++.

__thread int adbc_odbc_tls_value;

int AdbcOdbcTlsTouch(void) {
  adbc_odbc_tls_value += 1;
  return adbc_odbc_tls_value;
}
