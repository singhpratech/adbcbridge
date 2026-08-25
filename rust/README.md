# adbcBridge for Rust

Find and load [adbcBridge](https://github.com/singhpratech/adbcbridge) -- the
ADBC driver for any ODBC data source -- through the `adbc_driver_manager`
crate.

```toml
[dependencies]
adbcbridge = "0.1"
adbc_core = "0.24"
```

The crate has two functions:

* `adbcbridge::driver_path() -> Result<PathBuf, Error>` returns the absolute
  path of `libadbc_driver_odbc` (`.so`/`.dylib`/`.dll`).  Lookup order:
  the `ADBCBRIDGE_LIBRARY` then `ADBC_ODBC_DRIVER` environment variables; the
  copy compiled by the `bundled` feature; the ADBC driver manifest named
  `odbc` (`odbc.toml`) in the driver manager's search directories; and finally
  the usual install directories (`$VIRTUAL_ENV/lib`, `/usr/local/lib`,
  `/usr/lib`, ...) plus a CMake `build/` tree next to a source checkout.
* `adbcbridge::load() -> Result<ManagedDriver, Error>` loads that library into
  the ADBC driver manager (entry point `AdbcDriverInit`, ADBC 1.1.0).  The
  result is an ordinary `adbc_driver_manager::ManagedDriver`; the ODBC
  connection string is passed as the ADBC `uri` option.  `load_from(path)`
  does the same for a library you have already located.

```rust
use adbc_core::options::OptionDatabase;
use adbc_core::{Connection, Database, Driver, Statement};

let mut driver = adbcbridge::load()?;
let uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=:memory:;";
let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
let mut connection = database.new_connection()?;
let mut statement = connection.new_statement()?;
statement.set_sql_query("SELECT 1 AS one")?;
for batch in statement.execute()? {
    println!("{} row(s)", batch?.num_rows());
}
```

## The `bundled` feature (default)

With `bundled` on, `build.rs` compiles the driver from the C sources shipped
in the crate (`csrc/`, a verbatim copy of the repository's `src/`, `include/`
and `vendor/nanoarrow/`) and links it against the ODBC driver manager, so the
only prerequisites are a C compiler and the ODBC driver-manager development
files:

* Debian/Ubuntu: `apt install unixodbc-dev`
* Fedora/RHEL: `dnf install unixODBC-devel`
* macOS: `brew install unixodbc`
* Windows: `odbc32` from the Windows SDK, nothing to install.

`ADBCBRIDGE_ODBC_LIB` (for example `iodbc`), `ADBCBRIDGE_ODBC_INCLUDE_DIR` and
`ADBCBRIDGE_ODBC_LIB_DIR` override where the build looks for the driver
manager.

`cargo add adbcbridge --no-default-features` gives the loader only: you then
supply `libadbc_driver_odbc` yourself (set `ADBC_ODBC_DRIVER`, or
`cmake --install` the driver) and `driver_path()` finds it as described above.

## Developing

`csrc/` is refreshed from the repository root with `./sync-csrc.sh`; the
`csrc_in_sync` test fails when the copy has drifted.  The `sqlite` test runs
`SELECT 1` through the SQLite ODBC driver named by `SQLITE_ODBC_DRIVER` and
is skipped when that variable is unset.
