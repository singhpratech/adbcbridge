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
`~/.config/adbc/drivers/odbc.toml`, which is a directory the ADBC driver
manager already searches — so `driver="odbc"` resolves with nothing else set:
no `ADBC_DRIVER_PATH`, no `LD_LIBRARY_PATH`. Re-running it is safe.

`uri` is an ODBC connection string; `Driver=` takes either a registered ODBC
driver name or the path to its `.so`.

### Naming the driver from each language

Once the manifest is installed, every ADBC binding loads it as `"odbc"`:

| Language | How to name the driver |
|---|---|
| Python | `dbapi.connect(driver="odbc", db_kwargs={"uri": ...})` |
| R | `adbc_driver("odbc")`, then `adbc_database_init(drv, uri = ...)` (`adbcdrivermanager`) |
| Go | `drivermgr.Driver{}` → `NewDatabase(map[string]string{"driver": "odbc", "uri": ...})` |
| Rust | `ManagedDriver::load_from_name("odbc", None, AdbcVersion::V110, LOAD_FLAG_DEFAULT, None)` |
| Java | `JniDriver.PARAM_DRIVER.set(params, "odbc")` (`adbc-driver-jni`) |
| C# | `AdbcDriverManager.FindLoadDriver("odbc")` (`Apache.Arrow.Adbc.DriverManager`) |

Every one of these resolves a bare name through the same manifest search, so
they all pick up the file `install.sh` wrote. In R you can also pass the name
straight to `adbc_database_init("odbc", uri = ...)`; in Go the default load
flags already enable the manifest search; in Rust and C# the load flags /
`AdbcLoadFlags.Default` argument controls which directories are searched.

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
| DuckDB (latest) | duckdb-odbc | PASS (driver quirks handled: 2048-row vectors, no `SQL_BIT` params, no usable parameter arrays) |
| PostgreSQL 16 | psqlodbc 16 | PASS |
| MariaDB 11 | MariaDB Connector/ODBC 3.1 | PASS |
| SQL Server 2022 | msodbcsql 18 | PASS (incl. `NVARCHAR(MAX)` via chunked `SQLGetData`) |
| Oracle 23ai Free | Instant Client ODBC 23 | PASS (set `NLS_LANG=.AL32UTF8` for non-ASCII; 64-bit ints sent as numeric text — driver lacks `SQL_C_SBIGINT`) |
| ClickHouse 26 | clickhouse-odbc 1.5 | PASS (NULL params need `SQLDescribeParam`; no affected-row counts; `Nullable()` DDL wrapper on ingest; no usable parameter arrays) |
| MySQL 8.4 | MySQL Connector/ODBC 9.4 (and MariaDB Connector/ODBC 3.1) | PASS |
| CockroachDB 26.3 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; declare a PRIMARY KEY or the synthesised hidden `rowid` shows up in `GetObjects`) |
| MonetDB 11.55 (Dec2025-SP3) | MonetDBODBClib 11.55 | PASS |
| IBM Db2 12.1 | Db2 CLI driver (clidriver `libdb2.so`) | PASS (driver quirk handled: 32-bit `SQLLEN` — see `adbc.odbc.sqllen_32bit`) |
| Firebird 5 | Firebird ODBC 3.5.0-rc1 | PASS (driver quirks handled: `SQL_C_WCHAR` sized in 4-byte `wchar_t`, no usable parameter arrays) |

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

For a no-root user install, use [`install.sh`](install.sh) (see
[Quick start](#quick-start)); it wraps the CMake commands below with
`PREFIX=~/.local` and the manifest going to the ADBC user config directory.
`PREFIX`, `MANIFEST_DIR`, `BUILD_DIR`, `BUILD_TYPE` and `JOBS` override the
defaults. Otherwise, install by hand:

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
| `~/.config/adbc/drivers` | per-user install: what `./install.sh` uses (`$XDG_CONFIG_HOME/adbc/drivers` if set); by hand, `-DADBCBRIDGE_MANIFEST_DIR="$HOME/.config/adbc/drivers"` |
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
| `adbc.odbc.sqllen_32bit` | `true`/`false` to force the 32-bit-`SQLLEN` driver quirk on or off. Autodetected from `SQL_DRIVER_NAME` (on for IBM Db2's `libdb2.so`), so you normally never set it. Turn it on for any other ODBC driver that was built with a 32-bit `SQLLEN`/`SQLULEN` on a 64-bit platform — the giveaway is undetected NULLs, garbage string lengths, and row counts of `4294967295`. Also settable on the connection and the statement. |

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

## Python package

`python/` holds a thin pip-installable wrapper, `adbcbridge`, that locates the
shared library for you and hands it to the ADBC driver manager:

```sh
pip install ./python          # from a checkout; `pip install adbcbridge` once published
```

```python
import adbcbridge

with adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;") as conn:
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM t")
        table = cur.fetch_arrow_table()      # pyarrow.Table
```

`connect(uri=None, dsn=None, username=None, password=None, driver_path=None, **options)`
returns a plain `adbc_driver_manager.dbapi.Connection` — nothing is wrapped or
hidden. Extra keyword options become database options: a bare name is prefixed
with `adbc.odbc.` (`batch_size=4096` → `adbc.odbc.batch_size`), a dotted name is
passed through as given, and `True`/`False` become `"true"`/`"false"`.

`adbcbridge.driver_path()` returns the path of `libadbc_driver_odbc.so`, looked
up in this order: the `ADBC_ODBC_DRIVER` environment variable, a copy bundled
inside the package, the driver manifest named `odbc` (see above), then common
install locations (`<sys.prefix>/lib`, `/usr/local/lib`, `/usr/lib`, and a
`build/` tree next to a source checkout). It raises
`adbcbridge.DriverNotFoundError` if none of those has it.

There is a command line tool too:

```sh
adbcbridge query "Driver=SQLite3;Database=my.db;" "SELECT * FROM t"   # --format csv|schema, --limit N, -p PARAM
adbcbridge drivers        # ODBC drivers registered in odbcinst.ini
adbcbridge driver-path    # which libadbc_driver_odbc.so would be used
```

Wheels are pure Python unless a driver library is present at build time, in
which case it is bundled into the wheel and the wheel is tagged for the current
platform:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
python -m build --wheel python     # picks up ./build/libadbc_driver_odbc.so
# or: ADBCBRIDGE_LIBRARY=/path/to/libadbc_driver_odbc.so python -m build --wheel python
```

Details and the package-only README: [`python/README.md`](python/README.md).

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
| `adbc.odbc.array_binding` | `true` (default) — binds each Arrow batch as a column-wise ODBC parameter array, so bulk ingest and `executemany` issue one `SQLExecute` per batch instead of one per row; `false` forces row-at-a-time. Drivers that do not honour `SQL_ATTR_PARAMSET_SIZE`, or that cannot account for every parameter set they were handed, fall back automatically; DuckDB and clickhouse-odbc, whose parameter arrays silently drop values, default to `false` and can be forced back on with this option. Reported rows-affected is identical in both modes. |

Bulk ingest and `executemany` also batch their commits: when the connection is in
autocommit and more than one row is bound, the driver turns autocommit off for the
duration and commits once at the end (rolling back if the execute fails), instead of
paying a commit per row. A transaction the caller opened themselves is left alone.

## Use from C#

```sh
dotnet add package Apache.Arrow.Adbc --version 0.24.0
```

```csharp
using Apache.Arrow;
using Apache.Arrow.Adbc;
using Apache.Arrow.Adbc.C;
using Apache.Arrow.Ipc;

using AdbcDriver driver = CAdbcDriverImporter.Load("/path/to/libadbc_driver_odbc.so");

string uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;";
using AdbcDatabase database = driver.Open(new Dictionary<string, string> { ["uri"] = uri });
using AdbcConnection connection = database.Connect(null);

using AdbcStatement statement = connection.CreateStatement();
statement.SqlQuery = "SELECT * FROM my_table";
QueryResult result = statement.ExecuteQuery();

IArrowArrayStream stream = result.Stream!;
while (await stream.ReadNextRecordBatchAsync() is RecordBatch batch)
{
    Console.WriteLine($"{batch.Length} rows");
}
```

`CAdbcDriverImporter.Load` `dlopen`s the shared library and calls its
`AdbcDriverInit` export, so no driver manager or manifest is involved. Pass a
second argument to use a different entry point.

Parameters are bound as an Arrow `RecordBatch`, one column per `?` and one row
per execution:

```csharp
using Apache.Arrow.Types;

Schema parameters = new Schema.Builder()
    .Field(new Field("id", Int64Type.Default, nullable: true))
    .Field(new Field("name", StringType.Default, nullable: true))
    .Build();
RecordBatch batch = new RecordBatch(
    parameters,
    new IArrowArray[]
    {
        new Int64Array.Builder().Append(1).Append(2).Build(),
        new StringArray.Builder().Append("ada").AppendNull().Build(),
    },
    length: 2);

using AdbcStatement insert = connection.CreateStatement();
insert.SqlQuery = "INSERT INTO my_table (id, name) VALUES (?, ?)";
insert.Prepare();
insert.Bind(batch, parameters);
Console.WriteLine($"{insert.ExecuteUpdate().AffectedRows} rows inserted");
```

See `tests/csharp/` for a runnable example.

## Use from R

```r
install.packages(c("adbcdrivermanager", "nanoarrow"))
```

```r
library(adbcdrivermanager)

drv <- adbc_driver("/path/to/libadbc_driver_odbc.so", entrypoint = "AdbcDriverInit")
db <- adbc_database_init(
  drv,
  uri = "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;"
)
con <- adbc_connection_init(db)

# read_adbc() returns a nanoarrow array stream; as.data.frame() materialises it.
df <- as.data.frame(read_adbc(con, "SELECT * FROM my_table"))

# Parameters are bound as a data frame: a column per '?', a row per execution.
execute_adbc(con, "INSERT INTO my_table (id, name) VALUES (?, ?)",
             bind = data.frame(id = 1:2, name = c("ada", "grace")))

# Bulk ingest: create (or append to) a table from a data frame.
write_adbc(df, con, "my_copy")

adbc_connection_release(con)
adbc_database_release(db)
```

`read_adbc()` and `execute_adbc()` accept the database object directly if you
do not need an explicit connection. Results are nanoarrow array streams, so a
large one can be pulled a batch at a time — `s <- read_adbc(con, "SELECT ...")`
then `s$get_next()` until it returns `NULL` — instead of materialised with
`as.data.frame()`. See `tests/r/` for a runnable example and the docker command
that runs it.

## Use from Java

```xml
<!-- pom.xml -->
<dependency>
  <groupId>org.apache.arrow.adbc</groupId>
  <artifactId>adbc-driver-jni</artifactId>   <!-- ADBC >= 0.21 -->
  <version>0.24.0</version>
</dependency>
<dependency>
  <groupId>org.apache.arrow</groupId>
  <artifactId>arrow-memory-netty</artifactId> <!-- must match ADBC's Arrow -->
  <version>19.0.0</version>
  <scope>runtime</scope>
</dependency>

`adbc-driver-jni` bundles the native ADBC driver manager, so Java loads the
`.so` the same way every other binding does:

```java
Map<String, Object> parameters = new HashMap<>();
JniDriver.PARAM_DRIVER.set(parameters, "/path/to/libadbc_driver_odbc.so");
AdbcDriver.PARAM_URI.set(
    parameters, "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;");

try (BufferAllocator allocator = new RootAllocator();
    AdbcDatabase database = new JniDriver(allocator).open(parameters);
    AdbcConnection connection = database.connect();
    AdbcStatement statement = connection.createStatement()) {
  statement.setSqlQuery("SELECT * FROM my_table");
  try (AdbcStatement.QueryResult result = statement.executeQuery()) {
    ArrowReader reader = result.getReader();
    while (reader.loadNextBatch()) {
      VectorSchemaRoot root = reader.getVectorSchemaRoot();
      System.out.println(root.getRowCount() + " rows");
    }
  }

`PARAM_DRIVER` also accepts a bare library name or a driver-manifest name
(`"odbc"`, see above). Parameters are bound as a `VectorSchemaRoot`, one column
per `?` and one row per execution: `statement.prepare(); statement.bind(root);`
then `executeQuery()` or `executeUpdate()`. Run the JVM with
`--add-opens=java.base/java.nio=ALL-UNNAMED`, which Arrow's off-heap allocator
needs on JDK 17+. See `tests/java/` for a runnable example.

## Test

```sh
python -m venv .venv && .venv/bin/pip install adbc-driver-manager pyarrow
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python tests/test_sqlite.py
```

That the install itself is plug-and-play — install into a temp prefix, then
load the driver by the name `odbc` — is covered by:

```sh
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python tests/test_plug_and_play.py
```

The Python package (`python/`) has its own pytest suite, which also runs
against SQLite:

```sh
pip install -e python
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so .venv/bin/python -m pytest python/tests
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

And from C# (see [`tests/csharp/README.md`](tests/csharp/README.md) for the
docker one-liner, which needs no .NET SDK on the host):

```sh
cd tests/csharp && SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so dotnet test
```

And from R, in docker (see [`tests/r/README.md`](tests/r/README.md)):

```sh
docker build -t adbcbridge-r tests/r
docker run --rm -v "$PWD:/repo:ro" -v /path/to/odbc/drivers:/odbc:ro \
  -e ADBC_ODBC_DRIVER=/repo/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so \
  adbcbridge-r Rscript /repo/tests/r/smoke.R
```

And from Java, in a container (see `tests/java/README.md` for the full command):

  -w /work/tests/java -e ADBC_ODBC_DRIVER=/work/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so maven:3-eclipse-temurin-21 \
  bash -c 'apt-get update -qq && apt-get install -y -qq unixodbc && mvn -B test'

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build/test loop, the
`clang-format` + `pre-commit` setup, and what to include in a bug report.

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
