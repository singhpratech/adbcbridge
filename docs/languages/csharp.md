<!-- SPDX-License-Identifier: Apache-2.0 -->

# .NET (C#)

adbcBridge is a plain-C11 ADBC driver that talks to any ODBC data source. ADBC
(Arrow Database Connectivity) is a database API whose result sets are Apache
Arrow record batches; ODBC (Open Database Connectivity) is the older, row-based
C API that almost every database ships a driver for. The driver is one shared
library — `libadbc_driver_odbc.so` on Linux, `.dylib` on macOS,
`libadbc_driver_odbc.dll` on Windows.

The `AdbcBridge` NuGet package is a thin .NET wrapper around that library. It
does two things: it **finds** the shared library on the machine (or unpacks the
copy shipped inside the package), and it **hands it to** `Apache.Arrow.Adbc`,
the official Arrow ADBC binding for .NET. Everything after that — connections,
statements, Arrow record batches — is the `Apache.Arrow.Adbc` API.

This page assumes you are comfortable in C# but new to ODBC and ADBC.

---

## Table of contents

- [What the package contains](#what-the-package-contains)
- [Requirements](#requirements)
- [Install](#install)
- [The three entry points](#the-three-entry-points)
- [How the library is located](#how-the-library-is-located)
- [Connection strings](#connection-strings)
- [Driver options](#driver-options)
- [Running a query](#running-a-query)
- [Bulk ingest](#bulk-ingest)
- [Parameters](#parameters)
- [Metadata](#metadata)
- [Errors](#errors)
- [Target frameworks and ADO.NET](#target-frameworks-and-adonet)
- [Known limitations](#known-limitations)
- [Complete worked example](#complete-worked-example)

---

## What the package contains

| Item | Value |
|---|---|
| Package id | `AdbcBridge` |
| Version | `0.1.0` |
| Target frameworks | `netstandard2.0`, `net8.0` |
| Dependency | `Apache.Arrow.Adbc` `0.24.0` |
| Public type | `AdbcBridge.Driver` (static), `AdbcBridge.DriverNotFoundException` |
| License | Apache-2.0 |

The package always contains the managed wrapper. It **may** also carry the
native driver as a runtime asset, depending on how it was packed:

- The package attached to a GitHub Release is packed with the native library
  present, laid out as `runtimes/<rid>/native/libadbc_driver_odbc.{so,dylib,dll}`
  for each RID (runtime identifier — .NET's `os-arch` platform tag). The RIDs
  built for releases are `linux-x64`, `linux-arm64`, `osx-arm64` and `win-x64`.
- A package packed without native assets is managed-only; it relies on finding
  the library elsewhere on the machine (see
  [How the library is located](#how-the-library-is-located)).

Tip: `runtimes/<rid>/native/` is the standard NuGet layout for native assets. A
framework-dependent `net8.0` build copies the whole `runtimes/` tree next to
your application; a RID-specific publish (`dotnet publish -r <rid>`) flattens the
one matching library beside the application. Either way the wrapper finds it
automatically.

---

## Requirements

1. **An ODBC driver manager**, which the native library links against:
   - Linux: unixODBC (provides `libodbc.so.2`); on Debian/Ubuntu, `unixodbc`.
   - macOS: unixODBC or iODBC.
   - Windows: built into the operating system.
2. **The ODBC driver for your database** (for example the SQLite ODBC driver
   `libsqlite3odbc.so`, or psqlodbc for PostgreSQL). adbcBridge bridges to
   whichever ODBC drivers are installed; it does not contain them.
3. The .NET SDK/runtime for one of the target frameworks.

---

## Install

### From a GitHub Release now (local NuGet source)

The `.nupkg` is attached to each release. Download it, register the folder that
holds it as a package source, then add the package:

```sh
# 1. Download AdbcBridge.0.1.0.nupkg from the Release into ./localnuget/
mkdir -p localnuget
# (place AdbcBridge.0.1.0.nupkg in ./localnuget/)

# 2. Register the folder as a NuGet source.
dotnet nuget add source "$PWD/localnuget" --name adbcbridge-local

# 3. Add the package to your project.
dotnet add package AdbcBridge --version 0.1.0 --source adbcbridge-local
```

The release `.nupkg` is available on the project's GitHub Releases page at
<https://github.com/singhpratech/adbcbridge/releases>.

### From nuget.org once published

```sh
dotnet add package AdbcBridge
```

Registry publication follows the first releases; until then, use the local
source above.

### Building the package yourself

From a source checkout:

```sh
cd csharp
dotnet build
dotnet pack AdbcBridge -c Release                                   # managed-only package
dotnet pack AdbcBridge -c Release -p:NativeRoot=/abs/path/to/native # with native assets
```

`NativeRoot` must be a directory laid out as `<rid>/libadbc_driver_odbc.{so,dylib,dll}`:

```
native/
├── linux-x64/libadbc_driver_odbc.so
├── linux-arm64/libadbc_driver_odbc.so
├── osx-arm64/libadbc_driver_odbc.dylib
└── win-x64/libadbc_driver_odbc.dll
```

Every library found there is packed under `runtimes/<rid>/native/`. Missing RIDs
are simply skipped; a `NativeRoot` that is not a directory, or that holds no
library at all, fails the pack.

---

## The three entry points

Everything the wrapper adds lives on the static class `AdbcBridge.Driver`:

```csharp
using AdbcBridge;
using Apache.Arrow.Adbc;

// 1. Where is the native library? Returns an absolute path, or throws
//    DriverNotFoundException listing every place it looked.
string path = Driver.Path();

// 2. Load it: returns the Apache.Arrow.Adbc.AdbcDriver produced by
//    CAdbcDriverImporter.Load(path). Dispose it when done.
using AdbcDriver driver = Driver.Load();

// 3. Or skip straight to an open connection from an ODBC connection string.
using AdbcConnection connection = Driver.Connect("Driver=SQLite3;Database=my.db;");
```

| Method | Returns | Notes |
|---|---|---|
| `Driver.Path()` | `string` | Absolute path of the native library. Throws `DriverNotFoundException` if not found. |
| `Driver.Load()` | `AdbcDriver` | Calls `CAdbcDriverImporter.Load(Driver.Path())`. Caller disposes. |
| `Driver.Connect(string connectionString, IReadOnlyDictionary<string,string>? options = null)` | `AdbcConnection` | Loads the driver, opens an `AdbcDatabase` with `uri` set to `connectionString`, and connects. |
| `Driver.RuntimeIdentifier()` | `string` | The `os-arch` RID (`linux-x64`, …) the native asset is filed under for this process. |

`Driver.Connect` keeps the `AdbcDatabase` and `AdbcDriver` it opened alive for
as long as the returned `AdbcConnection` is; dispose the connection when done and
the rest is released with it. A `uri` entry in `options` is overridden by
`connectionString`.

The constants used along the way are public:

| Constant | Value |
|---|---|
| `Driver.LibraryVariable` | `ADBCBRIDGE_LIBRARY` |
| `Driver.DriverVariable` | `ADBC_ODBC_DRIVER` |
| `Driver.ManifestPathVariable` | `ADBC_DRIVER_PATH` |
| `Driver.ManifestName` | `odbc.toml` |

---

## How the library is located

`Driver.Path()` returns the first hit, in this order. The environment variables
are the two ways to point it at a specific build.

| # | Where it looks | Detail |
|---|---|---|
| 1 | `ADBCBRIDGE_LIBRARY` | The library's full path. Set but not a file is an **error**, not a fall-through. |
| 2 | `ADBC_ODBC_DRIVER` | The same variable the Python package and the test suite use. Same error rule. |
| 3 | Native asset in this package | `runtimes/<rid>/native/libadbc_driver_odbc.*` next to the `AdbcBridge` assembly, or flattened beside it by a RID-specific publish. |
| 4 | ADBC driver manifest `odbc.toml` | In the directories the ADBC driver manager searches: `ADBC_DRIVER_PATH` first, then `~/.config/adbc/drivers` (`$XDG_CONFIG_HOME/adbc/drivers`), `/etc/adbc/drivers`, `/usr/local/etc/adbc/drivers`, `/usr/share/adbc/drivers`, `/usr/local/share/adbc/drivers`. On macOS, `~/Library/Application Support/ADBC/Drivers` and `/Library/Application Support/ADBC/Drivers`; on Windows, `%LOCALAPPDATA%\ADBC\Drivers` and `%PROGRAMDATA%\ADBC\Drivers`. This is what `install.sh` and `cmake --install` write. |
| 5 | Common install directories | `~/.local/lib`, `/usr/local/lib`, `/usr/lib` (and their `lib64` and `<arch>-linux-gnu` variants), `/opt/adbcbridge/lib`, `/opt/homebrew/lib`. On Windows, `%LOCALAPPDATA%\adbcbridge\bin` and `%ProgramFiles%\adbcbridge\bin`. |
| 6 | A CMake `build/` tree | `build/`, `build/Release`, `build/Debug`, `build/RelWithDebInfo`, `build/MinSizeRel` in any directory above the application, the assembly, or the current directory — so a source checkout works right after `cmake --build build`. |

When nothing matches, `Driver.Path()` throws `AdbcBridge.DriverNotFoundException`.
Its `Message` and its `Searched` (`IReadOnlyList<string>`) property list every
location that was examined, in order, and its `LibraryName` is the file name it
looked for.

Troubleshooting: if `Driver.Path()` throws, read the `Searched` list — it names
every directory tried. The quickest fix is to set `ADBC_ODBC_DRIVER` to the
absolute path of your `libadbc_driver_odbc.*`, or install the driver with
`./install.sh` / `cmake --install` so the `odbc.toml` manifest is written.

---

## Connection strings

The first argument to `Driver.Connect` is a plain ODBC connection string. It
becomes the ADBC database's `uri` option. Two forms work:

- **DSN-less** (names the ODBC driver directly):

  ```text
  Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;
  ```

  `Driver=` takes either a driver **name** registered in `odbcinst.ini` (for
  example `Driver=SQLite3;`) or the **path** of the ODBC driver library.

- **DSN** (names a data source from `odbc.ini`):

  ```text
  DSN=warehouse;UID=me;PWD=secret;
  ```

Real connection strings from the project's compatibility matrix:

| Database | Connection string |
|---|---|
| SQLite | `Driver=<libsqlite3odbc.so>;Database=/path/to/m.db;` |
| PostgreSQL | `Driver=<psqlodbcw.so>;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| MySQL | `Driver=<libmyodbc.so>;Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;` |
| SQL Server | `Driver=<msodbcsql>;Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=…;TrustServerCertificate=yes;` |
| Oracle | `Driver=<liboraodbc.so>;DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;` |

Substitute the driver name or path for the value in angle brackets.

---

## Driver options

Pass driver options as the second argument to `Driver.Connect` (an
`IReadOnlyDictionary<string,string>`), or set them directly on an `AdbcDatabase`
if you drove `Driver.Load()` yourself. Keys and string values:

```csharp
using AdbcConnection connection = Driver.Connect(
    "Driver=SQLite3;Database=my.db;",
    new Dictionary<string, string> { ["adbc.odbc.batch_size"] = "4096" });
```

The database also understands the generic ADBC options `uri` (the full ODBC
connection string — this is what `connectionString` becomes), `dsn` (a DSN name,
appended as `DSN=…`), and `username` / `password` (appended as `UID=` / `PWD=`).

The `adbc.odbc.*` options below come from the driver:

| Option | Meaning |
|---|---|
| `adbc.odbc.batch_size` | Rows per Arrow batch (default `1024`). |
| `adbc.odbc.max_bind_bytes` | Widest value bound at the width the driver declares for it, in bytes (default `32768`). Wider values are bound at `long_bind_bytes`, or read with `SQLGetData` where that is not possible. |
| `adbc.odbc.long_bind_bytes` | Width, in bytes, at which to bind a column whose declared width is not a real bound — `TEXT`/`NVARCHAR(MAX)`/`LONGTEXT`/`bytea` and similar (default `2048`). Values longer than this are re-read in full, so this trades only speed. |
| `adbc.odbc.rowset_bytes` | Ceiling on a reader's bound rowset buffers, in bytes (default `8388608`). Holds `batch_size` rows unless that would cost more than this. |
| `adbc.odbc.decimal_as_string` | `true` to return `DECIMAL`/`NUMERIC` as strings. |
| `adbc.odbc.partitions` | How many partitions `AdbcStatementExecutePartitions` splits a query into — `0` (default) chooses from the table's size, `1` never splits. Set on the statement. |
| `adbc.odbc.prefetch` | Rowsets kept in flight on a background fetch thread — `0` (default) is off, `1` is double buffering, up to `8`. Settable on the database, connection, or statement. |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — see [Native delegation](#native-delegation-a-note). |
| `adbc.odbc.delegate.driver` | Force a specific native driver: a bare name (`postgresql`) or manifest name; a path only with `allow_paths`. |
| `adbc.odbc.delegate.search_path` | Extra directories to search for native drivers (`:`-separated); needs `allow_paths`. |
| `adbc.odbc.delegate.allow_paths` | `true` to let the two options above name filesystem paths (default `false`). |
| `adbc.odbc.delegate.last_error` | Read-only: why delegation did not happen. |
| `adbc.odbc.delegated_to` | Read-only: the native driver serving this database/connection, or `odbc`. |
| `adbc.odbc.tune` | `true` (default) / `false` — may the driver add ODBC connection keywords of its own where it recognises the target driver? `false` sends your connection string through untouched. |
| `adbc.odbc.sqllen_32bit` | `true`/`false` to force the 32-bit-`SQLLEN` driver quirk on or off. Autodetected (on for IBM Db2), so you normally never set it. Also settable on the connection and statement. |

### Native delegation (a note)

Some databases have a first-class native ADBC driver (PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL). When `adbc.odbc.delegate` is `auto`
(the default) and adbcBridge recognises such a target, it hands the whole
database over to that native driver for native speed. Set
`adbc.odbc.delegate=never` to force the ODBC path (this is what the benchmark
harness does so its numbers describe the ODBC driver), or `always` to make a
missing native driver an error instead of a fall-through. Delegation is not
implemented on Windows — there `auto` always takes the ODBC path.

---

## Running a query

`Driver.Connect` gives you an `Apache.Arrow.Adbc.AdbcConnection`. Create a
statement, set its SQL, execute, and drain the resulting Arrow stream:

```csharp
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Ipc;   // IArrowArrayStream

using AdbcConnection connection = Driver.Connect("Driver=SQLite3;Database=my.db;");
using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT id, name FROM customers";

QueryResult result = statement.ExecuteQuery();
IArrowArrayStream stream = (IArrowArrayStream)result.Stream;
while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
{
    Console.WriteLine($"batch of {batch.Length} rows");
    // batch.Column(0), batch.Schema.GetFieldByIndex(0).Name, ...
}
```

- `statement.ExecuteQuery()` returns a `QueryResult`; its `Stream` is an
  `IArrowArrayStream` (`Apache.Arrow.Ipc`).
- `stream.Schema` is the Arrow `Schema`; `stream.ReadNextRecordBatchAsync()`
  yields `RecordBatch` values until it returns `null`.
- For DML that returns no rows, use `statement.ExecuteUpdate()` instead of
  `ExecuteQuery()`.

Tip: dispose (or read to completion on) the `IArrowArrayStream` on the same
thread that created it, before disposing the statement. If the stream's release
callback runs later on the .NET finalizer thread, an ODBC driver whose client
library is thread-affine can crash inside `SQLCloseCursor`. A `using` on the
stream is the simplest guarantee.

Transactions: `connection.AutoCommit` defaults to on. Set it to `false` and call
`connection.Commit()` / `connection.Rollback()` to control transactions
yourself.

---

## Bulk ingest

Bulk ingest writes an Arrow `RecordBatch` into a table. The option keys come from
`Apache.Arrow.Adbc.AdbcOptions.Ingest`:

```csharp
using Apache.Arrow;
using Apache.Arrow.Adbc;

RecordBatch batch = /* build your batch */;

using AdbcStatement statement = connection.CreateStatement();
statement.SetOption(AdbcOptions.Ingest.TargetTable, "my_table");
statement.SetOption(AdbcOptions.Ingest.Mode, AdbcOptions.IngestMode.Create);
statement.Bind(batch, batch.Schema);
statement.ExecuteUpdate();

// With AutoCommit off, commit when the ingest is done:
connection.Commit();
```

- `AdbcOptions.Ingest.Mode` accepts `AdbcOptions.IngestMode.Create` (create the
  table and insert), among the other ADBC ingest modes.
- The driver generates the `CREATE TABLE` DDL from the Arrow schema, then sends
  one multi-row `INSERT` per batch of rows inside a single transaction. It works
  on every ODBC driver that can bind a parameter. `adbc.odbc.rows_per_insert`
  overrides the rows-per-`INSERT` the driver chooses.

---

## Parameters

The driver supports parameter binding (`Bind` / `BindStream`). To run a
parameterised statement, bind a single-row (or multi-row) `RecordBatch` whose
columns are the parameter values, then execute:

```csharp
using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT * FROM customers WHERE id = ?";
statement.Bind(parameterBatch, parameterBatch.Schema);  // one column per '?'
QueryResult result = statement.ExecuteQuery();
```

Binding a multi-row batch applies the statement once per row (an ODBC parameter
array), which is what bulk ingest builds on. The placeholder syntax (`?`, `$1`,
`:name`, …) is whatever the underlying ODBC driver and database accept.

---

## Metadata

The ADBC connection exposes the standard metadata calls, backed by the ODBC
driver's catalog functions: `GetInfo`, `GetObjects` (catalogs, schemas, tables,
columns), `GetTableTypes`, and `GetTableSchema`. Each returns Arrow data through
the same `Apache.Arrow.Adbc.AdbcConnection` you already have. Consult the
`Apache.Arrow.Adbc` API for the exact method signatures on your version
(`0.24.0`).

---

## Errors

Operations throw `Apache.Arrow.Adbc.AdbcException`. The driver maps each ODBC
diagnostic into it as a structured error carrying the **SQLSTATE** (the
five-character ODBC status code) and the driver's **native error code**, so you
can branch on the database's own error identity rather than parsing message
text. The precise property names belong to the `Apache.Arrow.Adbc` API; catch
`AdbcException` and inspect it:

```csharp
try
{
    using AdbcConnection connection = Driver.Connect("Driver=SQLite3;Database=my.db;");
    using AdbcStatement statement = connection.CreateStatement();
    statement.SqlQuery = "SELECT * FROM does_not_exist";
    statement.ExecuteQuery();
}
catch (AdbcException ex)
{
    Console.Error.WriteLine(ex.Message);   // includes SQLSTATE + native code
}
```

`AdbcBridge.DriverNotFoundException` is separate: it is thrown only when the
native library itself cannot be located (see
[How the library is located](#how-the-library-is-located)), before any database
work begins.

---

## Target frameworks and ADO.NET

The package targets `netstandard2.0` and `net8.0`.

- On `net8.0`, native assets under `runtimes/<rid>/native/` are resolved
  automatically by the build.
- On `netstandard2.0` consumers running on .NET Framework, `runtimes/` assets
  are **not** copied by the build; install the driver on the machine, or set
  `ADBCBRIDGE_LIBRARY`.

The `AdbcBridge` package does **not** provide an ADO.NET (`System.Data`)
provider — it is a locator and loader for the ADBC driver, and its surface is the
`Apache.Arrow.Adbc` types (`AdbcConnection`, `AdbcStatement`, and Arrow record
batches), not `IDbConnection` / `IDbCommand`.

---

## Known limitations

- **The ODBC stack is not bundled.** You still need an ODBC driver manager
  (unixODBC/iODBC on Unix, built in on Windows) and the ODBC driver for your
  database. The native library links against the driver manager
  (`libodbc.so.2` on Linux).
- **Native assets are present only if the package was packed with them.** The
  release `.nupkg` carries `linux-x64`, `linux-arm64`, `osx-arm64` and `win-x64`.
  Any other platform, or a managed-only package, must find the library through
  the environment, a manifest, or an install directory.
- **`netstandard2.0` on .NET Framework** does not get `runtimes/` native assets
  copied automatically; provide the library yourself.
- **Native delegation is not available on Windows** — `auto` always takes the
  ODBC path there and `always` fails with a message.
- **Thread affinity.** Release the Arrow result stream on the thread that created
  it (a `using`), not on the finalizer thread — some ODBC client libraries are
  thread-affine.
- **`Apache.Arrow.Adbc` version.** The package is built against `0.24.0`; use a
  compatible version in your project.

---

## Complete worked example

A minimal console application that finds the driver, connects to an in-memory
SQLite database through the SQLite ODBC driver, runs a query, and reads the Arrow
result.

### `HelloAdbcBridge.csproj`

```xml
<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>enable</Nullable>
    <LangVersion>latest</LangVersion>
    <!-- Brings in System, System.Linq, System.Collections.Generic, etc.,
         which the Program.cs below uses (.Select, Dictionary, top-level await). -->
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>

  <ItemGroup>
    <!-- From nuget.org once published; until then, add a local source:
         dotnet nuget add source "$PWD/localnuget" --name adbcbridge-local -->
    <PackageReference Include="AdbcBridge" Version="0.1.0" />
  </ItemGroup>

</Project>
```

### `Program.cs`

```csharp
// SPDX-License-Identifier: Apache-2.0
using AdbcBridge;
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Ipc;

// The SQLite ODBC driver: a name from odbcinst.ini (e.g. "SQLite3") or the path
// of libsqlite3odbc.so. Point SQLITE_ODBC_DRIVER at it, or edit the fallback.
string sqlite = Environment.GetEnvironmentVariable("SQLITE_ODBC_DRIVER") ?? "SQLite3";

try
{
    // 1. Find + load + connect, in one call. The string is an ODBC connection string.
    using AdbcConnection connection = Driver.Connect(
        $"Driver={sqlite};Database=:memory:;",
        new Dictionary<string, string> { ["adbc.odbc.batch_size"] = "1024" });

    // 2. Run a query.
    using AdbcStatement statement = connection.CreateStatement();
    statement.SqlQuery = "SELECT 42 AS answer, 'hi' AS greeting";

    // 3. Drain the Arrow stream on this thread.
    QueryResult result = statement.ExecuteQuery();
    using IArrowArrayStream stream = (IArrowArrayStream)result.Stream!;

    Console.WriteLine("columns: " +
        string.Join(", ", stream.Schema.FieldsList.Select(f => f.Name)));

    while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
    {
        var answers = (Int32Array)batch.Column(0);
        for (int i = 0; i < batch.Length; i++)
        {
            Console.WriteLine($"row {i}: answer = {answers.GetValue(i)}");
        }
    }
}
catch (DriverNotFoundException ex)
{
    Console.Error.WriteLine("adbcBridge library not found:");
    Console.Error.WriteLine(ex.Message);   // lists every place searched
    Environment.Exit(1);
}
catch (AdbcException ex)
{
    Console.Error.WriteLine("database error: " + ex.Message);
    Environment.Exit(1);
}
```

### Run it

```sh
# The SQLite ODBC driver must be installed; name or path in SQLITE_ODBC_DRIVER.
export SQLITE_ODBC_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so
dotnet run
```

Expected output:

```text
columns: answer, greeting
row 0: answer = 42
```
