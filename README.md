# adbc-odbc

**An [ADBC](https://arrow.apache.org/adbc/) driver for any ODBC data source.**
Written in plain C11 — one shared library that turns every ODBC driver on your
machine into an Arrow-native ADBC driver, usable from Python, R, Go, Rust, C++,
C#, Java and anything else that speaks the ADBC driver manager.

```
Python / R / Go / Rust / Java / C#
        │  ADBC driver manager
        ▼
 libadbc_driver_odbc.so   ← this project
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
- `GetInfo`, `GetTableTypes`, `GetTableSchema`, structured ODBC errors (SQLSTATE + native code)
- ADBC 1.0.0 and 1.1.0 ABI

Planned: `GetObjects` (SQLTables/SQLColumns), parameter binding, bulk ingest via
`SQLBindParameter` arrays, driver manifest for `adbc_driver_manager` discovery,
conformance suite, Windows/macOS builds.

## Build

```sh
sudo apt install unixodbc-dev cmake        # Debian/Ubuntu
cmake -S . -B build && cmake --build build
# -> build/libadbc_driver_odbc.so
```

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

## Test

```sh
python -m venv .venv && .venv/bin/pip install adbc-driver-manager pyarrow
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python tests/test_sqlite.py
```

The same smoke tests from Rust (see `tests/rust/README.md`):

```sh
cd tests/rust && SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so cargo test
```

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
