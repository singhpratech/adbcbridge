<!--
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
-->

# Rust smoke test

Proves that `libadbc_driver_odbc.so` loads and works through the *Rust* ADBC
driver manager, not just the Python one — the driver is a plain C shared
library, so every ADBC binding reaches it the same way.

The test crate is standalone (its own `Cargo.toml`, not a member of any
workspace) and depends only on published crates:

| crate | why |
|---|---|
| [`adbc_core`](https://crates.io/crates/adbc_core) 0.24 | the `Driver` / `Database` / `Connection` / `Statement` traits |
| [`adbc_driver_manager`](https://crates.io/crates/adbc_driver_manager) 0.24 | `ManagedDriver`, which `dlopen`s the `.so` and calls `AdbcDriverInit` |
| [`arrow-array`](https://crates.io/crates/arrow-array) / [`arrow-schema`](https://crates.io/crates/arrow-schema) 59 | the `RecordBatch`es the tests assert on |
| [`tempfile`](https://crates.io/crates/tempfile) 3 | a throwaway SQLite database file per test |

## Running

Build the driver first, then run `cargo test` from this directory:

```sh
cd ../..                                       # repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
cd tests/rust
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so cargo test
```

Expected output:

```
running 3 tests
test error_carries_a_message ... ok
test select_one ... ok
test parameterised_insert_and_select ... ok

test result: ok. 3 passed; 0 failed; ...
```

## Environment

| variable | default | meaning |
|---|---|---|
| `SQLITE_ODBC_DRIVER` | `SQLite3` | the SQLite ODBC driver to bridge to. Either an absolute path to `libsqlite3odbc.so` or a driver name registered in `odbcinst.ini`. It is passed through verbatim as `Driver=...` in the ODBC connection string. |
| `ADBC_ODBC_DRIVER` | `../../build/libadbc_driver_odbc.so` | the driver under test. Set it to test an installed copy instead of the one in `build/`. |

No DSN, no `odbc.ini` entry and no server are needed: each test creates its own
SQLite database file under a temporary directory and connects with a full
connection string.

## What the tests cover

`tests/smoke.rs`:

- **`select_one`** — `SELECT 1 AS one` returns a single batch with one `Int32`
  column named `one` holding the value `1`. (SQLite's ODBC driver describes an
  integer literal as `SQL_INTEGER`, which the bridge maps to Arrow `int32`.)
- **`parameterised_insert_and_select`** — prepares
  `INSERT INTO people (id, name) VALUES (?, ?)`, binds one three-row
  `RecordBatch` of parameters (including a NULL name), and checks that
  `execute_update` reports 3 rows. Then reads the table back and asserts on the
  values and the preserved NULL, and finally binds a single-row batch to
  `SELECT name FROM people WHERE id = ?` and asserts only the matching row comes
  back.
- **`error_carries_a_message`** — querying a missing table fails with an error
  whose message names the table, i.e. the ODBC diagnostic record reaches the
  caller rather than being swallowed.

`examples/readme_snippet.rs` is the "Use from Rust" snippet from the top-level
`README.md`. It is never run — the paths in it are placeholders — but `cargo
test` compiles it, so the snippet cannot silently rot.

## Notes

- The driver is loaded with `ManagedDriver::load_dynamic_from_filename`, passing
  the entrypoint `AdbcDriverInit` explicitly. `libadbc_driver_odbc.so` also
  exports `AdbcDriverOdbcInit`, which is the name the driver manager derives
  from the file name when no entrypoint is given — either works.
- `AdbcVersion::V110` is requested; the driver implements both the 1.0.0 and
  1.1.0 ABI.
- `target/` is gitignored; `Cargo.lock` is committed so the test builds against
  a known-good dependency set.
