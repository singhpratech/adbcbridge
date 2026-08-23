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

# C# smoke test

Proves that `libadbc_driver_odbc.so` loads and works through the *C#* ADBC
bindings, not just the Python and Rust ones — the driver is a plain C shared
library, so every ADBC binding reaches it the same way.

The project is standalone (its own `.csproj`, not part of any solution) and
depends only on published NuGet packages:

| package | why |
|---|---|
| [`Apache.Arrow.Adbc`](https://www.nuget.org/packages/Apache.Arrow.Adbc) 0.24.0 | `CAdbcDriverImporter.Load`, which `dlopen`s the `.so` and calls `AdbcDriverInit`, plus the `AdbcDriver` / `AdbcDatabase` / `AdbcConnection` / `AdbcStatement` types |
| [`Apache.Arrow`](https://www.nuget.org/packages/Apache.Arrow) | pulled in transitively — the `RecordBatch`es the tests assert on |
| `xunit` 2.9.2 + `Microsoft.NET.Test.Sdk` 17.11.1 | the test runner |

## Running

Build the driver first, then run the tests in a container. The image needs a
`unixodbc` driver manager (the bridge links `libodbc.so.2`) and `libsqlite3-0`
(the SQLite ODBC driver links it):

```sh
cd /path/to/adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j

docker run --rm \
  -v "$PWD":/repo:ro \
  -v /usr/lib/x86_64-linux-gnu/odbc:/odbc:ro \
  -e ADBC_ODBC_DRIVER=/repo/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so \
  mcr.microsoft.com/dotnet/sdk:8.0 \
  sh -c 'apt-get update -qq && apt-get install -y -qq unixodbc libsqlite3-0 \
         && cp -r /repo/tests/csharp /work && cd /work && dotnet test --nologo'
```

Expected output:

```
  Determining projects to restore...
  Restored /work/AdbcBridge.SmokeTests.csproj (in 2.78 sec).
  AdbcBridge.SmokeTests -> /work/bin/Debug/net8.0/AdbcBridge.SmokeTests.dll
Test run for /work/bin/Debug/net8.0/AdbcBridge.SmokeTests.dll (.NETCoreApp,Version=v8.0)
A total of 1 test files matched the specified pattern.

Passed!  - Failed:     0, Passed:     5, Skipped:     0, Total:     5, Duration: 114 ms
```

Two details of that command are deliberate:

- **the repo is mounted read-only** and the project is copied to `/work` first,
  so `dotnet` writes `bin/` and `obj/` inside the container instead of leaving
  root-owned build output in your checkout. If you would rather build in place,
  drop the `:ro` and the `cp`, and run `dotnet test` in `/repo/tests/csharp`.
- **the host driver `.so` is used as-is**, not rebuilt in the container. That
  works as long as the host glibc is no newer than the image's — the bridge
  itself only needs `GLIBC_2.14`, but `libsqlite3odbc.so` from a recent Ubuntu
  needs `GLIBC_2.34`, and `mcr.microsoft.com/dotnet/sdk:8.0` (Debian 12) has
  2.36. If your ODBC driver needs something newer, switch to an image built on
  a newer base (`mcr.microsoft.com/dotnet/sdk:8.0-noble` is Ubuntu 24.04), or
  add `cmake build-essential unixodbc-dev` to the `apt-get install` and build
  the bridge inside the container.

Nothing stops you running it outside docker either — with the .NET 8 SDK and
`unixodbc` installed, `dotnet test` in this directory does the same thing:

```sh
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so dotnet test
```

## Environment

| variable | default | meaning |
|---|---|---|
| `SQLITE_ODBC_DRIVER` | `SQLite3` | the SQLite ODBC driver to bridge to. Either an absolute path to `libsqlite3odbc.so` or a driver name registered in `odbcinst.ini`. It is passed through verbatim as `Driver=...` in the ODBC connection string. |
| `ADBC_ODBC_DRIVER` | `../../build/libadbc_driver_odbc.so` relative to the project directory | the driver under test. Set it to test an installed copy instead of the one in `build/`. Required when the project is copied out of the repo, as the docker command above does. |

No DSN, no `odbc.ini` entry and no server are needed: each test creates its own
SQLite database file under a temporary directory and connects with a full
connection string.

## What the tests cover

`SmokeTests.cs`:

- **`LoadsTheDriverAndNegotiatesAdbc110`** — `CAdbcDriverImporter.Load` finds
  the `AdbcDriverInit` export and the resulting `AdbcDriver.DriverVersion` is
  `AdbcVersion.Version_1_1_0`, i.e. the driver accepted the 1.1.0 ABI rather
  than making the importer fall back to 1.0.0.
- **`SelectOne`** — `SELECT 1 AS one` returns a single batch with one `Int32`
  column named `one` holding the value `1`. (SQLite's ODBC driver describes an
  integer literal as `SQL_INTEGER`, which the bridge maps to Arrow `int32`.)
- **`CreateInsertSelectWithNullAndUtf8`** — `CREATE TABLE`, a three-row
  `INSERT`, then `SELECT ... ORDER BY id`. Asserts the reported row count, the
  column names, the integers, that `'ΑΘΗΝΑ ✈ 日本語'` round-trips byte-for-byte
  as UTF-8, and that the SQL `NULL` arrives as an Arrow null rather than an
  empty string.
- **`ParameterisedInsertAndSelect`** — prepares
  `INSERT INTO people (id, name) VALUES (?, ?)`, binds one three-row
  `RecordBatch` of parameters (including a NULL name and a non-ASCII one) with
  `AdbcStatement.Bind`, and checks that `ExecuteUpdate` reports 3
  `AffectedRows`. Then reads the table back, and finally binds a single-row
  batch to `SELECT name FROM people WHERE id = ?` and asserts only the matching
  row comes back.
- **`ErrorCarriesAMessage`** — querying a missing table throws an
  `AdbcException` whose message names the table, i.e. the ODBC diagnostic
  record reaches the caller rather than being swallowed.

`ReadmeSnippet.cs` holds the "Use from C#" snippets from the top-level
`README.md`. They are never run — the paths in them are placeholders — but
`dotnet test` compiles the file, so the snippets cannot silently rot.

## Notes

- The driver is loaded with `CAdbcDriverImporter.Load(path)`, which defaults to
  the `AdbcDriverInit` entry point. `libadbc_driver_odbc.so` also exports
  `AdbcDriverOdbcInit`; pass it as the second argument
  (`CAdbcDriverImporter.Load(path, "AdbcDriverOdbcInit")`) if you prefer — both
  work.
- Parameters are bound as an Arrow `RecordBatch`: one column per `?` and one
  row per execution. `Bind` takes the batch and its schema.
- `bin/` and `obj/` are gitignored.
