# adbcBridge

[![ADBC docs](https://img.shields.io/badge/Apache_Arrow_ADBC_docs-listed-0B7285)](https://arrow.apache.org/adbc/main/integrations.html)
[![CI](https://github.com/singhpratech/adbcbridge/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/singhpratech/adbcbridge/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/adbcbridge)](https://pypi.org/project/adbcbridge/)
[![crates.io](https://img.shields.io/crates/v/adbcbridge)](https://crates.io/crates/adbcbridge)
[![NuGet](https://img.shields.io/nuget/v/AdbcBridge)](https://www.nuget.org/packages/AdbcBridge)
[![Go Reference](https://pkg.go.dev/badge/github.com/singhpratech/adbcbridge/go.svg)](https://pkg.go.dev/github.com/singhpratech/adbcbridge/go)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)

**An [ADBC](https://arrow.apache.org/adbc/) driver for any ODBC data source** —
listed on the Apache Arrow ADBC [Tools & Integrations](https://arrow.apache.org/adbc/main/integrations.html) page.
One plain-C11 shared library that turns every ODBC driver on your machine into an
Arrow-native ADBC driver — columnar record batches out, bulk ingest in — from Python,
Rust, Go, Java, C#, R and anything else that speaks the ADBC driver manager.

Site and docs: <https://adbcbridge.org> · Launch write-up with the numbers:
<https://theaivibe.org/blog/adbcbridge-apache-arrow-adbc-driver-for-any-odbc-database> ·
Write-up on the ADBC docs listing:
<https://theaivibe.org/blog/adbcbridge-listed-apache-arrow-adbc-official-integrations-page>

```
Python / R / Go / Rust / Java / C#
        │  ADBC driver manager
        ▼
 libadbc_driver_odbc.so   ← adbcBridge
        │  ODBC API (unixODBC / iODBC / Windows DM)
        ▼
 Db2 · Oracle · Teradata · SQL Server · Vertica · SAP HANA · Informix · Access ·
 Snowflake · Redshift · SQLite · anything with an ODBC driver
```

*(the names above are what ODBC reaches; the 53 actually verified are in
[`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md))*

Native ADBC drivers exist for a handful of databases. The other few hundred ship an ODBC
driver and nothing else. adbcBridge sits between the ADBC driver manager and that ODBC
driver and does the columnar work once, in C, for all of them — and where a native ADBC
driver *is* installed, it [hands the connection over](docs/how-it-works/delegation.md) so
you get native speed from the same install.

## Measured, not claimed

- **53 databases on Linux, 45 on macOS (Apple Silicon), 48 on Windows** pass one
  workload — types, NULLs, Unicode in parameters and in statement text, bulk ingest,
  batched reads, catalog, error mapping — against a real server or file; every cell that
  is not a pass names its reason. [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md)
- **Five languages × 53 databases on one binary** — Python, Rust, C#, Java and Go,
  261 of 265 cells on Linux, 215 of 225 on macOS, 219 of 240 on Windows, every empty cell
  explained. [`bench/LANGUAGE_BENCHMARKS.md`](bench/LANGUAGE_BENCHMARKS.md)
- **1.2–1.5× the native PostgreSQL ADBC driver** on a 1,000,000-row read split over eight
  connections — on a quiet host; 0.97× on a busy one, 0.60× on an M4 Max. Bulk ingest does
  not beat native (0.73–1.02×) and cannot over ODBC.
  [`docs/how-it-works/performance.md`](docs/how-it-works/performance.md)
- **Four ordinary laptops, never idle** — every number names its host and the load it was
  taken under. [`bench/README.md`](bench/README.md)
- **Bugs found upstream are filed with reproductions** — unixODBC, Virtuoso, Arrow Flight
  SQL ODBC so far. [`docs/UPSTREAM.md`](docs/UPSTREAM.md)

## Quick start

```sh
./install.sh                                  # build + install into ~/.local, no root
pip install adbc-driver-manager pyarrow
```

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(driver="odbc", db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"})
with conn.cursor() as cur:
    cur.execute("SELECT 42 AS answer")
    print(cur.fetch_arrow_table())
```

`install.sh` puts the library in `~/.local/lib` and a driver manifest in
`~/.config/adbc/drivers/odbc.toml` (`~/Library/Application Support/ADBC/Drivers/` on
macOS), a directory the ADBC driver manager already searches — so `driver="odbc"`
resolves with nothing else set. `uri` is an ODBC connection string; `Driver=` takes a
registered ODBC driver name or the path to its library. Prebuilt libraries and Python
wheels for Linux x86_64/aarch64, macOS arm64 and Windows x64 are attached to every
[release](https://github.com/singhpratech/adbcbridge/releases); `pip install adbcbridge`
from PyPI follows.

Once the manifest is installed, every ADBC binding loads the driver by the name `odbc`:

| Language | How to name the driver |
|---|---|
| Python | `dbapi.connect(driver="odbc", db_kwargs={"uri": ...})` |
| R | `adbc_driver("odbc")`, then `adbc_database_init(drv, uri = ...)` (`adbcdrivermanager`) |
| Go | `drivermgr.Driver{}` → `NewDatabase(map[string]string{"driver": "odbc", "uri": ...})` |
| Rust | `ManagedDriver::load_from_name("odbc", None, AdbcVersion::V110, LOAD_FLAG_DEFAULT, None)` |
| Java | `JniDriver.PARAM_DRIVER.set(params, "odbc")` (`adbc-driver-jni`) |
| C# | `AdbcDriverManager.FindLoadDriver("odbc")` (`Apache.Arrow.Adbc.DriverManager`) |

## Use it from every language

One library, loaded by name from every binding. Four of the five packages below — the
wheel, the crate, the nupkg and the jar — are built and attached by the release workflow
(which tests the crate; the bindings' own suites live under `tests/`), and the Go module
comes from the tagged source; the full page for each language covers options, parameters,
bulk ingest, metadata, errors and known limitations.

### Python

```sh
pip install adbcbridge                                  # PyPI; the same wheels are on the release page
```
```python
import adbcbridge
conn = adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;")
with conn.cursor() as cur:
    cur.execute("SELECT 42 AS answer"); print(cur.fetch_arrow_table())
```
The wheel bundles the driver library and loads the ODBC driver before pyarrow can get in
its way. Full page: [`docs/languages/python.md`](docs/languages/python.md).

### Rust

```toml
[dependencies]
adbcbridge = "0.1.0"                                    # crates.io; the default `bundled` feature compiles the driver
```
```rust
let mut driver = adbcbridge::load()?;               // compiles the driver from bundled sources
let database = driver.new_database_with_opts([(OptionDatabase::Uri, "Driver=SQLite3;Database=first.db;".into())])?;
let mut statement = database.new_connection()?.new_statement()?;
statement.set_sql_query("SELECT 42 AS answer")?;
for batch in statement.execute()? { println!("{} row(s)", batch?.num_rows()); }
```
Full page: [`docs/languages/rust.md`](docs/languages/rust.md).

### C#

```sh
dotnet add package AdbcBridge --version 0.1.0          # nuget.org; the .nupkg is on the release page too
```
```csharp
using AdbcConnection connection = Driver.Connect("Driver=SQLite3;Database=first.db;");
using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT 42 AS answer";
IArrowArrayStream stream = (IArrowArrayStream)statement.ExecuteQuery().Stream;
```
`netstandard2.0` and `net8.0`; the library ships as `runtimes/<rid>/native/` assets.
Full page: [`docs/languages/csharp.md`](docs/languages/csharp.md).

### Java

```xml
<dependency><groupId>org.adbcbridge</groupId><artifactId>adbcbridge</artifactId><version>0.1.0</version></dependency>
```
```java
try (RootAllocator allocator = new RootAllocator();
     AdbcDatabase database = AdbcBridge.open(allocator, "Driver=SQLite3;Database=first.db;", null);
     AdbcConnection connection = database.connect();
     AdbcStatement statement = connection.createStatement()) {
  statement.setSqlQuery("SELECT 42 AS answer");
  try (AdbcStatement.QueryResult result = statement.executeQuery()) { /* result.getReader() */ }
}
```
The jar carries the natives (install it from the release with `mvn install:install-file`
until it is on Maven Central); JDK 17+ needs `--add-opens=java.base/java.nio=ALL-UNNAMED`.
Full page: [`docs/languages/java.md`](docs/languages/java.md).

### Go

```sh
go get github.com/singhpratech/adbcbridge/go        # cgo: needs a C compiler
```
```go
db, err := adbcbridge.Open(ctx, memory.DefaultAllocator, "Driver=SQLite3;Database=first.db;", nil)
cnxn, err := db.Open(ctx)
stmt, err := cnxn.NewStatement()
err = stmt.SetSqlQuery("SELECT 42 AS answer")
rdr, _, err := stmt.ExecuteQuery(ctx)
```
The module carries no binary: the library comes from `install.sh`, a release download or
the manifest, or is embedded with `-tags adbcbridge_embed`.
Full page: [`docs/languages/go.md`](docs/languages/go.md).

### C, C++ and R

The C ABI directly — `dlopen` + `AdbcDriverInit`, or the ADBC driver manager —
[`docs/languages/c.md`](docs/languages/c.md); and R through `adbcdrivermanager` —
[`docs/languages/r.md`](docs/languages/r.md).

## Documentation

Everything below except the benchmark index lives under [`docs/`](docs/index.md);
`mkdocs.yml` renders those files as a site with sidebar navigation.

**Getting started** — [Overview](docs/index.md) ·
[Install on Linux](docs/getting-started/install-linux.md) ·
[Install on macOS](docs/getting-started/install-macos.md) ·
[Install on Windows](docs/getting-started/install-windows.md) ·
[Your first query, in six languages](docs/getting-started/first-query.md)

**Use it from** — [Python](docs/languages/python.md) · [Rust](docs/languages/rust.md) ·
[.NET (C#)](docs/languages/csharp.md) · [Java](docs/languages/java.md) ·
[Go](docs/languages/go.md) · [C and C++](docs/languages/c.md) · [R](docs/languages/r.md)

**How it works** — [Performance, with the conditions attached](docs/how-it-works/performance.md) ·
[Native delegation](docs/how-it-works/delegation.md) ·
[Partitioned reads](docs/how-it-works/partitioned-reads.md) ·
[Prefetch](docs/how-it-works/prefetch.md) ·
[Connection keywords set for you](docs/how-it-works/connection-keywords.md)

**Reference** — [Options and environment variables](docs/reference/options.md) ·
[Connection strings, per entry](docs/reference/connection-strings.md) ·
[Type mapping](docs/reference/types.md) · [Driver quirks and why](docs/reference/quirks.md) ·
[Build, install and the driver manifest](docs/reference/install.md) ·
[Building from source and testing](docs/reference/building.md) ·
[Troubleshooting](docs/TROUBLESHOOTING.md)

**Project** — [Compatibility, 53 databases × 3 operating systems](docs/COMPATIBILITY.md) ·
[Benchmarks, by OS](bench/README.md) · [Upstream](docs/UPSTREAM.md) ·
[Roadmap](docs/ROADMAP.md) · [FAQ](docs/community/faq.md) ·
[Contributing](docs/community/contributing.md)

## What it does

- `SELECT` → Arrow record batches: columns bound once with `SQLBindCol` into rowsets of up
  to 8 MiB, copied column-at-a-time; UTF-16 → UTF-8, decimals → `decimal128`, long and
  unbounded columns chunked through `SQLGetData`.
- Types: bool, int8–64 (and unsigned), float/double, char/varchar/nvarchar, binary, date,
  time, timestamp (µs/ns), decimal — [type mapping](docs/reference/types.md).
- DML with `rows_affected`, prepared statements, autocommit / commit / rollback;
  `GetInfo`, `GetObjects`, `GetTableTypes`, `GetTableSchema`; structured ODBC errors
  (SQLSTATE + native code).
- Parameter binding (`Bind`/`BindStream`) and bulk ingest: parameter arrays or multi-row
  `INSERT` (or Oracle's `INSERT ALL`, or Firebird's `UNION ALL SELECT`), probed once per
  connection, fanned out over up to 64 connections —
  [performance](docs/how-it-works/performance.md#bulk-ingest).
- [Partitioned reads](docs/how-it-works/partitioned-reads.md) (`ExecutePartitions`: `ctid`
  on a PostgreSQL heap, key range elsewhere, `yb_hash_code()` on YugabyteDB) and a
  [prefetch](docs/how-it-works/prefetch.md) pipeline.
- ADBC 1.0.0 and 1.1.0 ABI; discoverable by name through an ADBC driver manifest on
  Linux, macOS and Windows.
- [Native delegation](docs/how-it-works/delegation.md): a connection string that names a
  database with a native ADBC driver (PostgreSQL, SQLite, DuckDB, Snowflake, BigQuery,
  Flight SQL) is handed to that driver, when it is installed.
- Every ODBC driver quirk that the 53 databases needed is detected from the driver's own
  name and handled — [driver quirks](docs/reference/quirks.md).
- Windows: the prefetch pipeline and parallel ingest are compiled out until a Win32 thread
  shim lands — [roadmap](docs/ROADMAP.md).

## Language packages

One driver library, five packages that find and load it. Four of them — the wheel, the
crate, the nupkg and the jar — are built and attached to every
[release](https://github.com/singhpratech/adbcbridge/releases) together with the bare
libraries (the release workflow tests the crate; the bindings' own suites live under
`tests/`); the Go module is fetched with `go get` from the tagged source (`go/v0.1.0`,
the sub-module tag). The wheel is on [PyPI](https://pypi.org/project/adbcbridge/),
the crate on [crates.io](https://crates.io/crates/adbcbridge) and the nupkg on
[nuget.org](https://www.nuget.org/packages/AdbcBridge) (all 0.1.0); the jar is not on Maven
Central yet — install it from the release assets.

| Language | Package | What it gives you | Where |
|---|---|---|---|
| Python | `adbcbridge` wheel, `py3-none-<platform>` with the library bundled | `adbcbridge.connect(uri=...)` → `adbc_driver_manager.dbapi` connection; `adbcbridge` CLI | [`python/`](python/README.md) · [docs](docs/languages/python.md) |
| Rust | `adbcbridge` crate; the default `bundled` feature compiles the driver from the sources carried in the crate | `adbcbridge::load()?` → `ManagedDriver` | [`rust/`](rust/README.md) · [docs](docs/languages/rust.md) |
| C# | `AdbcBridge` NuGet (netstandard2.0, net8.0) with `runtimes/<rid>/native/` assets | `Driver.Load()`, `Driver.Connect(connectionString)` | [`csharp/`](csharp/README.md) · [docs](docs/languages/csharp.md) |
| Java | `org.adbcbridge:adbcbridge` over `adbc-driver-jni`, natives inside the jar | `AdbcBridge.driver(allocator)`, `AdbcBridge.open(...)` | [`java/`](java/README.md) · [docs](docs/languages/java.md) |
| Go | `github.com/singhpratech/adbcbridge/go` over `drivermgr` (cgo) | `adbcbridge.NewDriver(alloc)`, `adbcbridge.Open(...)` | [`go/`](go/README.md) · [docs](docs/languages/go.md) |

Each package resolves the library in the same order: an explicit override
(`ADBC_ODBC_DRIVER` everywhere; `ADBCBRIDGE_LIBRARY` too in Rust, C#, Java and Go, and the
`adbcbridge.library` property in Java), a copy shipped inside the package, the ADBC driver
manifest named `odbc`, the usual install directories, then a `build/` tree next to a
checkout — and each raises an error when it cannot; Rust, C#, Java and Go list every
place they looked.

## Status and roadmap

Early (0.1.0). Working: everything under *What it does*, on Linux, macOS (arm64) and
Windows (x64 and Win32 built and tested in CI on every push; the Windows build lacks
prefetch and parallel ingest); 0.1.0 on PyPI, crates.io and nuget.org. Next: the ADBC Driver
Foundry validation suite, Maven Central, a driver bootstrap for the open-licence ODBC
drivers, the Win32 thread shim; then a JDBC bridge on the same model —
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Upstream: giving back

Running 53 databases through one driver, 43 of them on three operating systems, finds defects that
belong to other projects. They are reported with a reproduction that needs no adbcBridge
in the stack — [lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239)
(the driver manager aborts on the first SQL error from a 4-byte-`SQLWCHAR` driver; the
maintainer committed a check the same day),
[openlink/virtuoso-opensource#1469](https://github.com/openlink/virtuoso-opensource/issues/1469)
and [dremio/warpdrive#16](https://github.com/dremio/warpdrive/issues/16). The full record,
filed or not yet, is [`docs/UPSTREAM.md`](docs/UPSTREAM.md).

## Contributing

[`docs/community/contributing.md`](docs/community/contributing.md) — how the code is laid
out, how to add a database to the matrix or a driver quirk, how to run the tests; the
short version is [`CONTRIBUTING.md`](CONTRIBUTING.md). Bring a database with an ODBC
driver that is not in the list, or a binding you want measured: the matrix is one Python
file and a `docker-compose` service per database.

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
