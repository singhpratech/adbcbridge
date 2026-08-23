# adbcbridge

**An [ADBC](https://arrow.apache.org/adbc/) driver for any ODBC data source.**
Written in plain C11 — one shared library that turns every ODBC driver on your
machine into an Arrow-native ADBC driver, usable from Python, R, Go, Rust, C++,
C#, Java and anything else that speaks the ADBC driver manager.

```
Python / R / Go / Rust / Java / C#
        │  ADBC driver manager
        ▼
 libadbc_driver_odbc.so   ← adbcbridge
        │  ODBC API (unixODBC / iODBC / Windows DM)
        ▼
 Db2 · Oracle · Teradata · SQL Server · Vertica · SAP HANA · Informix · Access ·
 Snowflake · Redshift · SQLite · anything with an ODBC driver
```

## Status

Early (0.1.0). Working today:

- `SELECT` → Arrow record batches (block fetch, column-wise binding)
- Types: bool, int8–64 (+unsigned), float/double, char/varchar/nvarchar (UTF-16 → UTF-8),
  binary, date, time, timestamp (µs/ns), decimal → `decimal128`
- Long/unbounded columns (LONGVARCHAR/BLOB) via chunked `SQLGetData`
- DML with `rows_affected`, prepared statements, autocommit / commit / rollback
- `GetInfo`, `GetObjects`, `GetTableTypes`, `GetTableSchema`, structured ODBC errors
  (SQLSTATE + native code)
- Parameter binding (`Bind`/`BindStream`) and bulk ingest (`adbc.ingest.*`)
- ADBC 1.0.0 and 1.1.0 ABI, discoverable by name through an ADBC driver manifest
- [Native delegation](#native-delegation): where a native ADBC driver exists
  (PostgreSQL, SQLite, DuckDB, …) the whole driver is handed over to it, so you
  get native speed from the same install

Planned: conformance suite, prebuilt binaries.

## Performance

1,000,000 rows `(int, double, varchar(20), date)` from SQLite, median of 5 (`bench/BENCHMARKS.md`):

| Path | Time | Relative |
|---|---:|---:|
| **adbcbridge `fetch_arrow_table()`** | **0.48 s** | 1.0× |
| pyodbc `fetchall()` → `pyarrow.Table` | 1.16 s | 2.4× slower |
| pyodbc `fetchall()` → `pandas.DataFrame` | 1.32 s | 2.8× slower |
| raw `SQLBindCol`+`SQLFetch`, no Arrow (floor) | 0.44 s | 0.93× |

The bridge runs within 7% of the raw ODBC floor; the remaining cost is the ODBC driver itself.

## Compatibility matrix

Same workload (types, NULLs, Unicode incl. emoji, parameters, bulk ingest, batched
reads, GetObjects, error mapping) run through `tests/compat/test_matrix.py`:

| Database | ODBC driver | Status |
|---|---|---|
| SQLite 3.45 | sqliteodbc 0.99991 | PASS |
| DuckDB (latest) | duckdb-odbc | PASS (driver quirks handled: 2048-row vectors, no `SQL_BIT` params) |
| PostgreSQL 16 | psqlodbc 16 | PASS |
| MariaDB 11 / MySQL | MariaDB Connector/ODBC 3.1 | PASS |
| SQL Server 2022 | msodbcsql 18 | PASS (incl. `NVARCHAR(MAX)` via chunked `SQLGetData`) |
| Oracle 23ai Free | Instant Client ODBC 23 | PASS (set `NLS_LANG=.AL32UTF8` for non-ASCII; 64-bit ints sent as numeric text — driver lacks `SQL_C_SBIGINT`) |
| ClickHouse 26 | clickhouse-odbc 1.5 | PASS (NULL params need `SQLDescribeParam`; no affected-row counts; `Nullable()` DDL wrapper on ingest) |
| MySQL 8.4 | MySQL Connector/ODBC 9.4 (and MariaDB Connector/ODBC 3.1) | PASS |

Servers for the matrix: `docker compose -f tests/compat/docker-compose.yml up -d`.
Per-database driver setup and the exact commands are in
[`tests/compat/README.md`](tests/compat/README.md).

## Build

```sh
sudo apt install unixodbc-dev cmake        # Debian/Ubuntu
brew install unixodbc cmake                # macOS
# Windows: the ODBC driver manager ships with the OS
cmake -S . -B build && cmake --build build
# -> build/libadbc_driver_odbc.so
```

## Install

```sh
cmake --install build --prefix /usr/local
```

This installs two things:

- `<prefix>/lib/libadbc_driver_odbc.so` (`bin\libadbc_driver_odbc.dll` on Windows)
- `<prefix>/etc/adbc/drivers/odbc.toml` — an
  [ADBC driver manifest](https://arrow.apache.org/adbc/current/format/driver_manifests.html)
  pointing at the installed library

The manifest is what lets applications ask for the driver by name instead of by
path (see below). Pass `-DADBCBRIDGE_INSTALL_MANIFEST=OFF` to skip it, or
`-DADBCBRIDGE_MANIFEST_DIR=<dir>` to install it elsewhere — relative to the
install prefix (`share/adbc/drivers`) or absolute (`/etc/adbc/drivers`).

The absolute library path inside the manifest is computed while
`cmake --install` runs, not at configure time. A single build tree can
therefore be installed into as many prefixes as you like — `/usr/local`,
`"$VIRTUAL_ENV"`, a packaging staging root via `DESTDIR=` — and every installed
manifest points at its own copy of the library.

## Use by name (driver manifest)

With the manifest installed somewhere the ADBC driver manager searches, every
binding can load adbcbridge as simply `odbc`:

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="odbc",   # resolved via <prefix>/etc/adbc/drivers/odbc.toml
    db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"},
)
```

The driver manager looks for `odbc.toml` in, among others:

| location | how to use it |
|---|---|
| `$ADBC_DRIVER_PATH` | colon-separated list of directories (`;`-separated on Windows) |
| `<sys.prefix>/etc/adbc/drivers` | added by the Python driver manager inside a virtualenv: `cmake --install build --prefix "$VIRTUAL_ENV"` |
| `~/.config/adbc/drivers` | per-user install: `-DADBCBRIDGE_MANIFEST_DIR="$HOME/.config/adbc/drivers"` (`$XDG_CONFIG_HOME/adbc/drivers` if set) |
| `/etc/adbc/drivers` | system-wide install: configure with `-DADBCBRIDGE_MANIFEST_DIR=/etc/adbc/drivers`, then `cmake --install build --prefix /usr` |

On macOS the user/system directories are `~/Library/Application Support/ADBC/Drivers`
and `/Library/Application Support/ADBC/Drivers`; on Windows the driver manager
also reads `HKEY_CURRENT_USER\SOFTWARE\ADBC\Drivers` and the machine-wide
equivalent.

Loading by path keeps working, and is what you want for a build tree:
`driver="/path/to/libadbc_driver_odbc.so"`.

If `driver="odbc"` fails with

```
dlsym(AdbcDriverInit) failed: .../libodbc.so: undefined symbol: AdbcDriverInit
```

then no manifest was found, and the driver manager fell back to loading a plain
shared library named `odbc` — which on Unix is unixODBC's own driver manager,
not this driver. Check that the directory holding `odbc.toml` is one of the
locations above, and that the path recorded inside `odbc.toml` exists.

## Use (Python)

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="/path/to/libadbc_driver_odbc.so",
    db_kwargs={"uri": "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;"},
)
with conn.cursor() as cur:
    cur.execute("SELECT * FROM my_table")
    table = cur.fetch_arrow_table()
```

Options (set on the database):

| key | meaning |
|---|---|
| `uri` | full ODBC connection string (`Driver=...;Server=...;`) |
| `dsn` | DSN name from `odbc.ini` (appended as `DSN=...`) |
| `username`, `password` | appended as `UID=`/`PWD=` |
| `adbc.odbc.batch_size` | rows per Arrow batch (default 1024) |
| `adbc.odbc.max_bind_bytes` | max bound buffer per value before falling back to `SQLGetData` (default 32768) |
| `adbc.odbc.decimal_as_string` | `true` to return DECIMAL/NUMERIC as strings |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — see [Native delegation](#native-delegation) |
| `adbc.odbc.delegate.driver` | force a specific native driver (name, manifest, or path) |
| `adbc.odbc.delegate.search_path` | extra directories to search for native drivers (`:`-separated) |
| `adbc.odbc.delegate.last_error` | read-only: why delegation did not happen |
| `adbc.odbc.delegated_to` | read-only: `odbc` when this driver is serving the connection |

## Native delegation

Native speed where a native ADBC driver exists, ODBC everywhere else, one
install.

Some databases already have a first-class ADBC driver: PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL. Those drivers talk the wire protocol
and build Arrow directly, so they are faster than anything that has to go
through ODBC's row-oriented API — 1,000,000 PostgreSQL rows take 0.42 s through
`adbc_driver_postgresql` and 1.00 s through adbcbridge over psqlodbc.

So adbcbridge gets out of the way. When `AdbcDatabaseInit` recognizes a target
that a native driver handles, it loads that driver and hands the entire ADBC
driver over to it: connections, statements, result sets and errors all come
from the native driver, and adbcbridge is not in the data path at all.
Delegated fetches measure the same as calling the native driver directly.

| target | delegated to |
|---|---|
| `uri=postgresql://…` / `postgres://…` | `postgresql` |
| `uri=sqlite:…`, `duckdb:…` | `sqlite`, `duckdb` |
| `uri=snowflake://…`, `bigquery://…` | `snowflake`, `bigquery` |
| `uri=grpc://…`, `grpc+tls://…` | `flightsql` |
| `uri=Driver=…psqlodbcw.so;Server=…` | `postgresql` (URI rebuilt from `Server`/`Port`/`Database`/`Uid`/`Pwd`) |
| `uri=Driver=…sqlite3odbc.so;Database=…` | `sqlite` (the `Database=` path) |
| `dsn=…` | whatever the DSN's `Driver=` in `odbc.ini` maps to |
| anything else (Db2, Oracle, SQL Server, Teradata, …) | nobody — ODBC, as before |

Native drivers are resolved through the ADBC driver manager's own loader, so
manifests (`<name>.toml`), `ADBC_DRIVER_PATH` and installed packages all work;
Python wheel layouts (`site-packages/adbc_driver_postgresql/`) are searched too,
and `adbc.odbc.delegate.search_path` adds directories of your own. adbcbridge
never links against the driver manager — it resolves `AdbcLoadDriver` from
whichever manager already loaded it.

Delegation is a best-effort optimization. If no native driver is installed, if
it cannot be loaded, or if it rejects the target, `auto` falls back to ODBC
silently and records why in `adbc.odbc.delegate.last_error`. It also needs the
ADBC driver manager: a program that `dlopen`s adbcbridge itself always gets the
ODBC path.

```python
# Off, for this database:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"})
# Off, for a whole deployment:
#   export ADBC_ODBC_DELEGATE=never   (ADBC_ODBC_DELEGATE_PATH adds search directories)
# Required, so that a missing native driver is an error instead of a slow path:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "always"})
```

Who served a connection is visible through `adbc_get_info()["driver_name"]`
(`ADBC PostgreSQL Driver` vs `ADBC ODBC Driver (psqlodbcw.so)`). The option
`adbc.odbc.delegated_to` answers `odbc` on the ODBC path; once a native driver
has taken over, the option is the native driver's business and it will not know
the key.

Database options are translated for the native driver: `uri` (rebuilt from the
ODBC keywords when needed), `username`/`password`, and any `adbc.*` option that
is not `adbc.odbc.*` is passed through untouched. ODBC-specific options
(`adbc.odbc.batch_size`, …) are meaningless to a native driver and are dropped.

## Use from Rust

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

Parameters are bound as a `RecordBatch`, one column per `?` and one row per
execution: `statement.prepare()?; statement.bind(params)?;` then `execute()` or
`execute_update()`. See `tests/rust/` for a runnable example.

Options (set on the statement):

| key | meaning |
|---|---|
| `adbc.odbc.array_binding` | `false` (default; opt-in while it is verified across the compatibility matrix) — `true` binds each Arrow batch as an ODBC parameter array, so bulk ingest and `executemany` issue one `SQLExecute` per batch instead of one per row; `false` forces row-at-a-time. Drivers that do not honour `SQL_ATTR_PARAMSET_SIZE` fall back automatically. Reported rows-affected is identical in both modes. |

## Test

```sh
python -m venv .venv && .venv/bin/pip install adbc-driver-manager pyarrow
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python tests/test_sqlite.py
```

Native delegation (each case skips when its native driver, ODBC driver or server
is missing):

```sh
.venv/bin/pip install adbc-driver-postgresql adbc-driver-sqlite
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python tests/test_delegate.py
```

The same smoke tests from Rust (see `tests/rust/README.md`):

```sh
cd tests/rust && SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so cargo test
```
## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build/test loop, the
`clang-format` + `pre-commit` setup, and what to include in a bug report.

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
