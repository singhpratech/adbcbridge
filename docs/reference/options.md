<!-- SPDX-License-Identifier: Apache-2.0 -->
# Options

adbcBridge is an ADBC (Arrow Database Connectivity) driver that reaches a
database through an ODBC (Open Database Connectivity) driver. It is configured
the ADBC way: string key/value **options** set on a database, connection, or
statement handle. This page lists every option the driver accepts, its level,
type, default and effect, then explains the notable ones in depth, and finally
documents the environment variables the library and its language bindings read.

Options come in two families:

- **`adbc.odbc.*`** — options specific to adbcBridge.
- **Standard ADBC options** the driver honours: `uri`, `username`, `password`,
  `adbc.connection.autocommit`, `adbc.connection.catalog`, and the bulk-ingest
  options `adbc.ingest.*`.

Values are always strings (ADBC also offers typed setters — integer, double,
bytes — which adbcBridge maps onto the string form where meaningful). Unless a
default is noted, an option is simply unset.

An option set at a broader level is inherited by narrower ones opened afterward:
`adbc.odbc.batch_size` set on the database is the starting value for every
connection and statement, each of which may override it.

---

## Database options

Set with `AdbcDatabaseSetOption` before `AdbcDatabaseInit`.

| Key | Type / values | Default | Effect |
|---|---|---|---|
| `uri` | string | — | The ODBC connection string, e.g. `Driver=SQLite3;Database=my.db;`, or a native ADBC URI when delegating. Alias: `adbc.odbc.connection_string`. |
| `adbc.odbc.connection_string` | string | — | Same as `uri`. |
| `dsn` | string | — | An ODBC Data Source Name (a driver + settings registered in `odbc.ini`). One of `uri`/`dsn` is required. |
| `username` | string | — | Appended as `UID=` to the connection string. |
| `password` | string | — | Appended as `PWD=`. Not readable back. |
| `adbc.odbc.batch_size` | positive integer | `1024` | Rows per Arrow batch (`SQL_ATTR_ROW_ARRAY_SIZE`). |
| `adbc.odbc.prefetch` | integer `0`–`8` | `0` | Rowsets kept in flight on a background fetch thread. `0` off, `1` double-buffering. |
| `adbc.odbc.max_bind_bytes` | positive integer | `32768` | Declared column width (bytes) above which a variable-length column is not bound at full width. |
| `adbc.odbc.long_bind_bytes` | positive integer | `2048` | Width at which such an over-wide column is bound anyway, with truncated values repaired. |
| `adbc.odbc.rowset_bytes` | positive integer | `8388608` (8 MiB) | Ceiling on one reader's bound rowset buffers; rows per rowset are capped to fit. |
| `adbc.odbc.decimal_as_string` | `true` to enable | `false` | Read `DECIMAL`/`NUMERIC` as Arrow `string` instead of `decimal128`. Only the exact value `true` enables it. |
| `adbc.odbc.sqllen_32bit` | `true`/`false` (`1`/`0`) | autodetected | Force the 32-bit-`SQLLEN` driver quirk on or off. Unset means autodetect from the driver name. |
| `adbc.odbc.tune` | `true`/`false` (`1`/`0`) | `true` | Allow adbcBridge to add its own ODBC connection keywords where it recognises the driver. |
| `adbc.odbc.delegate` | `auto` / `never` / `always` | `auto` (or `$ADBC_ODBC_DELEGATE`) | Whether to hand the connection to a native ADBC driver instead of going over ODBC. |
| `adbc.odbc.delegate.driver` | string | — | The native ADBC driver to delegate to (name, path, or manifest). |
| `adbc.odbc.delegate.search_path` | path list | — | Extra directories to find the native driver in (honoured only with `allow_paths`). |
| `adbc.odbc.delegate.allow_paths` | `true`/`false` (`1`/`0`) | `false` (or `$ADBC_ODBC_DELEGATE_ALLOW_PATHS`) | Opt in to loading a native driver from a caller-named path. |
| any `adbc.*` not under `adbc.odbc.*` | string | — | Held and passed through to the native driver when the connection is delegated; reported as unknown if ODBC ends up serving it. |

Read-only database options (via `AdbcDatabaseGetOption`): `uri`, `dsn`,
`username`, and `adbc.odbc.tune`. Typed integer reads are available for
`batch_size`, `max_bind_bytes`, `long_bind_bytes`, `rowset_bytes`,
`sqllen_32bit`, and `tune`.

Setting any delegate option after `AdbcDatabaseInit` is an error.

---

## Connection options

Set with `AdbcConnectionSetOption`.

| Key | Type / values | Default | Effect |
|---|---|---|---|
| `adbc.connection.autocommit` | `true`/`false` | `true` | Turns ODBC autocommit on or off (`SQL_ATTR_AUTOCOMMIT`). |
| `adbc.connection.catalog` | string | — | Sets the current catalog (`SQL_ATTR_CURRENT_CATALOG`); requires the connection to be open. |
| `adbc.odbc.batch_size` | positive integer | inherited | Per-connection batch size. |
| `adbc.odbc.prefetch` | integer `0`–`8` | inherited | Per-connection prefetch. |
| `adbc.odbc.sqllen_32bit` | `true`/`false` (`1`/`0`) | inherited | Per-connection 32-bit-`SQLLEN` override. |

Read-only connection options (via `AdbcConnectionGetOption`):

| Key | Value |
|---|---|
| `adbc.connection.autocommit` | `true`/`false` |
| `adbc.connection.catalog` | current catalog (when connected) |
| `adbc.odbc.sqllen_32bit` | `true`/`false` |
| `adbc.odbc.driver_name` | `SQL_DRIVER_NAME` of the backing ODBC driver (the underlying driver's file name; `ADBC_INFO_DRIVER_NAME` stays a stable identity for adbcBridge itself) |
| `adbc.odbc.delegated_to` | the native driver serving the connection, or `odbc` |

A standard ADBC option that neither ODBC nor the driver understands before init
is *held* so a native driver can receive it if the connection turns out to be
delegated; if ODBC serves the connection, the first such option is reported as
unknown.

---

## Statement options

Set with `AdbcStatementSetOption`.

| Key | Type / values | Default | Effect |
|---|---|---|---|
| `adbc.odbc.batch_size` | positive integer | inherited | Per-statement batch size. |
| `adbc.odbc.prefetch` | integer `0`–`8` | inherited | Per-statement prefetch. |
| `adbc.odbc.sqllen_32bit` | `true`/`false` (`1`/`0`) | inherited | Per-statement override. |
| `adbc.odbc.array_binding` | `true`/`false` | `true` (unless a driver quirk disables it) | Bind Arrow batches as ODBC parameter arrays (one execute per batch) instead of one execute per row. |
| `adbc.odbc.partitions` | integer `0`–`256` | `0` | Partitions `AdbcStatementExecutePartitions` aims for. `0` automatic, `1` never split. |
| `adbc.odbc.rows_per_insert` | integer `0`–`2147483647` | `0` | Row-groups per `INSERT` during bulk ingest. `0` automatic, `1` disables the multi-row rewrite. |
| `adbc.odbc.ingest_connections` | integer `1`–`64` | `1` | Connections a bulk ingest may spread over. `1` keeps it atomic on the caller's connection. Forced to `1` on Windows. |
| `adbc.ingest.target_table` | string | — | Target table for bulk ingest. |
| `adbc.ingest.target_catalog` | string | — | Target catalog for bulk ingest. |
| `adbc.ingest.target_db_schema` | string | — | Target schema for bulk ingest. |
| `adbc.ingest.mode` | `adbc.ingest.mode.create` / `.append` / `.replace` / `.create_append` | `create` | How the target table is populated (see below). |
| `adbc.ingest.temporary` | `true` to enable | `false` | Ingest into a temporary table. Only the exact value `true` enables it. |

Read-only statement options (via `AdbcStatementGetOptionInt`): `batch_size`,
`array_binding`, `rows_per_insert`, `ingest_connections`, `partitions`,
`prefetch`, `sqllen_32bit`.

### Ingest modes

| Value | Meaning |
|---|---|
| `adbc.ingest.mode.create` | Create a new table (the default); fail if it exists. |
| `adbc.ingest.mode.append` | Insert into an existing table; do not create. |
| `adbc.ingest.mode.replace` | Drop any existing table and recreate it. |
| `adbc.ingest.mode.create_append` | Create the table if absent, then insert. |

---

## Notable options in depth

### Native delegation (`adbc.odbc.delegate*`)

Some databases ship a first-party ADBC driver that is faster or more faithful
than going through ODBC. When delegation is enabled, adbcBridge stands up that
native driver, translates the ODBC connection string and standard options into
what the native driver expects, and forwards every call to it — the ODBC path is
never opened.

- `adbc.odbc.delegate` = `auto` (try native, fall back to ODBC), `never` (always
  ODBC), or `always` (native required; fail if unavailable). The default comes
  from the `ADBC_ODBC_DELEGATE` environment variable, else `auto`.
- `adbc.odbc.delegate.driver` names the native driver.
- `adbc.odbc.delegate.search_path` adds directories to look in, but only when
  `adbc.odbc.delegate.allow_paths` is `true` — loading a driver from a
  caller-named path is opt-in for safety, and is refused outright for
  setuid/setgid processes.
- `adbc.odbc.delegate.last_error` (read-only) explains why delegation did not
  happen; `adbc.odbc.delegated_to` (read-only) names the driver that served the
  connection, or `odbc`.

### Prefetch (`adbc.odbc.prefetch`)

By default the reader fetches one rowset, converts it to Arrow, then fetches the
next. Prefetch runs the ODBC `SQLFetch` for the next rowset on a background
thread while the current rowset is being converted, overlapping the two. `0`
(the default) is off; `1` is classic double buffering; the ceiling is `8`. Each
rowset in flight is another full set of bound buffers (`rowset_bytes` of them),
and one is usually enough to hide the fetch behind the conversion — more helps
only when fetch and conversion times are very unevenly matched.

### Batch and buffer sizing (`batch_size`, `rowset_bytes`, `max_bind_bytes`, `long_bind_bytes`)

These four together decide how much the reader binds at once. `batch_size` is the
row count it would like per rowset. Because ODBC drivers describe a variable-length
column by the widest value its *type* could hold — sqliteodbc reports 65,536
characters for every `TEXT` column, MySQL 16,777,215, SQL Server 2,147,483,647
for `NVARCHAR(MAX)` — binding at full declared width would allocate hundreds of
megabytes per rowset. So:

- `rowset_bytes` (default 8 MiB) caps the bound buffers; the reader fetches
  `min(batch_size, rowset_bytes / row_width)` rows at a time, at least one.
- `max_bind_bytes` (default 32 KiB) is the width above which a variable-length
  column is not bound at its declared width.
- `long_bind_bytes` (default 2 KiB) is the narrower width such a column *is*
  bound at, with any value that overflows re-read (where the driver supports it —
  see [Driver quirks](quirks.md)). Binding narrow and repairing the overflow
  costs nothing on the values that fit; the default of 2 KiB was measured fastest
  for ordinary text (2 KiB/row versus 256 KiB/row at the declared width).

### `adbc.odbc.sqllen_32bit`

Some ODBC drivers are compiled with a 32-bit `SQLLEN`/`SQLULEN` while the driver
manager and adbcBridge use 64-bit ones — notably IBM's freely downloadable Db2
CLI driver on 64-bit Linux, and MDB Tools (Microsoft Access). Every length or
indicator such a driver writes is then only four bytes wide, which unrepaired
reads as garbage (a NULL can look like a 4 GB value). adbcBridge autodetects this
from the driver name; set this option to `true`/`false` to force it either way
(which also suppresses the autodetection).

### Bulk-ingest tuning (`rows_per_insert`, `ingest_connections`, `array_binding`)

- `adbc.odbc.array_binding` (default `true`) binds an Arrow batch as one ODBC
  parameter array executed once, rather than one execute per row. Some drivers
  accept parameter arrays but execute them incorrectly; for those it is turned
  off automatically (quirk `no_param_arrays`).
- `adbc.odbc.rows_per_insert` (default `0` = automatic) controls the multi-row
  `INSERT` rewrite ingest uses: `INSERT INTO t VALUES (?,?),(?,?),…` carrying this
  many row-groups per execute. `1` disables the rewrite; the automatic value is
  found by probing how large a statement the server will prepare. Only bulk
  ingest is rewritten — a caller's own SQL is never touched.
- `adbc.odbc.ingest_connections` (default `1`) lets a bulk ingest fan out over
  several connections. **This trades atomicity for speed** and is opt-in for that
  reason: `N > 1` opens `N−1` extra connections, each an independent transaction,
  so a failure can leave the table holding an unspecified subset of the stream.
  Use it only where a partially populated table on failure is acceptable. Fan-out
  is skipped (silently falling back to one connection) when the caller is inside
  its own transaction, and is compiled out entirely on Windows.

### `adbc.odbc.tune`

Where adbcBridge recognises the backing driver, it may add ODBC connection
keywords that suit how it reads a result set — for example asking psqlodbc to
`Fetch` several rowsets per server-side-cursor round trip. It never overrides a
keyword the caller set (in the connection string or the DSN) and never sets
anything that changes what a query returns. Set `adbc.odbc.tune=false` to turn
this off.

### Diagnostics and logging

adbcBridge has no logging or diagnostics option. Errors surface through the
standard ADBC `AdbcError` (message, SQLSTATE, vendor code), populated from the
backing driver's ODBC diagnostic records. The read-only options
`adbc.odbc.driver_name`, `adbc.odbc.delegated_to`, and
`adbc.odbc.delegate.last_error` are the introspection points for which driver is
in play and whether delegation happened.

---

## Environment variables

Environment variables fall into three groups: those the driver library itself
reads, those the language bindings read to locate the library, and those the
test and benchmark harnesses read. Secure-exec (setuid/setgid) processes ignore
the path-bearing ones.

### Read by the driver library

| Variable | Read in | Controls |
|---|---|---|
| `ADBC_ODBC_DELEGATE` | `src/odbc_delegate.c` | Default value of `adbc.odbc.delegate` (`auto`/`never`/`always`). Invalid values fall back to `auto`. |
| `ADBC_ODBC_DELEGATE_ALLOW_PATHS` | `src/odbc_delegate.c` | Default for `adbc.odbc.delegate.allow_paths`. Ignored under secure-exec. |
| `ADBC_ODBC_DELEGATE_PATH` | `src/odbc_delegate.c` | Extra native-driver search directories (fallback for `delegate.search_path`). Ignored under secure-exec. |
| `ADBC_DRIVER_PATH` | `src/odbc_delegate.c` | Extra directories searched for ADBC driver manifests. Ignored under secure-exec. |
| `XDG_CONFIG_HOME`, `HOME` | `src/odbc_delegate.c` | Locate the per-user manifest directory (`$XDG_CONFIG_HOME/adbc/drivers`, else `$HOME/.config/adbc/drivers`). |

The driver library itself does **not** read `ADBC_ODBC_DRIVER`, `ODBCINI`, or
`ODBCSYSINI` — those are conventions of the bindings and of unixODBC.

### Read by the ODBC driver manager (unixODBC / iODBC)

adbcBridge does not read these; the driver manager it loads does, and they decide
which vendor ODBC drivers and DSNs are available:

| Variable | Controls |
|---|---|
| `ODBCSYSINI` | Directory holding the system `odbcinst.ini` (registered drivers) and `odbc.ini` (system DSNs). |
| `ODBCINI` | Path of the user `odbc.ini` (DSN definitions). |

### Read by the language bindings to locate the library

Each binding finds `libadbc_driver_odbc` by checking, in order, an explicit
library path, then the ADBC manifest directories, then conventional install
locations.

| Variable | Bindings | Controls |
|---|---|---|
| `ADBCBRIDGE_LIBRARY` | Java, C#, and (build-time) Python, Rust | Explicit path to the driver library; checked first. |
| `ADBC_ODBC_DRIVER` | Python, Java, C#, Go, and the test suites | Explicit path to the driver library. |
| `ADBCBRIDGE_PRELOAD` | Python | Toggles the driver preload in `connect()`; `0`/`false`/`no`/`off` disables it. |
| `ADBCBRIDGE_BUILD_DIR` | Python (build) | Build tree to find the freshly built library in. |
| `ADBC_DRIVER_PATH` | Python, Java, C# | Extra manifest search directories. |
| `ODBCSYSINI`, `ODBCINI` | Python | Fallbacks for locating `odbcinst.ini`/`odbc.ini` when `odbcinst -j` yields nothing. |
| `XDG_CONFIG_HOME` | Python, Java, C# | Overrides the `~/.config` manifest location. |
| `LOCALAPPDATA`, `PROGRAMDATA`, `ProgramFiles`, `USERPROFILE` | Java, C# (Windows) | Windows manifest and install directories. |
| `VIRTUAL_ENV`, `CONDA_PREFIX` | Java | Python-environment prefixes searched for a bundled driver. |

### Read by the tests and benchmarks

These configure the compatibility matrix (`tests/compat/test_matrix.py`) and the
benchmarks; they are not read by the driver:

| Variable | Controls |
|---|---|
| `<NAME>_ODBC_DRIVER` | Path to each database's vendor ODBC driver (e.g. `SQLITE_ODBC_DRIVER`, `POSTGRES_ODBC_DRIVER`, `MYSQL_ODBC_DRIVER`, …). Also gates whether that database's matrix entry runs. |
| `<NAME>_CONN` | Overrides the whole connection string for one matrix entry. |
| `ADBC_MATRIX_SUFFIX` | Table-name suffix so concurrent runs on a shared server do not collide. |
| `ADBC_ODBC_DELEGATE` | Set to `never` by some tests to force the ODBC path. |

`SQLITE_ODBC_DRIVER` is also consumed by CMake's C smoke test (see
[Building from source](building.md)).
