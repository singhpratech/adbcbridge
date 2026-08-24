# adbcBridge (Python)

Thin Python wrapper around [adbcBridge](https://adbcbridge.org), the ADBC
driver for any ODBC data source. It finds the `libadbc_driver_odbc` shared
library and hands it to `adbc_driver_manager`, so every ODBC driver on the
machine becomes an Arrow-native ADBC database.

```sh
pip install adbcbridge
```

```python
import adbcbridge

with adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;") as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM t")
        table = cur.fetch_arrow_table()   # pyarrow.Table
```

`connect()` returns a plain `adbc_driver_manager.dbapi.Connection`, so the whole
ADBC DBAPI surface (`cursor()`, `fetch_arrow_table()`, `adbc_ingest()`,
`adbc_get_objects()`, …) is available.

## API

```python
adbcbridge.connect(uri=None, dsn=None, username=None, password=None,
                   driver_path=None, *, autocommit=False, conn_kwargs=None,
                   **options) -> adbc_driver_manager.dbapi.Connection
adbcbridge.driver_path() -> str            # path of libadbc_driver_odbc.so
adbcbridge.odbc_drivers() -> list[OdbcDriver]   # from odbcinst.ini
adbcbridge.odbcinst_ini() -> pathlib.Path | None
adbcbridge.odbc_driver_library(driver=None, *, uri=None, dsn=None) -> str | None
adbcbridge.preload_odbc_driver(driver=None, *, uri=None, dsn=None,
                               strict=False) -> str | None
```

`import adbcbridge` does not import pyarrow: `adbc_driver_manager.dbapi` is
imported inside `connect()`, after the ODBC driver named in the connection
string has been opened. A few ODBC drivers — MySQL Connector/ODBC is the one in
the wild — can only be loaded while libstdc++ has not yet been pinned to dynamic
thread-local storage, which importing pyarrow does. `preload_odbc_driver()` is
that step on its own, for programs that drive `adbc_driver_manager` or pyodbc
themselves; `ADBCBRIDGE_PRELOAD=0` disables the automatic one in `connect()`.
See [`docs/TROUBLESHOOTING.md`](../docs/TROUBLESHOOTING.md).

Extra `**options` become database options: a bare name is prefixed with
`adbc.odbc.` (`batch_size=4096` → `adbc.odbc.batch_size`), a dotted name is
passed through as given, and `True`/`False` become `"true"`/`"false"`.

```python
conn = adbcbridge.connect(
    uri="Driver=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;Server=127.0.0.1;Database=app;",
    username="app", password="secret",
    batch_size=4096, decimal_as_string=True,   # adbc.odbc.*
)
```

## Finding the driver library

`driver_path()` looks, in order, at

1. the `ADBC_ODBC_DRIVER` environment variable (an explicit value that does not
   exist is an error, not a silent fallback);
2. a copy bundled inside this package (see below);
3. the ADBC driver manifest named `odbc` — `odbc.toml` under
   `$ADBC_DRIVER_PATH`, `<sys.prefix>/etc/adbc/drivers`,
   `~/.config/adbc/drivers`, `/etc/adbc/drivers`, … — as installed by
   `cmake --install build --prefix "$VIRTUAL_ENV"`;
4. common install locations: `<sys.prefix>/lib`, `/usr/local/lib`, `/usr/lib`,
   `/usr/lib/<arch>-linux-gnu`, and a `build/` tree next to a source checkout
   (so `pip install -e python` works right after `cmake --build build`).

If nothing matches it raises `adbcbridge.DriverNotFoundError`.

## Command line

```sh
adbcbridge query "Driver=SQLite3;Database=my.db;" "SELECT * FROM t"
adbcbridge query "Driver=SQLite3;Database=my.db;" "SELECT * FROM t WHERE i > ?" -p 3
adbcbridge query ... --format csv > out.csv     # or --format schema, --limit N
adbcbridge drivers            # ODBC drivers from odbcinst -j / odbcinst.ini
adbcbridge driver-path        # which libadbc_driver_odbc.so would be used
```

## Building a wheel with the driver bundled

The wheel is pure Python unless a driver library is around at build time, in
which case `setup.py` copies it into the package and tags the wheel for the
current platform:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python -m pip install build
python -m build --wheel python          # picks up ./build/libadbc_driver_odbc.so
```

Point it somewhere else with `ADBCBRIDGE_LIBRARY=/path/to/libadbc_driver_odbc.so`
(or `ADBCBRIDGE_BUILD_DIR=/path/to/cmake-build-tree`). Without either, and with
no `build/` tree, the wheel contains no binary and `driver_path()` falls back to
the manifest or a system-wide install at run time.

## Tests

```sh
pip install -e python
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  pytest python/tests
```

Apache-2.0.
