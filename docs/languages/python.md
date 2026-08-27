<!-- SPDX-License-Identifier: Apache-2.0 -->
# Python

adbcBridge lets any [ODBC](https://en.wikipedia.org/wiki/Open_Database_Connectivity)
(Open Database Connectivity) data source be used as an
[ADBC](https://arrow.apache.org/adbc/) (Arrow Database Connectivity) database, so
results arrive as Apache Arrow — columnar, zero-copy into pandas, Polars or
pyarrow — rather than row-by-row Python objects.

The `adbcbridge` package is a thin convenience layer. It finds the driver shared
library (`libadbc_driver_odbc.so` / `.dylib` / `.dll`) and hands it to the ADBC
driver manager. `adbcbridge.connect()` returns a plain
`adbc_driver_manager.dbapi.Connection`, so the whole ADBC DBAPI 2.0 surface is
available and nothing is wrapped or hidden.

This page assumes you are comfortable with Python but new to ODBC and ADBC. Terms
are explained on first use.

---

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
- [Loading the driver](#loading-the-driver)
- [Connection strings](#connection-strings)
- [Options](#options)
- [Running queries](#running-queries)
- [Parameters](#parameters)
- [Bulk ingest](#bulk-ingest)
- [Metadata](#metadata)
- [Transactions and autocommit](#transactions-and-autocommit)
- [Partitioned reads](#partitioned-reads)
- [Prefetch](#prefetch)
- [Errors](#errors)
- [Performance tips](#performance-tips)
- [DB-API 2.0 usage](#db-api-20-usage)
- [Known limitations](#known-limitations)
- [Complete worked example](#complete-worked-example)

---

## Requirements

| Requirement | Notes |
|---|---|
| Python | `>= 3.9` |
| `adbc-driver-manager` | `>= 1.0` — pulled in automatically |
| `pyarrow` | `>= 14` — pulled in automatically |
| An ODBC driver manager | `unixODBC` on Linux, `unixODBC` or `iODBC` on macOS, the built-in Driver Manager on Windows |
| An ODBC driver for your database | e.g. `libsqlite3odbc.so`, `psqlodbcw.so`, MySQL Connector/ODBC |

adbcBridge is the ODBC *client* — it does not include the database's own ODBC
driver. Install that separately and point the connection string at it.

---

## Installation

The `python/` directory of the repository holds the `adbcbridge` package, a thin
pip-installable wrapper that locates the shared library for you and hands it to
the ADBC driver manager. Wheels are published on the project's GitHub Releases.
Install the wheel for your platform:

```sh
pip install adbcbridge
```

That installs the wheel from [PyPI](https://pypi.org/project/adbcbridge/). The same
wheels are attached to the GitHub Release (`adbcbridge-0.1.0-py3-none-<platform>.whl`),
so an offline machine can install one directly:

```sh
pip install ./adbcbridge-0.1.0-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl
```

Or install from a source checkout:

```sh
pip install ./python
```

The package-only README lives at [`python/README.md`](../../python/README.md).

### What the wheel contains

The published wheel bundles the compiled adbcBridge driver library
(`libadbc_driver_odbc.*`) directly inside the package, so you do not build
anything. It is a plain C shared library with no Python extension module, which
is why a single wheel per platform serves every supported Python version — the
wheel is tagged `py3-none-<platform>`.

The wheel **does not** bundle the ODBC driver manager itself (`libodbc`). That is
deliberate: a foreign `libodbc` would change which `odbcinst.ini` your ODBC
drivers are read from. `auditwheel` (Linux) and `delocate` (macOS) run with
`libodbc` excluded, so the wheel uses the system driver manager already on your
machine.

### Building a wheel yourself

Wheels are pure Python unless a driver library is present at build time, in
which case it is bundled into the wheel and the wheel is tagged for the current
platform:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python -m build --wheel python     # picks up ./build/libadbc_driver_odbc.so
# or: ADBCBRIDGE_LIBRARY=/path/to/libadbc_driver_odbc.so python -m build --wheel python
```

### Platform tags

Binary wheels are built for these platforms:

| Platform | Wheel tag |
|---|---|
| Linux x86-64 | `py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64` |
| Linux aarch64 (ARM64) | `py3-none-manylinux2014_aarch64.manylinux_2_17_aarch64.manylinux_2_28_aarch64` |
| macOS arm64 (Apple Silicon, macOS 14+) | `py3-none-macosx_14_0_arm64` |
| Windows x64 | `py3-none-win_amd64` |

The Linux wheels are built for `manylinux_2_28`; `auditwheel repair` writes the
compressed tag set (`manylinux2014`, `manylinux_2_17`, `manylinux_2_28`) into
the file name, which is why the Linux names are long.

A source distribution (`sdist`) is also published. Installed on a platform with no
matching wheel it produces a pure-Python package with no bundled library; at run
time `adbcbridge.driver_path()` then falls back to a driver manifest or a
system-wide install (see [Loading the driver](#loading-the-driver)).

### The command-line tool

The package installs an `adbcbridge` command (`python -m adbcbridge` is the same
thing). Use it to confirm which library the package resolved, to list the ODBC
drivers on the machine, or to run a query without writing a script:

```sh
adbcbridge query "Driver=SQLite3;Database=my.db;" "SELECT * FROM t"   # --format table|csv|schema, --limit N, -p PARAM
adbcbridge drivers          # ODBC drivers registered in odbcinst.ini (-v adds descriptions)
adbcbridge driver-path      # which libadbc_driver_odbc.* would be used
```

`query` takes the connection string and the SQL as its two arguments. Each
`-p VALUE` supplies one `?` parameter, in order, always sent as a string;
`--limit N` prints at most `N` rows; `--format` is `table` (default), `csv` or
`schema`; `--driver-path PATH` overrides the auto-detected library.

---

## Loading the driver

There are three ways to get the driver in front of the driver manager. The
`adbcbridge` package is the simplest.

### 1. The `adbcbridge` package (recommended)

```python
import adbcbridge

with adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;") as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM t")
        table = cur.fetch_arrow_table()   # pyarrow.Table
```

`connect()` locates the library, opens the ODBC driver named in the connection
string, then hands everything to `adbc_driver_manager`. Its signature:

```python
adbcbridge.connect(uri=None, dsn=None, username=None, password=None,
                   driver_path=None, *, autocommit=False,
                   conn_kwargs=None, **options)
    -> adbc_driver_manager.dbapi.Connection
```

| Argument | Meaning |
|---|---|
| `uri` | Full ODBC connection string, e.g. `"Driver=SQLite3;Database=my.db;"` |
| `dsn` | A DSN (Data Source Name) from `odbc.ini`; used instead of, or alongside, `uri` |
| `username` | Sent as `UID=` |
| `password` | Sent as `PWD=` |
| `driver_path` | Path to `libadbc_driver_odbc.*`; defaults to `adbcbridge.driver_path()`. The manifest name `"odbc"` also works, since the value goes straight to the driver manager |
| `autocommit` | `False` (default) runs transactionally; `True` commits each statement |
| `conn_kwargs` | Extra connection-level ADBC options as a `dict` |
| `**options` | Further database options (see [Options](#options)) |

Extra keyword options become database options: a bare name is prefixed with
`adbc.odbc.` (`batch_size=4096` sets `adbc.odbc.batch_size`), a dotted name is
passed through as given, and `True`/`False` become the strings `"true"`/`"false"`:

```python
conn = adbcbridge.connect(
    uri="Driver=psqlodbcw.so;Server=127.0.0.1;Database=app;",
    username="app", password="secret",
    batch_size=4096, decimal_as_string=True,   # -> adbc.odbc.*
)
```

#### How `driver_path()` finds the library

`adbcbridge.driver_path()` looks, in order, at:

1. the `ADBC_ODBC_DRIVER` environment variable (a value that is set but does not
   exist is an error, not a silent fallback);
2. a copy bundled inside the installed package;
3. the ADBC driver manifest named `odbc` (`odbc.toml`), in the directories the
   driver manager searches — `$ADBC_DRIVER_PATH`, `<sys.prefix>/etc/adbc/drivers`,
   `~/.config/adbc/drivers`, `/etc/adbc/drivers`, and the platform equivalents;
4. common install locations: `<sys.prefix>/lib`, `/usr/local/lib`, `/usr/lib`,
   `/usr/lib/<arch>-linux-gnu`, and a `build/` tree next to a source checkout.

If nothing matches it raises `adbcbridge.DriverNotFoundError`.

### 2. `adbc_driver_manager` with an explicit path

If you would rather drive the ADBC layer yourself, point it at the library. The
`Driver=` in the connection string can be a registered name or, as here, the
path of the ODBC driver library itself:

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

Options are set on the database through `db_kwargs` (see [Options](#options)).

### 3. By name, via the driver manifest

If adbcBridge was installed with `cmake --install` (which writes an `odbc.toml`
manifest into an ADBC driver-search directory), any binding can ask for the
driver as `"odbc"`:

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="odbc",   # resolved via <prefix>/etc/adbc/drivers/odbc.toml
    db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"},
)
```

**Troubleshooting:** if `driver="odbc"` fails with
`dlsym(AdbcDriverInit) failed: .../libodbc.so: undefined symbol: AdbcDriverInit`,
then no manifest was found and the driver manager fell back to loading a shared
library literally named `odbc` — which on Unix is unixODBC's own driver manager,
not adbcBridge. Check that the directory holding `odbc.toml` is one the driver
manager searches and that the path recorded inside it exists.

### The import-order rule (Linux)

`import adbcbridge` deliberately does **not** import pyarrow —
`adbc_driver_manager.dbapi` (and with it pyarrow) is imported only when you call
`connect()`, after the ODBC driver named in the connection string has been
opened. That order matters for a handful of ODBC drivers — MySQL Connector/ODBC
is the one seen in the wild — that need `libstdc++`'s thread-locals in *static*
thread-local storage, which importing pyarrow first makes impossible.
`connect()` opens the ODBC driver first, so this is handled for you.

If you drive `adbc_driver_manager` or `pyodbc` yourself, do the same as the very
first thing in your program, before anything imports pyarrow, pandas, or
`adbc_driver_manager.dbapi`:

```python
import adbcbridge

adbcbridge.preload_odbc_driver("MySQL ODBC 9.4 Unicode Driver")  # or a full path

import adbc_driver_manager.dbapi  # imports pyarrow; now harmless
```

`preload_odbc_driver()` does on its own what `connect()` does automatically, for
programs that use `adbc_driver_manager` or pyodbc directly. Its signature:

```python
adbcbridge.preload_odbc_driver(driver=None, *, uri=None, dsn=None, strict=False)
    -> str | None     # the library path that was loaded, or None
```

`driver` is a driver path or a driver name from `odbcinst.ini`; leave it out and
the driver is taken from the `Driver=` of `uri`, or from the `Driver` of the
`dsn` section of `odbc.ini`. It is best-effort by default — a driver that will
not preload is left to fail, with its real reason, at connection time; pass
`strict=True` to have it raise `OSError` when it cannot resolve or load the
library. On Windows it is a no-op, since static TLS is an ELF concept. Set
`ADBCBRIDGE_PRELOAD=0` to switch off the automatic preload inside `connect()`.
See [`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md) for the full explanation.

---

## Connection strings

An ODBC connection string is a `;`-separated list of `KEY=VALUE` attributes. The
one attribute every connection needs is `Driver=`, which is either:

- the **path** to the ODBC driver library
  (`Driver=/usr/lib/x86_64-linux-gnu/odbc/psqlodbcw.so;…`), or
- the **name** of a driver registered in `odbcinst.ini`
  (`Driver=SQLite3;…` or `Driver={PostgreSQL Unicode};…`).

A value that contains a semicolon must be wrapped in braces: `Driver={My Driver};`.
Use a DSN instead of an inline driver with `DSN=my_source;` (the DSN's own
attributes come from `odbc.ini`).

`Username`/`password` passed to `connect()` are appended as `UID=`/`PWD=`; you can
also put them in the string directly.

### Examples

The attribute structures below are the ones adbcBridge's compatibility matrix
uses (`tests/compat/test_matrix.py`); host, port and credentials are placeholders.
For `Driver=`, substitute either the path to your ODBC driver library or its
registered `odbcinst.ini` name — the matrix supplies a path. Where a well-known
registered name is used elsewhere in this project it is shown; otherwise
`<...driver>` marks the name to fill in.

| Database | Connection string |
|---|---|
| SQLite | `Driver=SQLite3;Database=/path/to/my.db;` |
| DuckDB | `Driver=<DuckDB driver>;Database=/path/to/duck.db;` |
| PostgreSQL | `Driver=PostgreSQL Unicode;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| MySQL | `Driver=MySQL ODBC 9.4 Unicode Driver;Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;` |
| MariaDB | `Driver=<MariaDB driver>;Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;` |
| SQL Server | `Driver=<SQL Server driver>;Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=<password>;TrustServerCertificate=yes;` |
| ClickHouse | `Driver=<ClickHouse driver>;Url=http://127.0.0.1:18123;Database=adbc;UID=adbc;PWD=adbc;` |

The port numbers above are the matrix's test-harness ports; use your server's
actual port. In Python you can pass the string directly, or as the
`adbc.odbc.connection_string`
option (an alias of `uri`) when using `adbc_driver_manager` directly:

```python
# adbcbridge package
conn = adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;")

# adbc_driver_manager, uri form
conn = dbapi.connect(driver="odbc",
                     db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"})

# adbc_driver_manager, connection_string alias
conn = dbapi.connect(driver="odbc",
                     db_kwargs={"adbc.odbc.connection_string":
                                "Driver=SQLite3;Database=my.db;"})
```

---

## Options

Options are ADBC key–value settings. adbcBridge's own options are prefixed
`adbc.odbc.`; it also honours the ADBC-standard `uri`, `dsn`, `username`,
`password`, autocommit, and ingest options.

Options have a **scope** — the level at which the driver reads them:

- **database** — set at connect time (`db_kwargs`, or `connect(**options)`);
- **connection** — set on the connection (`conn_kwargs`, or on
  `conn.adbc_connection`);
- **statement** — set on the statement (`cur.adbc_statement.set_options(...)`).

Where an option lists more than one scope, a narrower scope inherits the wider
one's value unless overridden.

### adbcBridge options (`adbc.odbc.*`)

| Key | Scope | Values | Default | Purpose |
|---|---|---|---|---|
| `adbc.odbc.connection_string` | database | string | — | ODBC connection string; alias of the standard `uri` |
| `adbc.odbc.batch_size` | database, connection, statement | integer > 0 | `1024` | Rows per Arrow batch produced by the reader |
| `adbc.odbc.rowset_bytes` | database | integer > 0 | `8388608` (8 MiB) | Ceiling on a reader's bound rowset buffers. The rowset holds `batch_size` rows unless that would exceed this, in which case it holds as many as fit |
| `adbc.odbc.max_bind_bytes` | database | integer > 0 | `32768` (32 KiB) | Widest value bound at the width the driver declares for it. Wider columns are bound at `long_bind_bytes` instead, or read with `SQLGetData` where the driver cannot re-read a clipped value |
| `adbc.odbc.long_bind_bytes` | database | integer > 0 | `2048` (2 KiB) | Width to bind a column whose declared width is not a real bound — a `TEXT`/`NVARCHAR(MAX)`/`LONGTEXT`/`bytea` column, which drivers describe by what the *type* could hold (past `max_bind_bytes`), or by nothing at all (a width of 0, which is how psqlodbc describes PostgreSQL's `bytea`). Values longer than this are read again in full, so this trades nothing but speed. Where enough of them turn out not to fit — the reader watches the first 256 rows and compares the bytes it had to re-read against what the block cursor is worth — it stops binding that column for the rest of the result set and reads it with `SQLGetData` instead, since a buffer the values do not fit in costs more than it saves |
| `adbc.odbc.decimal_as_string` | database | `"true"` enables | `false` | Return `DECIMAL`/`NUMERIC` columns as Arrow strings (exact digits, no precision loss) |
| `adbc.odbc.prefetch` | database, connection, statement | integer `0`–`8` | `0` (off) | Rowsets kept in flight on a background fetch thread, so `SQLFetch` for the next one overlaps the Arrow conversion of the current one. `1` is double-buffering. See [Prefetch](#prefetch) |
| `adbc.odbc.partitions` | statement | integer `0`–`256` | `0` (auto) | How many partitions `execute_partitions` splits a query into. `0` chooses from the table's size (its block count, its row estimate, or the span of its key, whichever the chosen strategy has — one partition per 64 MiB, at most 8); `1` never splits. A count the driver cannot honour is reduced, never faked: it will not hand out a slice that reads nothing. See [Partitioned reads](#partitioned-reads) |
| `adbc.odbc.array_binding` | statement | `"true"` / `"false"` | `"true"` | Bind each Arrow batch as a column-wise ODBC parameter array (one execute per batch). `false` forces row-at-a-time. Drivers that mishandle arrays fall back automatically |
| `adbc.odbc.rows_per_insert` | statement | integer `0`–`2147483647` | `0` (auto) | Row-groups per multi-row `INSERT` for bulk ingest. `1` disables the rewrite. See [Bulk ingest](#bulk-ingest) |
| `adbc.odbc.ingest_connections` | statement | integer `1`–`64` | `1` | Connections a bulk ingest may fan out over. `N > 1` trades atomicity for speed. See [Bulk ingest](#bulk-ingest) |
| `adbc.odbc.sqllen_32bit` | database, connection, statement | `"true"`/`"1"` / `"false"`/`"0"` | autodetect | Force the 32-bit-`SQLLEN` driver quirk on or off. Autodetected from `SQL_DRIVER_NAME` (on for IBM Db2's `libdb2.so`, which also drives Informix, and for MDB Tools), so you normally never set it. Turn it on for any other ODBC driver that was built with a 32-bit `SQLLEN`/`SQLULEN` on a 64-bit platform — the giveaway is undetected NULLs, garbage string lengths, and row counts of `4294967295` |
| `adbc.odbc.tune` | database | `"true"` / `"false"` | `"true"` | May adbcBridge add ODBC connection keywords of its own where it recognises the target driver? See [Connection keywords set for you](../how-it-works/connection-keywords.md) for the complete list; `false` sends your connection string through untouched |
| `adbc.odbc.driver_name` | connection (read-only) | string | — | The `SQL_DRIVER_NAME` of the backing ODBC driver |

### Native delegation options (`adbc.odbc.delegate*`)

When a first-class native ADBC driver exists for the target (PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL), adbcBridge can hand the whole database
over to it. All of these are **database**-scoped and frozen once the database is
initialised. See [Native delegation](../how-it-works/delegation.md) for how the
choice is made.

| Key | Values | Default | Purpose |
|---|---|---|---|
| `adbc.odbc.delegate` | `auto` / `never` / `always` | `auto` | Whether to delegate to a native driver. `auto` falls back to ODBC if none is available; `always` makes a missing native driver an error |
| `adbc.odbc.delegate.driver` | string | — | Force a specific native driver: a bare name (`postgresql`) or manifest name; a filesystem path only with `allow_paths` |
| `adbc.odbc.delegate.search_path` | path list | — | Extra directories to search for native drivers (`:`-separated; `;` on Windows); needs `allow_paths` |
| `adbc.odbc.delegate.allow_paths` | `"true"` / `"false"` | `false` | Permit the two options above to name filesystem paths |
| `adbc.odbc.delegate.last_error` | (read-only) | — | Why delegation did not happen (empty if it did) |
| `adbc.odbc.delegated_to` | (read-only) | — | The native driver serving this database/connection, or `odbc` (empty before the database is initialised) |

**Note:** `adbc.odbc.delegate.driver` accepts a *path* only when
`adbc.odbc.delegate.allow_paths` is `true`, because a database option that names a
shared library is a code-execution primitive for any host that forwards
caller-supplied options.

### ADBC-standard options adbcBridge honours

| Key | Scope | Notes |
|---|---|---|
| `uri` | database | ODBC connection string (alias of `adbc.odbc.connection_string`) |
| `dsn` | database | DSN name from `odbc.ini`; appended to the connection string as `DSN=...` |
| `username`, `password` | database | Appended as `UID=`/`PWD=` |
| `adbc.connection.autocommit` | connection | `"true"`/`"false"`; default autocommit is on at the ODBC level, but `connect()` runs transactionally unless you pass `autocommit=True` |
| `adbc.connection.catalog` | connection | Current catalog; requires a live connection |
| `adbc.ingest.target_table` | statement | Set for you by `adbc_ingest()` |
| `adbc.ingest.mode` | statement | `create` / `append` / `replace` / `create_append` |
| `adbc.ingest.target_catalog`, `adbc.ingest.target_db_schema`, `adbc.ingest.temporary` | statement | Set for you by `adbc_ingest()` |

**Note:** `adbc.odbc.decimal_as_string` and `adbc.ingest.temporary` treat only the
literal `"true"` as enabling; any other value (including a typo) is read as false,
with no error. `adbc.odbc.array_binding`, `adbc.odbc.tune` and
`adbc.odbc.sqllen_32bit` validate strictly and reject anything but
`"true"`/`"false"` (or `"1"`/`"0"`).

---

## Running queries

### To a pyarrow Table

`fetch_arrow_table()` materialises the whole result set into a `pyarrow.Table`:

```python
with conn.cursor() as cur:
    cur.execute("SELECT id, name, created FROM users WHERE active = ?", (True,))
    table = cur.fetch_arrow_table()
    print(table.num_rows, table.schema)
```

### Streaming: a record-batch reader

For a result set larger than memory, stream it batch by batch.
`fetch_record_batch()` returns a `pyarrow.RecordBatchReader`; iterate it and each
`RecordBatch` holds up to `adbc.odbc.batch_size` rows:

```python
with conn.cursor() as cur:
    cur.execute("SELECT * FROM huge_table")
    reader = cur.fetch_record_batch()
    for batch in reader:            # each batch is a pyarrow.RecordBatch
        process(batch)             # e.g. compute, write to Parquet, ...
```

### To pandas or Polars

The result is Arrow, so conversion is cheap. The DBAPI cursor offers helpers:

```python
cur.execute("SELECT * FROM sales")
df = cur.fetch_df()        # pandas.DataFrame  (needs pandas installed)

cur.execute("SELECT * FROM sales")
pl = cur.fetch_polars()    # polars.DataFrame  (needs polars installed and adbc-driver-manager >= 1.6)
```

Equivalently, convert a fetched Arrow table yourself:

```python
table = cur.fetch_arrow_table()
df = table.to_pandas()             # pyarrow -> pandas
import polars as pl
pl_df = pl.from_arrow(table)       # pyarrow -> polars
```

---

## Parameters

Parameters use the ODBC `?` placeholder, positional and in order.

### A single execution

Pass a tuple of values:

```python
cur.execute("SELECT name FROM parent WHERE id = ?", (2,))
row = cur.fetchone()
```

### Batched: `executemany`

`executemany()` binds many parameter sets at once. adbcBridge sends one
`SQLExecute` per Arrow batch of parameters (a column-wise parameter array) rather
than one round trip per row, where the driver supports it:

```python
rows = [(1, "a"), (2, "b"), (3, None)]
cur.executemany("INSERT INTO parent VALUES (?, ?)", rows)
```

### Binding an Arrow array directly

For maximum throughput bind a `pyarrow.RecordBatch` (one column per `?`, one row
per execution) through the statement, then execute:

```python
import pyarrow as pa

batch = pa.record_batch({"a": pa.array([1, 2, 3]),
                         "b": pa.array(["x", "y", "z"])})
cur.adbc_statement.set_sql_query("INSERT INTO t VALUES (?, ?)")
cur.adbc_statement.bind(batch)
cur.adbc_statement.execute_update()
```

Column types map to SQL types by the Arrow type of each bound column. An untyped
Arrow null column binds as SQL `NULL`.

---

## Bulk ingest

`adbc_ingest()` loads a whole `pyarrow.Table` (or `RecordBatch`, or reader) into a
table, creating it if asked. It is the fast path for loading data and understands
several write modes.

```python
import pyarrow as pa

tbl = pa.table({"a": pa.array([1, 2, 3]),
                "b": pa.array(["p", "q", "r"])})

with conn.cursor() as cur:
    cur.adbc_ingest("my_table", tbl, mode="create")
conn.commit()
```

### Modes

| `mode` | Behaviour |
|---|---|
| `create` | Create the table from the Arrow schema, then insert. Fails if it exists |
| `append` | Insert into an existing table |
| `replace` | Drop the table if present, recreate it, then insert |
| `create_append` | Create the table if absent, then insert either way |

### How rows are sent, and per-driver probing

By default adbcBridge sends one `INSERT INTO t VALUES (…),(…),…` per *K* rows
inside a single transaction, rather than one statement per row. *K* is probed
against the driver at run time and remembered on the connection — SQLite's
999-variable limit, ClickHouse's refusal to execute large prepared row-groups, and
Oracle's rejection of the multi-row form (it falls back to
`INSERT ALL … SELECT 1 FROM dual`) are all discovered automatically. Firebird,
which has no multi-row `VALUES`, gets a `UNION ALL` of typed one-row `SELECT`s.

Two statement options tune this:

- `adbc.odbc.rows_per_insert` — `0` (default) picks *K* automatically, `1` turns
  the multi-row rewrite off (one `INSERT` per row, or ODBC parameter arrays), any
  other value asks for that many row-groups.
- `adbc.odbc.array_binding` — `"true"` (default) binds each batch as a column-wise
  ODBC parameter array where the driver honours it; drivers that mishandle arrays
  fall back automatically, and DuckDB and clickhouse-odbc default it off.

Set them on the statement before ingesting:

```python
cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": "500"})
cur.adbc_ingest("my_table", tbl, mode="create")
```

**PostgreSQL** goes one step further automatically: it sends a whole *column* of a
batch as a single array parameter and lets the server expand it with `unnest(...)`.
This is on by default, needs no option, and is used only against genuine
PostgreSQL servers (not forks) and only for types it can render exactly.

### Parallel ingest

`adbc.odbc.ingest_connections` (default `1`) spreads one ingest over `N`
connections, each with its own transaction. This trades atomicity for speed: `N`
connections are `N` transactions, so a failure can leave some batches committed and
others not. Use `N > 1` only where a partially populated table on failure is
acceptable, or where the caller drops and retries. It also quietly stays on one
connection when the caller is inside its own transaction (the `CREATE TABLE` would
be invisible to the workers), and is forced to `1` on Windows.

Commits are batched: when the connection is in autocommit and more than one row is
bound, adbcBridge turns autocommit off for the ingest and commits once at the end
(rolling back on failure), instead of paying a commit per row. A transaction you
opened yourself is left alone.

---

## Metadata

The connection exposes the database catalog through ADBC's metadata calls.

### `adbc_get_objects` — catalogs, schemas, tables, columns

```python
reader = conn.adbc_get_objects(depth="all")       # a RecordBatchReader
catalog = reader.read_all()
```

`depth` is one of `"catalogs"`, `"db_schemas"`, `"tables"`, `"all"` (columns).
Optional filters narrow the result:

```python
conn.adbc_get_objects(depth="tables", table_name_filter="par%")
```

### `adbc_get_table_schema` — one table's Arrow schema

```python
schema = conn.adbc_get_table_schema("my_table")    # pyarrow.Schema
```

Takes optional `catalog_filter=` and `db_schema_filter=` arguments to disambiguate.

### `adbc_get_info` — driver and server facts

```python
info = conn.adbc_get_info()          # dict-like: vendor name/version, driver name, ...
```

`adbc_get_info()` is also how you see who served a connection: the driver name is
`ADBC ODBC Driver` for the ODBC path, or the native driver's name (e.g.
`ADBC PostgreSQL Driver`) when the connection was delegated.

---

## Transactions and autocommit

By default `adbcbridge.connect()` opens a **transactional** connection: changes are
held until you call `conn.commit()`, and `conn.rollback()` discards them.

```python
conn = adbcbridge.connect(uri="Driver=PostgreSQL Unicode;Server=...;")
with conn.cursor() as cur:
    cur.execute("INSERT INTO t VALUES (1)")
    cur.execute("INSERT INTO t VALUES (2)")
conn.commit()          # both, or neither if an error rolled back
```

Pass `autocommit=True` to `connect()` (or set the connection option
`adbc.connection.autocommit` to `"true"`) to commit each statement immediately:

```python
conn = adbcbridge.connect(uri="...", autocommit=True)
```

**Note:** not every ODBC driver supports transactions. Against one that does not,
run in autocommit.

---

## Partitioned reads

One connection reading a large table is one CPU decoding it. ADBC's partition
contract lets you split a query into *N* opaque descriptors and read each on its
own connection, in parallel, and concatenate the pieces. adbcBridge implements both
halves.

```python
import concurrent.futures, pyarrow
import adbcbridge

CONN = "Driver=PostgreSQL Unicode;Server=127.0.0.1;Database=adbc;Uid=adbc;Pwd=adbc;"

def open_conn():
    return adbcbridge.connect(uri=CONN, delegate="never")

with open_conn() as conn, conn.cursor() as cur:
    cur.adbc_statement.set_options(**{"adbc.odbc.partitions": "8"})
    cur.adbc_statement.set_sql_query("SELECT id, val, txt, dt FROM bench")
    descriptors, schema, _ = cur.adbc_statement.execute_partitions()

def read(descriptor):                       # one connection per partition
    with open_conn() as conn:
        stream = conn.adbc_connection.read_partition(descriptor)
        return pyarrow.RecordBatchReader._import_from_c(stream.address).read_all()

with concurrent.futures.ThreadPoolExecutor(len(descriptors)) as pool:
    table = pyarrow.concat_tables(list(pool.map(read, descriptors)))
```

`adbc.odbc.partitions` controls the split count: `0` (default) chooses from the
table's size, `1` never splits. The driver splits only what it can prove is safe —
a bare `SELECT <columns> FROM <one table>` on PostgreSQL-wire servers with a heap
or a usable integer primary key — and otherwise returns a single descriptor
carrying the original query, which is always correct and never slower than not
partitioning. The strategies and their measurements are in
[Partitioned reads](../how-it-works/partitioned-reads.md).

**Note:** partitions are read on separate connections and therefore under separate
snapshots. Against a table being written concurrently, the union of the slices is
not a point-in-time view. Against a table that is not being written, the slices
reproduce the unpartitioned read exactly.

---

## Prefetch

`adbc.odbc.prefetch=1` overlaps ODBC's `SQLFetch` with the conversion into Arrow:
a background thread fetches the next rowset while the calling thread converts the
current one. Higher values keep more rowsets in flight, up to `8`.

```python
conn = adbcbridge.connect(uri="Driver=PostgreSQL Unicode;Server=...;", prefetch=1)
```

It is **off by default**, because whether it is safe depends on the ODBC driver
underneath and no driver can be asked. It engages only when every column is bound
at a width that cannot truncate; a column that would need re-reading (a `TEXT` or
`NVARCHAR(MAX)`), or a driver that fetches one row at a time, turns it off
silently for that query, changing nothing about the rows or the errors.

**In practice it is worth very little**: most ODBC drivers have already buffered
the whole result set client-side by the time `SQLFetch` is called, so there is no
socket wait to hide. For a big PostgreSQL read, [partitioning](#partitioned-reads)
is the mechanism that pays. The measurements are in
[Prefetch](../how-it-works/prefetch.md).

---

## Errors

Database and driver errors are raised as subclasses of the DBAPI `Error`
hierarchy, which `adbc_driver_manager` populates from the underlying ADBC error.
The exception carries:

- a **message** — the driver's diagnostic, prefixed with the failing ODBC call;
- a **SQLSTATE** — the five-character `SQL:2003` status code (e.g. `42S02` for
  "table not found"), when the driver provides one;
- a **vendor code** — the database-specific integer error code, when provided.

```python
from adbc_driver_manager import dbapi

try:
    cur.execute("SELECT * FROM no_such_table")
except dbapi.Error as exc:
    print("message:", exc)
    # SQLSTATE, vendor code and status details are carried on the ADBC error;
    # inspect exc.args / the exception attributes your driver-manager version exposes
```

The DBAPI exception hierarchy (`dbapi.Error` and its subclasses
`DatabaseError`, `ProgrammingError`, `IntegrityError`, ...) is also exposed as
attributes of the connection (`conn.Error`, `conn.DatabaseError`, ...).

An error does not poison the statement or connection: a valid query afterwards
still works. adbcBridge also augments the common unixODBC
`Can't open lib '<path>' : file not found` message — reported for *any* load
failure, not only a missing file — with the real reason (see
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md)).

---

## Performance tips

- **Read in Arrow, stay in Arrow.** Use `fetch_arrow_table()` /
  `fetch_record_batch()` and hand the columnar data to pandas, Polars or Parquet
  without a Python-object round trip.
- **Tune `adbc.odbc.batch_size`** (default `1024`) up for wide, simple rows to
  amortise per-batch overhead; the rowset buffer is still capped by
  `adbc.odbc.rowset_bytes`.
- **Bulk-load with `adbc_ingest()`**, not row-by-row `INSERT`s. The multi-row
  rewrite alone is often 10–100× faster; see the measured numbers in the project's
  `bench/BENCHMARKS.md`.
- **For large reads, partition** across connections rather than reaching for
  prefetch — partitioning is what pays for a big scan.
- **Let adbcBridge delegate** to a native ADBC driver where one exists
  (`adbc.odbc.delegate` defaults to `auto`): native PostgreSQL/SQLite/DuckDB
  drivers build Arrow directly and are faster than anything over ODBC.
- **`decimal_as_string=True`** avoids precision loss on `DECIMAL`/`NUMERIC`
  columns whose declared scale the driver reports imprecisely.

---

## DB-API 2.0 usage

`connect()` returns a standard [PEP 249](https://peps.python.org/pep-0249/) DBAPI
2.0 connection from `adbc_driver_manager.dbapi`, so ordinary cursor code works
unchanged:

```python
import adbcbridge

conn = adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;")
try:
    cur = conn.cursor()
    cur.execute("SELECT id, name FROM users WHERE id > ?", (10,))
    for row in cur.fetchall():       # row-oriented, PEP 249 style
        print(row)
    cur.close()
    conn.commit()
finally:
    conn.close()
```

The row-oriented `fetchone()`/`fetchmany()`/`fetchall()` methods work, but for
anything data-heavy the Arrow methods (`fetch_arrow_table`, `fetch_record_batch`,
`fetch_df`, `fetch_polars` — the last needs adbc-driver-manager >= 1.6) are both
faster and lossless.

You can also drive the connection directly instead of through the `adbcbridge`
package:

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(driver="odbc",
                     db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"})
```

---

## Known limitations

- **The ODBC driver manager and the database's ODBC driver are not bundled.** The
  wheel ships only adbcBridge itself; you must have `unixODBC`/`iODBC`/the Windows
  Driver Manager and the target database's ODBC driver installed.
- **Static-TLS import order on Linux.** A few ODBC drivers (MySQL Connector/ODBC)
  cannot load after pyarrow has been imported. The `adbcbridge` package handles
  this for you; if you drive `adbc_driver_manager` yourself, preload the driver
  first. See [`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md).
- **Native delegation is not available on Windows.** There `adbc.odbc.delegate:
  auto` always takes the ODBC path and `always` fails with a clear message.
- **Parallel ingest (`ingest_connections > 1`) is not atomic** and is forced to
  `1` on Windows and whenever the caller is inside its own transaction.
- **Prefetch is off by default and often negligible**, because most ODBC drivers
  buffer the whole result set client-side.
- **Partitioning falls back to a single partition** for anything but a provably
  splittable `SELECT` — a `WHERE`/`JOIN`/`ORDER BY`, quoting or comments in the
  SQL, bound parameters, no heap and no usable integer primary key, and so on.
- **Behaviour varies by ODBC driver.** Type mappings, transaction support,
  NULL-parameter handling and reported row counts are all the underlying driver's,
  not adbcBridge's.

---

## Complete worked example

A self-contained script: connect, create and bulk-load a table, query it into
Arrow and pandas, run a parameterised query, and read the catalog.

```python
# SPDX-License-Identifier: Apache-2.0
"""End-to-end adbcBridge example against a local SQLite database."""
import pyarrow as pa
import adbcbridge

CONN = "Driver=SQLite3;Database=example.db;"

with adbcbridge.connect(uri=CONN) as conn:
    # 1. Bulk-load a pyarrow.Table into a fresh table.
    data = pa.table({
        "id":   pa.array([1, 2, 3, 4], pa.int64()),
        "name": pa.array(["alice", "bob", "carol", "dan"]),
        "score": pa.array([9.5, 7.0, None, 8.25], pa.float64()),
    })
    with conn.cursor() as cur:
        cur.adbc_ingest("people", data, mode="replace")
    conn.commit()

    # 2. Query the whole table into Arrow, then pandas.
    with conn.cursor() as cur:
        cur.execute("SELECT id, name, score FROM people ORDER BY id")
        table = cur.fetch_arrow_table()
        print(table)
        print(table.to_pandas())

    # 3. A parameterised query.
    with conn.cursor() as cur:
        cur.execute("SELECT name FROM people WHERE score >= ?", (8.0,))
        print([row[0] for row in cur.fetchall()])   # ['alice', 'dan']

    # 4. Stream a (here small) result set batch by batch.
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM people")
        for batch in cur.fetch_record_batch():
            print("batch of", batch.num_rows, "rows")

    # 5. Inspect the catalog.
    with conn.cursor() as cur:
        schema = conn.adbc_get_table_schema("people")
        print(schema)
```

Run it after installing the package and a SQLite ODBC driver:

```sh
pip install adbcbridge
python example.py
```
