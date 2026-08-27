<!-- SPDX-License-Identifier: Apache-2.0 -->
# Rust

adbcBridge is a plain-C ADBC (Arrow Database Connectivity) driver that reaches
any data source with an ODBC (Open Database Connectivity) driver. This page
covers using it from Rust through the `adbcbridge` crate.

The driver itself is a C shared library (`libadbc_driver_odbc.so` on Linux,
`.dylib` on macOS, `.dll` on Windows). The `adbcbridge` crate does two small
things:

1. **Finds** a copy of that library on your machine (`driver_path`), and
2. **Loads** it into the ADBC driver manager (`load`), after which the ordinary
   [`adbc_core`](https://crates.io/crates/adbc_core) traits — `Driver`,
   `Database`, `Connection`, `Statement` — run every query.

Everything past loading is standard ADBC: adbcBridge adds no query API of its
own, only the ODBC connection string and a handful of `adbc.odbc.*` options.

> **Abbreviations.** *ADBC* — Arrow Database Connectivity, a columnar database
> API that returns Apache Arrow data. *ODBC* — Open Database Connectivity, the
> older row-oriented C API that most databases ship a driver for. *Driver
> manager* — the library that loads a driver by name or path (unixODBC or iODBC
> on Unix, `odbc32` on Windows for ODBC; `adbc_driver_manager` for ADBC).

## What the crate is

| | |
|---|---|
| Crate name | `adbcbridge` |
| Version | `0.1.0` (early) |
| Edition | 2021 |
| License | Apache-2.0 |
| Dependencies | `adbc_core = "0.24"`, `adbc_driver_manager = "0.24"` |
| Build dependency | `cc = "1.0.83"` (only with the `bundled` feature) |
| Unsafe code | none — the crate is `#![forbid(unsafe_code)]` |

The crate re-exports `adbc_core`, `adbc_driver_manager` and
`adbc_driver_manager::ManagedDriver`, so you can name those types without
pinning their versions yourself. The crate's own README is
[`rust/README.md`](../../rust/README.md); the crate is not required — see
[Without the crate](#without-the-crate) for loading the driver through
`adbc_driver_manager` alone.

### Feature flags

| Feature | Default | Effect |
|---|---|---|
| `bundled` | **on** | `build.rs` compiles the driver from the C sources shipped inside the crate (`csrc/`, a verbatim copy of the repository's `src/`, `include/` and `vendor/nanoarrow/`) and links it against the platform's ODBC driver manager. |

With `bundled` off (`--no-default-features`) the crate only *locates* a
`libadbc_driver_odbc` library that is already on the machine; it compiles
nothing and links nothing.

### What the bundled build links

The compiled driver links the platform's ODBC **driver manager**, not any one
database's ODBC driver:

| Platform | Driver manager linked | Prerequisite |
|---|---|---|
| Debian / Ubuntu | unixODBC (`libodbc`) | `apt install unixodbc-dev` |
| Fedora / RHEL | unixODBC (`libodbc`) | `dnf install unixODBC-devel` |
| macOS | unixODBC (`libodbc`) | `brew install unixodbc` |
| Windows | `odbc32` | ships with the Windows SDK; nothing to install |

The compile mirrors the project's CMake build: C11 (GNU dialect where the
compiler has one), symbol visibility hidden, optimisation never below `-O2`
even in a debug Cargo profile.

Three environment variables override where the *build* looks for the driver
manager (useful for iODBC or a non-standard prefix):

| Variable | Purpose | Example |
|---|---|---|
| `ADBCBRIDGE_ODBC_LIB` | driver-manager library base name | `iodbc` |
| `ADBCBRIDGE_ODBC_INCLUDE_DIR` | directory holding `sql.h`, `sqlext.h` | `/opt/iodbc/include` |
| `ADBCBRIDGE_ODBC_LIB_DIR` | directory holding the driver-manager library | `/opt/iodbc/lib` |

> **Troubleshooting: the bundled build cannot find the ODBC headers.** If the
> build fails compiling with a message about `sql.h`/`sqlext.h`, install the
> driver-manager development files (`unixodbc-dev` / `unixODBC-devel` /
> `unixodbc` from Homebrew) or point `ADBCBRIDGE_ODBC_INCLUDE_DIR` at them.
> Alternatively build with `--no-default-features` and supply the driver
> library yourself (see [Without the bundled build](#without-the-bundled-build)).

## Adding the dependency

**From crates.io**:

```toml
[dependencies]
adbcbridge = "0.1"
adbc_core = "0.24"
```

**From git** (a pinned tag, if you prefer the repository over the registry):

```toml
[dependencies]
adbcbridge = { git = "https://github.com/singhpratech/adbcbridge", tag = "v0.1.0" }
adbc_core = "0.24"
```

**From a local checkout** (a build tree next to a source checkout is also found
automatically at run time; see the lookup order below):

```toml
[dependencies]
adbcbridge = { path = "../adbcbridge/rust" }
adbc_core = "0.24"
```

**From a release `.crate` file.** Each release publishes the packaged crate as
`target/package/*.crate` on the GitHub Release (the release workflow runs
`cargo package --allow-dirty` and uploads it). Unpack it and depend on it by
path, or add it to a local registry.

You almost always want `adbc_core` alongside `adbcbridge`: it defines the
`Driver`, `Database`, `Connection` and `Statement` traits and the option enums
that the examples below use.

## Locating and loading the library

The crate exposes exactly three functions.

```rust
// Absolute path of the driver library, or an Error explaining what was tried.
pub fn driver_path() -> Result<std::path::PathBuf, adbcbridge::Error>;

// driver_path() + load it into the ADBC driver manager.
pub fn load() -> Result<adbcbridge::ManagedDriver, adbcbridge::Error>;

// Load a library you have already located, skipping the lookup.
pub fn load_from(path: impl AsRef<std::path::Path>)
    -> Result<adbcbridge::ManagedDriver, adbcbridge::Error>;
```

`load` and `load_from` use the entry point `AdbcDriverInit` and request ADBC
version 1.1.0 (`AdbcVersion::V110`; the driver implements both the 1.0.0 and
the 1.1.0 ABI). The result is an ordinary
`adbc_driver_manager::ManagedDriver`.

### Lookup order

`driver_path` returns the first hit from, in order:

1. the `ADBCBRIDGE_LIBRARY` environment variable, then `ADBC_ODBC_DRIVER`
   (either set to something that is **not** a file is an error, not a silent
   miss);
2. the copy compiled into the crate's build directory by the `bundled` feature;
3. the ADBC driver manifest named `odbc` (`odbc.toml`) in the directories the
   ADBC driver manager searches: `ADBC_DRIVER_PATH`, the active `VIRTUAL_ENV` /
   `CONDA_PREFIX`, the per-user directory (`$XDG_CONFIG_HOME/adbc/drivers`, or
   `~/Library/Application Support/ADBC/Drivers` on macOS), and the system ones
   under `/etc` and `/usr`;
4. common install directories (`$VIRTUAL_ENV/lib`, `/usr/local/lib`,
   `/usr/lib`, `/opt/adbcbridge/lib`, `/opt/homebrew/lib`, the Debian multiarch
   `lib` directories) plus a CMake `build/` tree next to a source checkout.

> **Tip.** `ADBC_ODBC_DRIVER` is the same variable the project's Python package,
> benchmarks and C test suite read, so setting it once makes the library
> resolvable from every language.

### Without the bundled build

With `--no-default-features` the crate skips step 2 (there is no compiled copy)
and relies on steps 1, 3 and 4. Install the driver some other way — for example
`cmake --install build` in a checkout of the driver, or unpacking a release
tarball — or set `ADBC_ODBC_DRIVER` to its path.

### Without the crate

`adbcbridge` is a convenience. The driver is a plain C shared library, so the
`adbc_driver_manager` crate can load it directly, with no dependency on
`adbcbridge` at all:

```toml
# Cargo.toml
[dependencies]
adbc_core = "0.24"
adbc_driver_manager = "0.24"
arrow-array = "59"
```

```rust
use adbc_core::options::{AdbcVersion, OptionDatabase};
use adbc_core::{Connection, Database, Driver, Statement};
use adbc_driver_manager::ManagedDriver;

let mut driver = ManagedDriver::load_dynamic_from_filename(
    "/path/to/libadbc_driver_odbc.so",
    Some(b"AdbcDriverInit"),
    AdbcVersion::V110,
)?;

let uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;";
let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
let mut connection = database.new_connection()?;

let mut statement = connection.new_statement()?;
statement.set_sql_query("SELECT * FROM my_table")?;
for batch in statement.execute()? {
    let batch = batch?;               // arrow_array::RecordBatch
    println!("{} rows", batch.num_rows());
}
```

`adbcbridge::load()?` replaces the `load_dynamic_from_filename` call — it is
exactly what `load_from` does once `driver_path` has found the library. The
entry point is passed explicitly; the library also exports
`AdbcDriverOdbcInit`, the name the driver manager derives from the file name
when no entry point is given, and either works. The
[`tests/rust/`](../../tests/rust/) crate is a runnable example built this
way: a standalone smoke test that depends only on published crates, plus
`examples/readme_snippet.rs`, which holds the snippet above so `cargo test`
fails if it ever stops compiling.

## Connection strings

adbcBridge passes the ODBC connection string through as the ADBC **`uri`**
option (`OptionDatabase::Uri`). Write it exactly as unixODBC or the Windows
driver manager would take it — a `Driver=…` string or a `DSN=…` string:

```text
Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;
Driver={PostgreSQL Unicode};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;
DSN=mydsn;
```

Real connection strings the compatibility suite uses (`tests/compat/test_matrix.py`):

| Database | Connection string |
|---|---|
| SQLite | `Driver=<libsqlite3odbc.so>;Database=<file>;` |
| PostgreSQL | `Driver=<psqlodbcw.so>;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| SQL Server | `Driver=<msodbcsql>;Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=…;TrustServerCertificate=yes;` |
| Oracle | `Driver=<oracle>;DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;` |

## Options

Set these on the **database** (via `new_database_with_opts`) unless the table
says otherwise. Connection strings and identity go under the standard ADBC keys;
driver-specific tuning goes under `adbc.odbc.*`.

| Key | Meaning |
|---|---|
| `uri` (`OptionDatabase::Uri`) | full ODBC connection string (`Driver=…;Server=…;`) |
| `dsn` | DSN name from `odbc.ini` (appended as `DSN=…`) |
| `username`, `password` | appended as `UID=` / `PWD=` |
| `adbc.odbc.batch_size` | rows per Arrow batch (default 1024) |
| `adbc.odbc.max_bind_bytes` | widest value bound at the width the driver declares for it, in bytes (default 32768); wider ones are bound at `long_bind_bytes` or read with `SQLGetData` |
| `adbc.odbc.long_bind_bytes` | width, in bytes, to bind a column whose declared width is not a real bound — a `TEXT`/`NVARCHAR(MAX)`/`LONGTEXT`/`bytea` column (default 2048). Trades only speed: longer values are re-read in full. Where too many do not fit, the reader stops binding that column and uses `SQLGetData` instead |
| `adbc.odbc.rowset_bytes` | ceiling on a reader's bound rowset buffers, in bytes (default 8388608) |
| `adbc.odbc.decimal_as_string` | `true` to return DECIMAL/NUMERIC as strings |
| `adbc.odbc.partitions` | how many partitions `ExecutePartitions` splits a query into — `0` (default) chooses from the table's size, `1` never splits. Set on the **statement** |
| `adbc.odbc.prefetch` | rowsets kept in flight on a background fetch thread — `0` (default) off, `1` double-buffering, up to `8`. Settable on the database, connection or statement. *(Compiled out on Windows; see [Known limitations](#known-limitations).)* |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — native delegation (see below) |
| `adbc.odbc.delegate.driver` | force a specific native driver: a bare name (`postgresql`) or manifest name; a path only with `allow_paths` |
| `adbc.odbc.delegate.search_path` | extra directories to search for native drivers (`:`-separated); needs `allow_paths` |
| `adbc.odbc.delegate.allow_paths` | `true` to let the two options above name filesystem paths (default `false`) |
| `adbc.odbc.delegate.last_error` | read-only: why delegation did not happen |
| `adbc.odbc.delegated_to` | read-only: the native driver serving this database/connection, or `odbc` |
| `adbc.odbc.tune` | `true` (default) / `false` — may the driver add ODBC connection keywords of its own where it recognises the target driver? `false` sends your connection string through untouched |
| `adbc.odbc.sqllen_32bit` | `true`/`false` to force the 32-bit-`SQLLEN` driver quirk (autodetected for IBM Db2; you normally never set it). Also settable on the connection and statement |
| `adbc.odbc.rows_per_insert` | rows of parameters per `INSERT` for **bulk ingest** — `0` (default) picks a group size automatically, `1` turns the rewrite off, any other value asks for that many. Instead of executing `INSERT INTO t VALUES (?,?)` once per row, ingest prepares `INSERT INTO t VALUES (?,?),(?,?),…` with K row-groups and binds K rows' worth of ordinary parameters per execute, which divides the round trips by K. Against PostgreSQL, where a batch instead goes as one array parameter per column, the same value sets the rows one such statement carries (default 10,000) and `1` turns that off too. See [Bulk ingest](#bulk-ingest). Set on the **statement** |
| `adbc.odbc.ingest_connections` | connections a **bulk ingest** may spread itself over — `1` (default) keeps it on the caller's own connection in a single transaction. `N > 1` (up to `64`) opens `N` further connections, hands each a share of the bound stream's batches and lets each run the ordinary ingest path into the same table. **This trades atomicity for speed**: `N` connections are `N` transactions, so a failure can leave some batches committed. See [Parallel ingest](../how-it-works/performance.md#parallel-ingest-trading-atomicity-for-speed). Not in the Windows build (the fan-out is pthreads, compiled out on `_WIN32` — see the [roadmap](../ROADMAP.md) and [Known limitations](#known-limitations)); ingest stays on one connection there. Set on the **statement** |
| `adbc.odbc.array_binding` | `true` (default) binds each Arrow batch as a column-wise ODBC parameter array, so a multi-row bind (and ingest on a driver where arrays are the faster of the two) issues one `SQLExecute` per batch instead of one per row; `false` forces row-at-a-time. Drivers that do not honour `SQL_ATTR_PARAMSET_SIZE`, or that cannot account for every parameter set they were handed, fall back automatically; drivers whose parameter arrays silently drop or misapply values (DuckDB, clickhouse-odbc, MonetDB, Firebird's OdbcFb, Virtuoso, Apache Ignite, QuestDB via psqlodbc — quirk `no_param_arrays`) default to `false` and can be forced back on with this option. Reported rows-affected is identical in both modes. Set on the **statement** |

### Native delegation

Where a native ADBC driver exists for the target (PostgreSQL, SQLite, DuckDB,
Snowflake, BigQuery, Flight SQL), adbcBridge can hand the whole database over to
it for native speed from the same install. `adbc.odbc.delegate=auto` (the
default) delegates when it recognises the target; `never` keeps adbcBridge on
its own ODBC path; `always` requires delegation. Set `adbc.odbc.delegate=never`
when you specifically want to exercise the ODBC path.

## A first query: `SELECT 1`

```rust
use adbc_core::options::OptionDatabase;
use adbc_core::{Connection, Database, Driver, Statement};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut driver = adbcbridge::load()?;

    let uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=:memory:;";
    let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
    let mut connection = database.new_connection()?;
    let mut statement = connection.new_statement()?;

    statement.set_sql_query("SELECT 1 AS one")?;
    for batch in statement.execute()? {
        let batch = batch?;
        println!("{} row(s)", batch.num_rows());
    }
    Ok(())
}
```

`Statement::execute` returns a `RecordBatchReader` — an iterator that yields
`Result<RecordBatch>` (both from the [`arrow-array`](https://crates.io/crates/arrow-array)
crate — add `arrow-array = "59"` to your dependencies, as the examples do;
`adbc_core` does not re-export it). Iterate it to stream Arrow batches;
collect it if you want the whole result at once.

## Reading into Arrow

```rust
use arrow_array::RecordBatch;

statement.set_sql_query("SELECT id, val, txt FROM my_table")?;
let reader = statement.execute()?;                 // impl RecordBatchReader

// Stream batch by batch:
let mut total = 0usize;
for batch in reader {
    let batch: RecordBatch = batch?;
    total += batch.num_rows();
}

// Or collect all of it:
// let batches: Vec<RecordBatch> = statement.execute()?.collect::<Result<_, _>>()?;
```

The reader's `schema()` gives the Arrow schema before you pull any rows.

## Parameters

Bind an Arrow `RecordBatch` of parameter values with `Statement::bind` (or a
whole stream with `bind_stream`), then execute. The batch has one column per
`?` and each row of the batch is one execution:

```rust
statement.set_sql_query("INSERT INTO t (a, b) VALUES (?, ?)")?;
statement.prepare()?;                 // optional; execute prepares if you do not
statement.bind(params_batch)?;        // one RecordBatch, its rows = parameter sets
statement.execute_update()?;          // returns rows affected
```

`execute_update` runs a statement for its side effect and returns the affected
row count; `execute` runs it for its result set. A query you wrote is executed
as written, whatever is bound to it — only the `INSERT` that bulk ingest
generates is ever rewritten (see [Bulk ingest](#bulk-ingest)).

A multi-row bind and bulk ingest also batch their commits: when the connection
is in autocommit and more than one row is bound, adbcBridge turns autocommit
off for the duration and commits once at the end (rolling back if the execute
fails), instead of paying a commit per row. A transaction the caller opened
themselves is left alone, and a driver that refuses to leave autocommit is
simply left autocommitting — slower, but correct.

## Bulk ingest

Bulk ingest writes an Arrow batch (or stream) into a table, generating the DDL
and batching the rows. Set the target table and ingest mode as statement
options, bind the data, and run `execute_update`:

```rust
use adbc_core::options::{IngestMode, OptionStatement};

let mut statement = connection.new_statement()?;
statement.set_option(OptionStatement::TargetTable, "my_table".into())?;
statement.set_option(OptionStatement::IngestMode, IngestMode::Create.into())?;
statement.bind(batch)?;               // the RecordBatch to ingest
statement.execute_update()?;
connection.commit()?;                 // if autocommit is off
```

Under the hood adbcBridge builds the `INSERT` itself, so it can pack K rows
into one statement — `INSERT INTO t ("a", "b") VALUES (?, ?), (?, ?), …` with K
row-groups — rather than executing one statement per row. K rows' worth of
scalar parameters go in per `SQLExecute`, over the same `SQLBindParameter` calls
and the same per-driver parameter handling as one row at a time; no parameter
arrays are involved, so it works on every driver that can bind ordinary
parameters, including those whose parameter arrays are unusable (DuckDB,
MonetDB, clickhouse-odbc, QuestDB via psqlodbc) and those where an array is no
cheaper than a loop (MySQL Connector/ODBC). It is the default for ingest;
parameter arrays are kept ahead of it only for MariaDB Connector/ODBC before
3.2, whose arrays go out as a single `COM_STMT_BULK_EXECUTE` (3.2 and later is
switched to the multi-row form, because its arrays segfault on a NULL date and
misreport row counts against MySQL), and for Vertica's own driver, whose arrays
become a native bulk load. On PostgreSQL adbcBridge goes a step
further and sends each column of a batch as a single array parameter, with the
multi-row form as the fallback — see [PostgreSQL: one array parameter per
column](../how-it-works/performance.md#postgresql-one-array-parameter-per-column).

K is chosen from a parameter budget (2000 parameters, at most 1000 row-groups —
SQL Server's limits are 2100 and 1000) and clipped by the driver's
`SQL_MAX_STATEMENT_LEN`. ODBC has no "maximum parameters" question to ask, so
the real ceiling is *probed*: a `SQLPrepare` (or, for a driver that only objects
later, the first `SQLExecute`) that is refused halves K and asks again, and the
answer is remembered on the connection. A server with no multi-row `VALUES` at
all is found the same way, by a two-row `SQLPrepare` before anything is written,
and ingest carries on in whatever form the server does take (Oracle's
`INSERT ALL`, Firebird's typed `UNION ALL SELECT`, SQLite's 999-variable build,
Cloud Spanner's declared 950-parameter ceiling); the per-server table is in
[Bulk ingest](../how-it-works/performance.md#bulk-ingest). A failure part way
through is unchanged by any of this: the whole ingest is one transaction, so it
commits completely or leaves nothing behind. Tune with
`adbc.odbc.rows_per_insert`, `adbc.odbc.array_binding` and (trading atomicity
for speed) `adbc.odbc.ingest_connections`; see the [options table](#options).

To turn autocommit off first:

```rust
use adbc_core::options::OptionConnection;
connection.set_option(OptionConnection::AutoCommit, "false".into())?;
```

## Metadata

adbcBridge implements the standard ADBC metadata calls on the `adbc_core`
`Connection` trait — `get_info`, `get_objects`, `get_table_types` and
`get_table_schema` — backed by ODBC catalog functions and returning Arrow. Use
them exactly as you would with any ADBC driver; adbcBridge adds nothing of its
own here.

## Errors

Failures from `driver_path`/`load` are an `adbcbridge::Error`
(`#[non_exhaustive]`):

| Variant | Meaning |
|---|---|
| `BadEnvironment { variable, value }` | `ADBCBRIDGE_LIBRARY` or `ADBC_ODBC_DRIVER` is set but does not name a file |
| `NotFound { searched }` | no driver library was found; `searched` lists every path tried, and the `Display` text repeats them |
| `Adbc(adbc_core::error::Error)` | the library was found but the ADBC driver manager could not load it |

Once loaded, query and connection failures surface as
`adbc_core::error::Error`, which carries the structured ODBC diagnostics
(SQLSTATE plus the native error code) that adbcBridge maps from the driver.

## Comparison with `odbc-api` / `arrow-odbc`

The repository's Rust benchmark (`bench/rust/`) measures the same
ingest-and-fetch workload three ways over the *same* ODBC driver: through
adbcBridge (the ADBC path), through [`odbc-api`](https://crates.io/crates/odbc-api)
into a column-wise rowset buffer (no Arrow), and through
[`arrow-odbc`](https://crates.io/crates/arrow-odbc) into Arrow `RecordBatch`es.
The harness pins `arrow-odbc = "25"`, `odbc-api = "29"` and
`arrow-array`/`arrow-schema = "59"`.

This is a measured comparison, not a claim: the per-database numbers live in
[`bench/RUST_BENCHMARKS.md`](../../bench/RUST_BENCHMARKS.md) and the cross-language
tables in [`bench/LANGUAGE_BENCHMARKS.md`](../../bench/LANGUAGE_BENCHMARKS.md).
One property the harness itself documents: `odbc-api` and `arrow-odbc` both ask
the caller to cap text-column width (some drivers describe a `VARCHAR(20)` as
holding 65,536 or 2,147,483,647 characters), and the benchmark caps both sides
the same way that adbcBridge's `adbc.odbc.max_bind_bytes` does, so the reads are
bounded identically.

## Known limitations

- **Early release (0.1.0).** The API surface is small and may change. The
  ADBC Driver Foundry validation suite runs against the driver from
  `tests/validation/` (latest results in `tests/validation/RESULTS.md`; driver
  defects it found are still being worked through); prebuilt driver tarballs
  ship on GitHub Releases and the crate is on crates.io.
- **Windows: no prefetch, no parallel ingest.** The prefetch pipeline
  (`adbc.odbc.prefetch`) and the ingest fan-out (`adbc.odbc.ingest_connections`)
  both use POSIX threads and are compiled out on Windows. On Windows those
  options have no effect; queries and single-connection ingest work normally.
- **Windows: a driver that throws a C++ exception across the FFI boundary
  aborts the process.** This is a measured finding
  ([`bench/LANGUAGE_BENCHMARKS-windows.md`](../../bench/LANGUAGE_BENCHMARKS-windows.md)):
  the DuckDB ODBC driver throws a C++ exception out of `SQLExecute`, and Rust
  cannot unwind a foreign exception, so the process ends with `fatal runtime
  error: Rust cannot catch foreign exceptions, aborting`. This is a property of
  that ODBC driver crossing the FFI boundary, not of adbcBridge, and it is not
  something the crate can catch; it was seen only with the DuckDB driver on
  Windows in that campaign. Other drivers on Windows were unaffected.
- **You still need an ODBC driver for your database.** adbcBridge is the bridge,
  not the database driver: the driver manager still has to find the ODBC driver
  your `Driver=…`/`DSN=…` names.

## Complete worked example

A self-contained program that loads adbcBridge, opens SQLite in memory over
ODBC, creates a table, ingests three rows from an Arrow batch, and reads them
back — all through the ADBC traits.

`Cargo.toml`:

```toml
[package]
name = "adbcbridge-example"
version = "0.1.0"
edition = "2021"

[dependencies]
adbcbridge = "0.1"
adbc_core = "0.24"
arrow-array = "59"
arrow-schema = "59"
```

`src/main.rs`:

```rust
use std::sync::Arc;

use adbc_core::options::{IngestMode, OptionConnection, OptionDatabase, OptionStatement};
use adbc_core::{Connection, Database, Driver, Statement};
use arrow_array::{Int32Array, RecordBatch, StringArray};
use arrow_schema::{DataType, Field, Schema};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 1. Find and load the driver.
    let mut driver = adbcbridge::load()?;

    // 2. Open a connection. adbc.odbc.delegate=never keeps us on the ODBC path
    //    even if a native ADBC SQLite driver happens to be installed.
    let uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=:memory:;";
    let database = driver.new_database_with_opts([
        (OptionDatabase::Uri, uri.into()),
        (OptionDatabase::Other("adbc.odbc.delegate".into()), "never".into()),
    ])?;
    let mut connection = database.new_connection()?;
    connection.set_option(OptionConnection::AutoCommit, "false".into())?;

    // 3. Build a small Arrow batch.
    let schema = Arc::new(Schema::new(vec![
        Field::new("id", DataType::Int32, false),
        Field::new("name", DataType::Utf8, false),
    ]));
    let batch = RecordBatch::try_new(
        schema,
        vec![
            Arc::new(Int32Array::from(vec![1, 2, 3])),
            Arc::new(StringArray::from(vec!["ada", "grace", "linus"])),
        ],
    )?;

    // 4. Bulk-ingest it into a new table.
    let mut ingest = connection.new_statement()?;
    ingest.set_option(OptionStatement::TargetTable, "people".into())?;
    ingest.set_option(OptionStatement::IngestMode, IngestMode::Create.into())?;
    ingest.bind(batch)?;
    ingest.execute_update()?;
    drop(ingest);
    connection.commit()?;

    // 5. Read the rows back into Arrow.
    let mut query = connection.new_statement()?;
    query.set_sql_query("SELECT id, name FROM people ORDER BY id")?;
    let mut rows = 0usize;
    for batch in query.execute()? {
        rows += batch?.num_rows();
    }
    println!("read {rows} rows back");
    Ok(())
}
```

> **Tip.** If `load()` returns `NotFound`, set `ADBC_ODBC_DRIVER` to the path of
> `libadbc_driver_odbc.{so,dylib,dll}`, or build with the default `bundled`
> feature so the driver is compiled into the crate. The error's message lists
> every location it checked.
