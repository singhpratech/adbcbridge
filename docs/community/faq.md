<!-- SPDX-License-Identifier: Apache-2.0 -->
# FAQ

Frequently asked questions about adbcBridge, a plain-C11 [ADBC](https://arrow.apache.org/adbc/)
driver that turns any ODBC data source into an Arrow-native one. Answers link to
the fuller documents where they exist. Every figure quoted here is measured; the
host and state behind each number are recorded in the benchmark files under
[`bench/`](../../bench/README.md).

Abbreviations used throughout: **ADBC** — Arrow Database Connectivity, Apache
Arrow's Arrow-native database API; **ODBC** — Open Database Connectivity, the
long-standing C call-level interface that almost every database ships a driver
for; **DDL** — Data Definition Language (`CREATE TABLE`, …); **DML** — Data
Manipulation Language (`INSERT`, `UPDATE`, …); **DSN** — Data Source Name, a
named connection profile in `odbc.ini`; **WAL** — write-ahead log.

---

## General

### What is ADBC, and what does adbcBridge add to it?

ADBC is Apache Arrow's database API: a client asks for rows and gets back Arrow
record batches, columnar and zero-copy, with no per-row conversion. A database
needs an *ADBC driver* to speak it. Only a handful exist natively (PostgreSQL,
SQLite, DuckDB, Snowflake, BigQuery, Flight SQL). adbcBridge is one ADBC driver —
the shared library `libadbc_driver_odbc.so` (`.dylib` on macOS, `.dll` on
Windows) — that fronts *any* ODBC driver, so every database with an ODBC driver
becomes usable from any ADBC binding: Python, R, Go, Rust, C++, C# and Java. See
the [README](../../README.md) for the architecture diagram.

### Why bridge through ODBC at all?

Because the ODBC ecosystem already covers a few hundred data sources — Db2,
Oracle, Teradata, SQL Server, SAP HANA, Informix, Vertica, and the long tail of
enterprise databases that will never get a native ADBC driver. Writing one
C library that drives all of them is far less work than writing one ADBC driver
per database, and it makes those sources available to the whole Arrow ecosystem
at once.

### How is this different from pyodbc, turbodbc, arrow-odbc or a JDBC bridge?

pyodbc returns Python row tuples, so a large result is converted a cell at a time
into Python objects. adbcBridge binds columns and builds Arrow batches directly,
which is both faster and language-neutral: the same driver library serves Python,
R, Go, Rust, C# and Java, not just Python. turbodbc and arrow-odbc solve the same
"ODBC to columnar" problem for a single language (Python and Rust respectively);
adbcBridge solves it once, in C, behind the ADBC application interface, so you are
not locked to one language's binding. A JDBC bridge would need a JVM in every
process — a C-side JDBC bridge is on the [roadmap](../ROADMAP.md) as a *separate*
future driver, not the ODBC one.

### Is it faster than pyodbc?

Yes, measurably, for the Arrow use case. Reading 1,000,000 rows of
`(int, double, varchar(20), date)` from SQLite (median of 5,
[`bench/BENCHMARKS.md`](../../bench/BENCHMARKS.md)):

| Path | Time | Relative |
|---|---:|---:|
| adbcBridge `fetch_arrow_table()` | 0.48 s | 1.0× |
| pyodbc `fetchall()` → `pyarrow.Table` | 1.16 s | 2.4× slower |
| pyodbc `fetchall()` → `pandas.DataFrame` | 1.32 s | 2.8× slower |
| raw `SQLBindCol`+`SQLFetch`, no Arrow (floor) | 0.44 s | 0.93× |

adbcBridge runs within 7% of the raw ODBC floor; the remaining cost is the ODBC
driver itself, not the bridge.

### Is it faster than a native ADBC driver?

For a single connection, no — a native driver talks the wire protocol and builds
Arrow directly, which is the ceiling for one connection. 1,000,000 PostgreSQL
rows take 0.42 s through the native `adbc_driver_postgresql` and 0.67 s through
adbcBridge over psqlodbc; on 10 M rows a single bridge connection is 0.36×
native. The bridge beats the native driver only by doing work it does not:
[splitting one query across several connections](#what-are-partitioned-reads).
At eight partitions on a quiet host adbcBridge reads 1 M rows 1.2–1.5× faster
than the native driver and 10 M rows 1.55× faster; the same 1 M read on a host
with 46 idle containers came in at 0.97×, so plan around the low end. Bulk
*ingest* does not clear parity (0.73–1.02×), because an `INSERT` writes about
twice the WAL of the `COPY` the native driver uses. Where you want native speed
and a native driver exists, adbcBridge hands the whole connection over — see
[native delegation](#what-is-native-delegation).

### Then when should I *not* use adbcBridge?

When a native ADBC driver exists and you are reading through a single connection:
use the native driver (or let adbcBridge
[delegate](#what-is-native-delegation) to it). Replacing native drivers where they
exist is an explicit non-goal in the [roadmap](../ROADMAP.md). adbcBridge is for
everything else — the several hundred ODBC-only sources.

### What is native delegation?

When `AdbcDatabaseInit` recognises a target that a native ADBC driver handles
(a `postgresql://`, `sqlite:`, `duckdb:`, `snowflake://`, `bigquery://` or
`grpc://` URI, or a `Driver=…psqlodbcw.so;…` ODBC string), adbcBridge loads that
native driver and forwards every call to it. The result set is the native
driver's own Arrow stream, handed back untouched: delegation costs one
function-pointer hop per ADBC call and nothing per row, and delegated fetches
measure the same as calling the native driver directly (0.20 s for the million
PostgreSQL rows, against 0.21 s native). It is controlled by
`adbc.odbc.delegate` (`auto` default / `never` / `always`) and is a best-effort
optimisation: if no native driver is installed it falls back to ODBC and records
why in `adbc.odbc.delegate.last_error`. Delegation is not implemented on Windows.
See [Native delegation](../how-it-works/delegation.md).

### Which languages can use it?

Python, R, Go, Rust, C++, C# and Java — anything that speaks the ADBC driver
manager. There are convenience packages for five of them — a Python wheel, a Rust
crate, a NuGet package, a Maven jar and a Go module — attached to the GitHub
Release, and the first three also on PyPI, crates.io and nuget.org. See
[Which package do I install for my language?](#which-package-do-i-install-for-my-language)

---

## Install

### Which ODBC driver manager do I need?

A driver manager is the C library that loads ODBC drivers and dispatches calls to
them. On Linux, install **unixODBC** (`unixodbc-dev`). On Windows, the driver
manager ships with the operating system — nothing to install. On macOS see the
next question.

### Do I need unixODBC on macOS? What about iODBC?

macOS has two driver managers, and they differ in one way that matters:
unixODBC's `SQLWCHAR` (the wide-character unit) is 2 bytes, iODBC's is 4. A driver
compiled against one cannot be loaded through the other. Build adbcBridge against
**unixODBC** for the common drivers (psqlodbc, sqliteodbc, MariaDB Connector/ODBC,
…). A few vendor drivers ship only iODBC builds (MySQL Connector/ODBC for macOS,
OpenSearch's macOS package); for those, build a *second* copy of adbcBridge
against iODBC and use it for those drivers. One bridge build per driver manager.
The full recipe, including relinking a downloaded connector, is in
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md#macos-a-vendor-driver-built-for-iodbc-empty-diagnostics-or-lost-connection-during-query).

### What is the Windows ANSI code-page issue?

The Windows driver manager transcodes every *narrow* (`SQLCHAR`) string between
the driver and the process's ANSI code page (1252 on a Western install), whereas
unixODBC and iODBC pass narrow bytes through untouched. adbcBridge was first
written on the pass-through assumption, so early Windows builds mangled non-ASCII
text. Since build `5b932c5` (2026-08-24) the Windows path goes through the wide
(`W`) entry points and reads every character column as `SQL_C_WCHAR`, which fixes
it. If you build from source, `python tests\test_windows_text.py` must print
`all passed`. Details in
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md#windows-non-ascii-text-arrives-mangled-or-pyarrow-raises-unicodedecodeerror).

### Where is the driver manifest, and what is it for?

A *driver manifest* is a small `odbc.toml` file that lets applications ask for the
driver by the name `odbc` instead of by full path. `./install.sh` writes it to
`~/.config/adbc/drivers/odbc.toml` (a directory the ADBC driver manager already
searches), so `driver="odbc"` resolves with nothing else set — no
`ADBC_DRIVER_PATH`, no `LD_LIBRARY_PATH`. The manifest's absolute library path is
computed at `cmake --install` time, so one build tree can be installed into many
prefixes. On macOS the search directories are under
`~/Library/Application Support/ADBC/Drivers`; on Windows the manager also reads
`HKEY_CURRENT_USER\SOFTWARE\ADBC\Drivers`. See
[Use by name](../reference/install.md#use-by-name-driver-manifest).

### How do I install without root?

Run [`install.sh`](../../README.md#quick-start): it builds and installs into
`~/.local` and writes the manifest to your ADBC user config directory. `PREFIX`,
`MANIFEST_DIR`, `BUILD_DIR`, `BUILD_TYPE` and `JOBS` override the defaults, and
re-running it is safe.

### Which package do I install for my language?

One driver library, five wrapper packages that locate and load it:

| Language | Package | How you name the driver |
|---|---|---|
| Python | `adbcbridge` wheel | `adbcbridge.connect(uri=...)` |
| Rust | `adbcbridge` crate (`bundled` builds it from source) | `adbcbridge::load()?` |
| C# | `AdbcBridge` NuGet | `Driver.Load()`, `Driver.Connect(...)` |
| Java | `org.adbcbridge:adbcbridge` over `adbc-driver-jni` | `AdbcBridge.driver(allocator)` |
| Go | `github.com/singhpratech/adbcbridge/go` over `drivermgr` | `adbcbridge.NewDriver(alloc)` |

All five are built, tested and attached to every
[GitHub Release](https://github.com/singhpratech/adbcbridge/releases/tag/v0.1.0);
the wheel, crate and nupkg are also on PyPI, crates.io and nuget.org (Maven Central
is still to come). Each resolves the library the
same way and raises an error when it cannot; Rust, C#, Java and Go list every
place they looked. See
[Language packages](../../README.md#language-packages).

---

## Usage

### What does a connection string look like?

`uri` is a full ODBC connection string. `Driver=` takes either a registered ODBC
driver name or the path to its `.so`:

```python
import adbc_driver_manager.dbapi as dbapi
conn = dbapi.connect(driver="odbc",
                     db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"})
```

Set database options alongside `uri`: `dsn` (a DSN name from `odbc.ini`, appended
as `DSN=…`), and `username` / `password` (appended as `UID=` / `PWD=`). The full
option table is in [Use (Python)](../languages/python.md).

### DSN or `Driver=` — which should I use?

Either. `Driver=name` names a driver registered in `odbcinst.ini` (or gives its
`.so` path directly); `dsn=name` pulls a whole pre-configured profile from
`odbc.ini`. A path in `Driver=` is the most self-contained and is what a build
tree wants. For [native delegation](#what-is-native-delegation) a DSN works only
if the driver manager can enumerate its keywords.

### How do I pass parameters?

Bind them as an Arrow record batch: one column per `?` placeholder, one row per
execution. In Python this is `cur.execute(sql, parameters=...)` / `executemany`;
in Rust, C#, Java and R it is `prepare` then `bind`. A query you wrote is executed
exactly as written, whatever is bound to it — only the `INSERT` that bulk ingest
generates is ever rewritten.

### What bulk-ingest modes are there?

`adbc_ingest` creates or appends to a table from an Arrow stream. Rather than one
statement per row, it packs K rows into one `INSERT INTO t VALUES (…),(…),…`
inside a single transaction; K is *probed* against the driver and remembered on
the connection. Speed-ups are large — e.g. ClickHouse 16 → 911 rows/s,
Oracle 475 → 23,628, DuckDB 24,057 → 344,597. Two refinements:

- **PostgreSQL** goes a step further and sends each batch as one array parameter
  per column (`INSERT … SELECT * FROM unnest(?::bigint[], …)`), which is on by
  default and narrow by design — only real PostgreSQL, only spellable types, and
  it falls back rather than guessing.
- **Parallel ingest** (`adbc.odbc.ingest_connections=N`) spreads one ingest over
  `N` connections. It is opt-in because it is **not atomic**: `N` connections are
  `N` transactions, so a failure can leave some batches committed.

`adbc.odbc.rows_per_insert` overrides the group size (`1` turns the rewrite off).
Full detail in [Bulk ingest](../how-it-works/performance.md#bulk-ingest).

### How are transactions handled?

Autocommit / commit / rollback all work. Bulk ingest and `executemany` also batch
their commits: in autocommit, when more than one row is bound, the driver turns
autocommit off for the duration and commits once at the end (rolling back on
failure) instead of paying a commit per row. A transaction you opened yourself is
left alone.

### What options help with large result sets?

- `adbc.odbc.batch_size` — rows per Arrow batch (default 1024).
- `adbc.odbc.prefetch` — `0` (default, off) up to `8`; a background thread fetches
  the next rowset while the current one is converted. It is worth little in
  practice because most ODBC drivers have already buffered the whole result set
  client-side; only psqlodbc's cursor mode leaves a real socket wait, and even
  there it buys 6–10%. See [Prefetch](../how-it-works/prefetch.md).
- `adbc.odbc.rowset_bytes`, `adbc.odbc.max_bind_bytes`, `adbc.odbc.long_bind_bytes`
  — control how wide values are bound versus re-read with `SQLGetData`.

For a genuinely large read, [partitioning](#what-are-partitioned-reads) is the
mechanism that pays, not prefetch.

### What are partitioned reads?

ADBC's partition contract: `AdbcStatementExecutePartitions` hands back N opaque
descriptors, and `AdbcConnectionReadPartition` turns any one back into a stream,
on any connection, in any order or process — so N connections read one query in
parallel and the pieces concatenate. adbcBridge splits only what it can *prove*
correct from the catalog (a PostgreSQL heap `ctid`, an integer primary-key range,
or YugabyteDB's tablet hash) and otherwise returns a single descriptor carrying
the original query — never slower than not calling it. It never guesses a
partition column. Set `adbc.odbc.partitions`. See
[Partitioned reads](../how-it-works/partitioned-reads.md).

---

## Compatibility

### Which databases are known to work?

53 databases are verified on Linux by one identical workload, and re-run on macOS (45 pass)
and Windows (48 pass); the full matrix with per-driver notes is in
[`docs/COMPATIBILITY.md`](../COMPATIBILITY.md). It spans SQLite, DuckDB,
PostgreSQL and its wire-compatible forks (CockroachDB, YugabyteDB, TimescaleDB,
Citus, CrateDB, QuestDB, RisingWave, Materialize, openGauss, Cloudberry, YDB,
Spanner via PGAdapter, ArcadeDB), the MySQL-wire family (MySQL, MariaDB, TiDB,
Dolt, Percona, Doris, StarRocks, MatrixOne, GreptimeDB, Databend, OceanBase,
SingleStore, MongoDB BI), SQL Server, Oracle, Db2, IBM Db2 for i, Informix,
SAP HANA Express, Exasol, Altibase, Kinetica, Actian Ingres, MonetDB, Firebird,
Vertica, ClickHouse, Virtuoso, Access, TDengine, Apache Ignite, OpenSearch, and
Arrow Flight SQL servers (sqlflite, InfluxDB 3, Dremio).

### Does a database that is not on the list work?

Probably, on the generic path — adbcBridge can reach anything with an ODBC driver.
But all but a few of the 53 verified drivers needed at least one workaround, so expect
an unlisted driver to work *and* to have a quirk waiting. Reachability is not
verification.

### What does "PASS" mean in the matrix?

That `tests/compat/test_matrix.py` ran one identical ADBC workload against a real
server or file and it all worked: multiple **types** (integers, float/double,
char/varchar, binary, date, time, timestamp, decimal, boolean), **NULLs**,
**Unicode** including an astral-plane (non-BMP) character such as an emoji,
**parameters**, **bulk ingest**, **batched reads**, **metadata** (`GetObjects`)
and **error mapping** (SQLSTATE plus native code). Some entries are marked
"read side only" where the driver has no `SQLBindParameter` or the server has no
`CREATE TABLE`. See [`tests/compat/README.md`](../../tests/compat/README.md).

### Which drivers are known to need quirks, and why?

All but a few of the 53 needed at least one. A few representative ones: **IBM Db2** and **Informix**
ship a 32-bit `SQLLEN` on 64-bit Linux (`adbc.odbc.sqllen_32bit`); **DuckDB**,
**MonetDB**, **ClickHouse**, **Firebird** and others have parameter arrays that
silently drop values, so those are turned off; **Oracle** lacks `SQL_C_SBIGINT`,
so 64-bit ints go as numeric text; **SQL Server** and **Db2** name a deprecated
wide-text type in `SQLGetTypeInfo`, so ingest DDL spells `NVARCHAR(MAX)` /
widest `VARCHAR` instead. Each quirk is keyed on the driver name or server
identity and documented with its measured reason in
[`docs/COMPATIBILITY.md`](../COMPATIBILITY.md).

### Do I need a licence for the vendor ODBC drivers?

adbcBridge itself is Apache-2.0 and redistributes no vendor driver. The vendor
ODBC drivers keep their own licences, and most do not allow redistribution — so
Oracle, Db2, SQL Server, Snowflake, Teradata, SAP HANA and the like stay your
download. The open-licence drivers (sqliteodbc, psqlodbc, MariaDB Connector/ODBC,
clickhouse-odbc) are the ones a planned installer will fetch for you; see the
[roadmap](../ROADMAP.md). Windows already ships the SQL Server driver.

---

## Troubleshooting

### The driver manager says a file is "not found" but it is right there.

unixODBC reports every load failure of a driver library as `file not found`,
discarding the real `dlerror()`. adbcBridge re-opens the path itself and puts the
real reason into the ADBC error — `Permission denied`, a missing dependency, a
32-bit driver in a 64-bit process, or the static-TLS problem below. A file that
genuinely is absent still says `No such file or directory`. See
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md#cant-open-lib-path--file-not-found-for-a-file-that-is-there).

### I get "cannot allocate memory in static TLS block" after importing pyarrow.

This affects exactly one driver in the matrix — MySQL Connector/ODBC — and it
reproduces with plain pyodbc, no ADBC involved. Importing pyarrow pins libstdc++
to dynamic thread-local storage (TLS), after which that driver cannot load.
Loading the ODBC driver *before* pyarrow settles it, which the `adbcbridge`
Python package does automatically. Raising `glibc.rtld.optional_static_tls` does
**not** help. Full account, per-driver table and a `readelf` check in
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md#cannot-allocate-memory-in-static-tls-block).

### On macOS the process dies with SIGABRT on the first SQL error.

The driver is an iODBC-width (4-byte `SQLWCHAR`) build loaded through unixODBC
(2-byte); unixODBC's driver manager overflows a stack buffer reading the wide
diagnostic. Build the bridge against iODBC and use those drivers through it. This
is a driver-manager bug, reported upstream
([lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239)); see
[`docs/TROUBLESHOOTING.md`](../TROUBLESHOOTING.md#macos-the-process-dies-with-sigabrt-on-the-first-sql-error-no-message).

---

## Project

### What licence is adbcBridge under?

Apache-2.0. Every non-vendored source file carries an
`SPDX-License-Identifier: Apache-2.0` header. Vendored Apache Arrow components
(`nanoarrow`, `adbc.h`, `utils.c/h`, `options.h`) are listed in
[`NOTICE`](../../NOTICE).

### What does the 0.1.x version mean?

Early. The read/write path, the 53-database matrix and the five language packages
are done and 0.1.0 is on PyPI, crates.io and nuget.org; a conformance suite and Maven
Central are in progress. `v0.1.0` was tagged 2026-08-25 with four platform libraries,
four wheels plus an sdist, a crate, a NuGet package and a jar. Treat the API as stabilising, not stable. See
the [roadmap](../ROADMAP.md).

### What is on the roadmap?

For the ODBC bridge: a driver-bootstrap installer that fetches the open-licence
drivers, Maven Central publication, the ADBC Driver Foundry validation
suite, and Windows parity for prefetch and parallel ingest. Beyond ODBC: a
**JDBC bridge** (load a JVM in-process and drive any JDBC driver) and, later, an
**OLE DB bridge** for Windows. Full detail and status in
[`docs/ROADMAP.md`](../ROADMAP.md).

### How do I report a bug, and what should I include?

ODBC drivers differ wildly, so a good report names the moving parts. Include: the
ODBC driver and version and the driver manager (unixODBC, iODBC or Windows); the
connection string with secrets removed; the failing SQL and the schema of the
columns involved; and the full ADBC error, including SQLSTATE and the native error
code. If you can, add a case to `tests/test_sqlite.py` that fails before the fix.
See [`CONTRIBUTING.md`](../../CONTRIBUTING.md#reporting-bugs) and
[Contributing](contributing.md). A security problem is the one exception: report it
through GitHub's private vulnerability reporting, not an issue — the policy, the
response times and the support window are in [Security](security.md).

### How are findings given back to upstream projects?

Driving 53 databases across three operating systems turns up defects that belong
to other projects. Each is reported upstream with a reproduction that needs no
adbcBridge in the stack, and the whole record — filed reports and findings
documented but not yet filed — is kept in
[`docs/UPSTREAM.md`](../UPSTREAM.md). Filed as of 2026-09-04: 27 reports across fifteen
projects — unixODBC, OpenLink Virtuoso, Dremio / Arrow Flight SQL ODBC, Firebird
ODBC, taos-odbc, Apache Doris, clickhouse-odbc, MySQL Connector/ODBC, Apache
Ignite, OpenSearch SQL ODBC, QuestDB, CrateDB, psqlodbc, Materialize and
SingleStore Connector/ODBC. Three have already been fixed by their maintainers
(CrateDB, psqlodbc #207 and #208), and unixODBC shipped a mitigation.

### How can I contribute?

Bug reports, driver-specific quirk fixes, type-mapping improvements, docs and
tests against uncovered drivers are all welcome. The build/test loop, coding
conventions, how to add a database or a driver quirk, and the release process are
in [Contributing](contributing.md) and [`CONTRIBUTING.md`](../../CONTRIBUTING.md).
