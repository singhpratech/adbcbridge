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

Planned: conformance suite, prebuilt binaries.

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
| Firebird 5 | Firebird ODBC 3.5.0-rc1 | PASS (driver quirk handled: `SQL_C_WCHAR` sized in 4-byte `wchar_t`) |

Servers for the matrix: `docker compose -f tests/compat/docker-compose.yml up -d`.
Per-database driver setup (root-free) and run commands: [`tests/compat/README.md`](tests/compat/README.md).

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

The same smoke tests from Rust (see `tests/rust/README.md`):

```sh
cd tests/rust && SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so cargo test
```
## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build/test loop, the
`clang-format` + `pre-commit` setup, and what to include in a bug report.

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
