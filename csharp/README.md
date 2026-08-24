<!--
Copyright 2026 the adbcbridge authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

SPDX-License-Identifier: Apache-2.0
-->

# AdbcBridge (C#)

Thin .NET wrapper around [adbcbridge](https://github.com/singhpratech/adbcbridge),
the ADBC driver for any ODBC data source. It finds the `libadbc_driver_odbc`
shared library and hands it to
[`Apache.Arrow.Adbc`](https://www.nuget.org/packages/Apache.Arrow.Adbc)'s
native driver importer, so every ODBC driver on the machine becomes an
Arrow-native ADBC database.

Targets `netstandard2.0` and `net8.0`; depends on `Apache.Arrow.Adbc` 0.24.0.

## Install

```sh
dotnet add package AdbcBridge
```

The machine also needs an ODBC driver manager (`unixodbc` on Linux, iODBC or
unixODBC on macOS, built in on Windows) and the ODBC driver for your database.

## The three calls

```csharp
using AdbcBridge;
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Ipc;

// 1. Where is the driver? Absolute path, or DriverNotFoundException listing every place searched.
string path = Driver.Path();

// 2. Load it: the Apache.Arrow.Adbc.AdbcDriver from CAdbcDriverImporter.Load(path).
using AdbcDriver driver = Driver.Load();

// 3. Or skip straight to a connection. The first argument is an ODBC connection
//    string (it becomes the database's `uri`); the optional dictionary holds
//    further database options such as adbc.odbc.batch_size.
using AdbcConnection connection = Driver.Connect(
    "Driver=SQLite3;Database=my.db;",
    new Dictionary<string, string> { ["adbc.odbc.batch_size"] = "4096" });

using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT 42 AS answer";
IArrowArrayStream stream = (IArrowArrayStream)statement.ExecuteQuery().Stream;
while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
{
    Console.WriteLine(batch.Length);
}
```

`Driver=` in the connection string takes either a driver name registered in
`odbcinst.ini` or the path of the ODBC driver library. A `DSN=` string works
too. `Driver.Connect` keeps the `AdbcDatabase` and `AdbcDriver` it opened alive
for as long as the connection object is; dispose the connection when done.

## How the driver is found

`Driver.Path()` returns the first hit, in this order:

1. `ADBCBRIDGE_LIBRARY` — the library's path. Set but not a file is an error, not a fall-through.
2. `ADBC_ODBC_DRIVER` — the same variable the Python package and the test suite use.
3. The native asset shipped inside this package: `runtimes/<rid>/native/libadbc_driver_odbc.{so,dylib,dll}`
   relative to the `AdbcBridge` assembly (or flattened next to it by a RID-specific publish).
4. The ADBC driver manifest named `odbc` (`odbc.toml`) in the directories the ADBC
   driver manager searches: `ADBC_DRIVER_PATH`, then `~/.config/adbc/drivers`
   (`$XDG_CONFIG_HOME/adbc/drivers`), `/etc/adbc/drivers`, `/usr/local/etc/adbc/drivers`,
   `/usr/share/adbc/drivers`, `/usr/local/share/adbc/drivers`;
   `~/Library/Application Support/ADBC/Drivers` and `/Library/Application Support/ADBC/Drivers`
   on macOS; `%LOCALAPPDATA%\ADBC\Drivers` and `%PROGRAMDATA%\ADBC\Drivers` on Windows.
   This is what `./install.sh` and `cmake --install` write.
5. Common install directories: `~/.local/lib`, `/usr/local/lib`, `/usr/lib`
   (and their `lib64` and `<arch>-linux-gnu` variants), `/opt/adbcbridge/lib`, `/opt/homebrew/lib`.
6. A CMake `build/` tree (or `build/Release`, `build/Debug`, ...) in any directory
   above the application, the assembly, or the current directory, so a source
   checkout works straight after `cmake --build build`.

When nothing matches, `Driver.Path()` throws `AdbcBridge.DriverNotFoundException`;
its message and `Searched` property list every location that was examined.

## Native asset

The package on NuGet may or may not carry the native library, depending on how
it was packed:

- Packed with `-p:NativeRoot=<dir>`, it ships `runtimes/<rid>/native/libadbc_driver_odbc.*`
  for every `<rid>` found under `<dir>` — `linux-x64`, `linux-arm64`, `osx-arm64`
  and `win-x64` are the ones built for releases. A `net8.0` application picks the
  right one up automatically (step 3 above): a framework-dependent build copies the
  whole `runtimes/` tree next to the application, a RID-specific publish copies the
  one library beside it.
- Packed without `NativeRoot`, it is managed-only and relies on steps 1-2 and 4-6:
  install the driver (`./install.sh` in a checkout, or `cmake --install`), or set
  `ADBCBRIDGE_LIBRARY`.
- `netstandard2.0` consumers on .NET Framework do not get `runtimes/` assets copied by
  the build; install the driver or set the variable there.

The native library in turn needs the ODBC driver manager it was linked against
(`libodbc.so.2` from unixODBC on Linux).

## Building the package

```sh
cd csharp
dotnet build
dotnet test                                   # needs SQLITE_ODBC_DRIVER, skips otherwise
dotnet pack AdbcBridge -c Release             # managed-only package
dotnet pack AdbcBridge -c Release -p:NativeRoot=/abs/path/to/native   # with native assets
```

`NativeRoot` must be laid out as `<rid>/libadbc_driver_odbc.{so,dylib,dll}`:

```
native/
├── linux-x64/libadbc_driver_odbc.so
├── linux-arm64/libadbc_driver_odbc.so
├── osx-arm64/libadbc_driver_odbc.dylib
└── win-x64/libadbc_driver_odbc.dll
```

Missing rids are simply not packed; a `NativeRoot` that is not a directory, or
that holds no library at all, fails the pack.

## Tests

`AdbcBridge.Tests` holds one real test: find the driver, load it, connect to
SQLite through the ODBC driver `SQLITE_ODBC_DRIVER` names
(`Driver=<value>;Database=:memory:;`), run `SELECT 1` and read the Arrow batch.
It is skipped with a message when `SQLITE_ODBC_DRIVER` is unset. The driver
itself is found through `Driver.Path()`, so either set `ADBC_ODBC_DRIVER`, or
build the repo (`cmake --build build`) and let step 6 find it.
