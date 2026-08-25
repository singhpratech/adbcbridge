# adbcBridge

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

## Machines behind the numbers

Every figure in this repository was measured on one of three ordinary machines — no
cloud instances, no dedicated benchmark hosts — and the files say which. Read the
numbers with these in mind:

| | Linux (reference host) | macOS | Windows |
|---|---|---|---|
| Machine | laptop, Intel Core i9-13900HK, 14 cores / 20 threads, 31 GiB | Apple M4 Max, 16 cores, 64 GB, arm64 | laptop, Intel Core i9-13900HK, 14 cores / 20 threads, 32 GB |
| OS | Linux Mint 22.3, kernel 7.0 | macOS 26.5.2 | Windows 11 Home 23H2 (build 22631) |
| Driver manager | unixODBC 2.3.12 | unixODBC 2.3.12 from source (iODBC from source for iODBC-only vendor drivers) | the OS's own (odbc32), ANSI code page 1252 |
| Compiler | gcc 13.3 | Apple clang 21 | MSVC 19.44 (VS 2022 Build Tools) |
| Databases | 46 of 46, Docker containers on the same machine (the full fleet often idling alongside) | 46 of 46 results: 41 pass, 5 without an obtainable driver or runnable server; servers in Docker Desktop, some amd64 images under emulation | 46 of 46 results: 45 pass, servers in Docker Desktop on WSL2 (20 GB VM cap) |
| Load during runs | never idle: ~23 GiB of other work resident, 1-minute load typically 2–5, CPU governor `powersave` after a reboot | 1-minute load 2.4–10.5, recorded per entry | up to 8 benchmark runs and ~30 idle containers at once; single samples |
| What the build lacks | — | — | prefetch pipeline and parallel ingest (pthreads, compiled out on `_WIN32`) |

What this means for the headline figures: the PostgreSQL-vs-native ratio (1.2–1.5× fetch)
holds on the Linux host when it is quiet and drops to 0.97× with 46 containers idling and
0.60× on the M4 Max; per-language rates agree within a band on each machine but are not
comparable across machines; and Windows rows measure a materially different code path
(no prefetch, no fan-out), and were taken with several runs and containers sharing the
box. Each benchmark file opens with the
exact host state of its runs (`bench/BENCHMARKS.md`, `bench/BENCHMARKS-macos.md`,
`bench/BENCHMARKS-windows.md`).

## Performance

1,000,000 rows `(int, double, varchar(20), date)` from SQLite, median of 5 (`bench/BENCHMARKS.md`):

| Path | Time | Relative |
|---|---:|---:|
| **adbcbridge `fetch_arrow_table()`** | **0.48 s** | 1.0× |
| pyodbc `fetchall()` → `pyarrow.Table` | 1.16 s | 2.4× slower |
| pyodbc `fetchall()` → `pandas.DataFrame` | 1.32 s | 2.8× slower |
| raw `SQLBindCol`+`SQLFetch`, no Arrow (floor) | 0.44 s | 0.93× |

The bridge runs within 7% of the raw ODBC floor; the remaining cost is the ODBC driver itself.

That floor is also the ceiling for a single connection, so beating a native ADBC driver
means doing work it does not: splitting one query across several connections. Against
the native `adbc_driver_postgresql`, adbcbridge on one connection is 0.30–0.36× on 10 M
rows; at eight partitions it reads 1 M rows **1.2–1.5× faster on a quiet host** and
10 M rows 1.55× faster — mean of three, a fresh process per run, sides interleaved,
every read checksum-compared against a reference. The same 1 M read on a host with
46 idle containers came in at 0.97×, so the number to plan around is the low end, and
bulk *ingest* does not clear parity at all (0.73–1.02×: an `INSERT` writes twice the
WAL of the `COPY` the native driver uses). See
[Partitioned reads](#partitioned-reads-executepartitions) and `bench/BENCHMARKS.md`.

### Bulk ingest

`adbc_ingest` sends one `INSERT INTO t VALUES (…),(…),…` per K rows rather than one
statement per row, inside a single transaction. K is probed against the driver — SQLite's
999-variable limit, ClickHouse preparing 500 row-groups and then refusing to execute them,
and Oracle rejecting the multi-row form outright (it falls back to `INSERT ALL … SELECT 1
FROM dual`) are all discovered at run time and remembered on the connection. It works on
every driver that can bind a parameter, including the six that mishandle ODBC parameter
arrays, and `adbc.odbc.rows_per_insert` overrides the choice. 10,000 rows, rows/s:

| Database | one statement per row | multi-row |
|---|---:|---:|
| ClickHouse 26 | 16 | **911** |
| Oracle 23ai | 475 | **23,628** |
| DuckDB | 24,057 | **344,597** |
| CockroachDB 26 | 1,205 | **14,259** |
| MonetDB 11.55 | 12,190 | **138,902** |
| MySQL 8.4 (50,000 rows) | 20,784 | **200,247** |
| PostgreSQL 16 | 102,508 | **312,025** |
| SQLite 3.45 (50,000 rows) | 634,417 | **866,080** |
| StarRocks 4.1.4 | 10 | **4,783** |
| Apache Doris 2.1.0 | 7 | **2,184** |
| CrateDB 6.4 | 820 | **49,986** |
| GreptimeDB 1.1.4 | 5,171 | **180,760** |

#### PostgreSQL: one array parameter per column

Against PostgreSQL the driver goes one step further and sends a whole *column* of a batch
as a single array parameter, letting the server expand it:

```sql
INSERT INTO t ("a", "b", "c", "d")
  SELECT * FROM unnest(?::bigint[], ?::float8[], ?::text[], ?::date[])
```

That is one parameter per column however many rows the statement carries (10,000 by
default), instead of one bound cell per value. On 1,000,000 four-column rows it takes the
single-connection ingest from 2.75 s to 1.40 s and the sixteen-connection ingest from
0.45 s to 0.34 s, with about a third less CPU on both sides. It is on by default and needs
no option.

It is deliberately narrow, because a wrongly quoted array literal is a data-corruption bug
rather than a slow one:

* **Only PostgreSQL.** psqlodbc drives every PostgreSQL-wire server, so the quirk is keyed
  on what the server says it is, not on the driver: `version()` has to be a PostgreSQL
  banner and must not carry a fork's marker. Checked against live servers: on for
  PostgreSQL itself, TimescaleDB and Citus (both of which *are* PostgreSQL, with an
  extension); off for CockroachDB, YugabyteDB, CrateDB, RisingWave, Materialize, openGauss
  and Apache Cloudberry. QuestDB, ArcadeDB, YDB and Cloud Spanner via PGAdapter are
  excluded by the same rule without having been tried. Before the form is used on a
  connection the server is also asked to prove it: one statement whose answer pins down
  NULL elements, empty elements, and `,`, `{`, `}`, `"` and `\` inside a quoted element.
  (That check on its own is not enough — CockroachDB passes it — which is why the identity
  test comes first and unknown servers default to off.)
* **Only the types it can spell exactly**: the integers, `float16/32/64`, `decimal128/256`,
  `bool`, `date32`, `time32/64`, `timestamp` and the string types (including
  `large_string` and `string_view`). A batch with a binary, interval, list, struct, map,
  dictionary or null-typed column keeps the multi-row `INSERT` path in full.
* **It falls back rather than guessing.** A target column PostgreSQL will not
  assignment-cast to (an Arrow `date` against a `text` column, say) is refused at
  `SQLPrepare`, before anything has been applied, and the ingest replays on the multi-row
  path. A single value the renderer cannot spell — a string with an embedded NUL, a date or
  timestamp outside years 0001–9999 — stops the array form at that row: the rows before it
  are already in, and the rest of the ingest goes the ordinary way. Nothing is written
  twice and nothing is dropped.

`adbc.odbc.rows_per_insert=1` turns it off along with the multi-row form.


#### Parallel ingest: trading atomicity for speed

`adbc.odbc.ingest_connections` spreads one ingest over several connections. One thread
drains the bound stream and hands batches to `N` workers, each with its own connection,
statement handle and transaction, each running the ordinary ingest path into the same
table — the array form above where the server is PostgreSQL, the multi-row `INSERT`
everywhere else. On PostgreSQL it takes 1,000,000 rows from 1.40 s to 0.34 s; the curve
flattens at 12–16 workers. (With the multi-row form, which is what every other server
gets, the same load went from 2.20 s to 0.505 s.)

**It is opt-in because it is not atomic, and it defaults to `1`.** `N` connections are `N`
transactions. A worker that fails trips the queue, so every worker still running fails its
own read of the stream and rolls its share back — but a worker that had already reached the
end of the queue has already committed, and those rows stay in the table. What the caller
gets is an error naming the worker and the server's complaint; what is left behind is some
unspecified subset of the stream. Never a corrupt or invented row, and never one that
violates a constraint — but not all-or-nothing either. Use `N > 1` only where a partially
populated table on failure is acceptable, or where the caller drops and retries. `N = 1` is
today's behaviour and today's atomicity, unchanged.

Two further things to know. Fanning out needs the target table to be visible to the worker
connections, so when the caller is inside its own transaction (autocommit off) the
`CREATE TABLE` is uncommitted and invisible; the driver quietly keeps the ingest on one
connection instead, which is correct and atomic and merely slower. And it is a
server-side-parallelism play: on a single-writer database it backfires — SQLite's ingest
goes from 0.224 s to 0.273 s on four connections, the workers contending for the one write
lock.

Even at its best this does not catch the native PostgreSQL driver, which ingests with
`COPY … (FORMAT binary)`. The array form closes most of the gap — at 1,000,000 rows and
sixteen connections the two are within noise of each other (0.9–1.0x over repeated runs,
best 0.317 s against 0.322 s), up from 0.77x — but not all of it, and at 10,000,000 rows it stays about 1.4x behind. The
reason is not the statement shape: an `INSERT` writes **twice the WAL** a `COPY` does
(96.4 MB against 48.8 MB for the same million rows), because `COPY` batches tuples into
one WAL record per page and `INSERT` writes one per row. That is what caps the parallel
curve — over half of the backends' time at N=16 is spent waiting on WAL and buffer locks —
and no statement an ODBC driver can send gets underneath it, `COPY` itself being
unreachable through the ODBC API. See [bench/BENCHMARKS.md](bench/BENCHMARKS.md) for the
measurements. A caller who needs native ingest speed against PostgreSQL should let the
driver [delegate](#native-delegation) to `adbc_driver_postgresql`.

MariaDB and Vertica keep ODBC parameter arrays, which are faster there. Firebird has no
multi-row `VALUES` in its dialect and takes a `UNION ALL` of typed one-row `SELECT`s
instead (5,974 → 7,924 rows/s).

### Rust

`bench/rust/` runs the same read and bulk-ingest workload from Rust, comparing the bridge
against the [`odbc-api`](https://crates.io/crates/odbc-api) and
[`arrow-odbc`](https://crates.io/crates/arrow-odbc) crates talking to the same ODBC driver.
Per-database results are in [`bench/RUST_BENCHMARKS.md`](bench/RUST_BENCHMARKS.md).

### Every language

`bench/csharp/`, `bench/java/` and `bench/go/` run that same ingest-and-fetch workload from
C#, Java and Go. [`bench/LANGUAGE_BENCHMARKS.md`](bench/LANGUAGE_BENCHMARKS.md) puts all five
bindings — Python, Rust, C#, Java and Go — side by side on the same table, so you can see how
much of the cost is the driver and how much is the language's driver manager.

## Compatibility matrix

adbcbridge can reach anything with an ODBC driver — the ODBC ecosystem covers a few
hundred data sources — but that is reachability, not verification. What has actually
been verified is the table below: the same workload (types, NULLs, Unicode incl. emoji,
parameters, bulk ingest, batched reads, GetObjects, error mapping) run through
`tests/compat/test_matrix.py` against a real server or file. Every one of these drivers
needed at least one workaround, so expect an unlisted driver to work on the generic path
and to have a quirk waiting; [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) tracks what
is verified, what is queued, and what only exists as a hosted service.

| Database | ODBC driver | Status |
|---|---|---|
| SQLite 3.45 | sqliteodbc 0.99991 | PASS |
| DuckDB (latest) | duckdb-odbc | PASS (driver quirks handled: 2048-row vectors, no `SQL_BIT` params, no usable parameter arrays) |
| PostgreSQL 16 | psqlodbc 16 | PASS |
| MariaDB 11 | MariaDB Connector/ODBC 3.1 | PASS |
| MariaDB ColumnStore 23.02 (MariaDB 11.1) | MariaDB Connector/ODBC 3.1 | PASS (driver quirk handled: ColumnStore rejects `maodbc`'s own `LONG VARCHAR`/`BIT` type names, so ingest DDL falls back to standard SQL names; server side: no `VARBINARY` column type, and `columnstore_cache_inserts=ON` is what makes parameterised inserts more than ~2 rows/s) |
| SQL Server 2022 | msodbcsql 18 | PASS (incl. `NVARCHAR(MAX)` via chunked `SQLGetData`; driver quirk handled: `SQLGetTypeInfo(SQL_LONGVARCHAR)` names the deprecated `TEXT`, which SQL Server will not sort, group or even compare, so ingest DDL spells an Arrow string `NVARCHAR(MAX)`) |
| Oracle 23ai Free | Instant Client ODBC 23 | PASS (set `NLS_LANG=.AL32UTF8` for non-ASCII; 64-bit ints sent as numeric text — driver lacks `SQL_C_SBIGINT`) |
| ClickHouse 26 | clickhouse-odbc 1.5 | PASS (NULL params need `SQLDescribeParam`; no affected-row counts; `Nullable()` DDL wrapper on ingest; no usable parameter arrays) |
| MySQL 8.4 | MySQL Connector/ODBC 9.4 (and MariaDB Connector/ODBC 3.1) | PASS |
| Dolt 2.3.1 (MySQL 8.0.33 wire protocol) | MySQL Connector/ODBC 9.4 | PASS (no driver quirks; Dolt offers only `mysql_native_password`, which Connector/ODBC 9.x loads as a plugin — the entry points `PLUGIN_DIR` at the tarball's own `lib/plugin`) |
| Percona Server 8.4 | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (no quirks; a drop-in MySQL fork, so the `mysql` entry applies unchanged) |
| CockroachDB 26.3 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; declare a PRIMARY KEY or the synthesised hidden `rowid` shows up in `GetObjects`) |
| YugabyteDB 2026.1 (YSQL) | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; YSQL is PostgreSQL 15, and its internal row id is a system column so `GetObjects` is unaffected) |
| TiDB 7.5 | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (no quirks; run from the tarball it needs `PLUGIN_DIR=` for the `mysql_native_password` client plugin TiDB's root account uses) |
| MonetDB 11.55 (Dec2025-SP3) | MonetDBODBClib 11.55 | PASS (driver quirk handled: no usable parameter arrays — executes only the first set) |
| TimescaleDB 2.29 (PostgreSQL 16) | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; also ingests into and reads back a `create_hypertable()` hypertable) |
| Citus 14.1 (PostgreSQL 18) | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; also ingests into and reads back a `create_distributed_table()` hash-distributed table — the one-node cluster has to register itself as a worker first) |
| CrateDB 6.4 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (driver quirk handled: psqlodbc reports no row count inside a transaction; server side: eventually consistent, so reads follow `REFRESH TABLE`, and it has no binary and no `DATE` column type) |
| QuestDB 10 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (quirks handled: ingest DDL in standard SQL type names, boolean params as `true`/`false` text, no usable parameter arrays; `GetObjects` falls back to `SQLDescribeCol` because psqlodbc's `SQLColumns` fails here) |
| RisingWave 3.0 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no driver quirks; server side: RisingWave's parser takes no type modifiers, so `VARCHAR`/`NUMERIC` are declared unqualified, and a write is visible to a scan only after `FLUSH`) |
| IBM Db2 12.1 | Db2 CLI driver (clidriver `libdb2.so`) | PASS (driver quirks handled: 32-bit `SQLLEN` — see `adbc.odbc.sqllen_32bit` — and `SQLGetTypeInfo(SQL_LONGVARCHAR)` naming `LONG VARCHAR`, which Db2 deprecated, will not sort or group on, and writes ~700x slower than a `VARCHAR`, so ingest DDL asks for the widest `VARCHAR` instead) |
| Firebird 5 | Firebird ODBC 3.5.0-rc1 | PASS (driver quirks handled: `SQL_C_WCHAR` sized in 4-byte `wchar_t`, no usable parameter arrays) |
| Databend 1.2 | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (server has no prepared statements, so the connector runs with `NO_SSPS=1`; driver quirks handled: `_binary` literals for date/timestamp/binary params, MySQL type names in ingest DDL) |
| Azure SQL Edge 16.0 | msodbcsql 18 | PASS (the SQL Server 2022 engine, so it takes the same path as SQL Server 2022, including the `TEXT` ingest-DDL quirk — it even reports `SQL_DBMS_NAME` "Microsoft SQL Server") |
| OpenLink Virtuoso 7.2 | Virtuoso ODBC (`virtodbc.so`, ANSI build) | PASS (driver quirks handled: no `SQL_C_WCHAR` — UTF-8 on the narrow path instead, 64-bit ints sent as numeric text, no usable parameter arrays for dates; the Unicode build `virtodbcu.so` crashes unixODBC's ANSI translation on the first failed statement) |
| Materialize 26.38 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no driver quirks; its SQL layer is PostgreSQL's, but it has no `SAVEPOINT`, so the entry sets psqlodbc's `Protocol=7.4-0` to stop the driver wrapping the second batch of a large ingest in one; its single 39-digit `NUMERIC` is wider than an Arrow decimal128, so decimals read back as exact strings; also ingests into and reads back an incrementally maintained `MATERIALIZED VIEW`) |
| openGauss 6.0 | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no quirks; a PostgreSQL 9.2 fork, so the `postgres` entry applies unchanged — the work is all server-side setup: the container needs `CAP_SYS_NICE`, and the initial user cannot log in remotely, so the matrix connects as a role created after start-up) |
| Apache Cloudberry 2.1.0-incubating (Greenplum fork) | psqlodbc 16 (PostgreSQL wire protocol) | PASS (no driver quirks and no tolerance flags; an MPP cluster of PostgreSQL 14 segments behind one coordinator, so the `postgres` entry applies unchanged — and since it reports `SQL_DBMS_NAME` "PostgreSQL" behind the same `psqlodbcw.so`, no driver-name quirk *could* be correct here without also firing on real PostgreSQL. The work is server-side: no Apache-published server image exists (`apache/incubator-cloudberry` ships only CI build/test toolchains), so a community image runs the released 2.1.0, and the segments need `--shm-size=1g` or `gpinitsystem` fails. Beyond the standard workload the entry checks the two things a single-node server has no answer for: a `DISTRIBUTED BY` table whose bulk-ingested rows really occupy more than one segment, and append-optimized column-oriented storage — `ao_column`, read from `pg_am` now that PostgreSQL 14 table access methods have replaced Greenplum's `relstorage`) |
| MatrixOne 4.2 (MySQL 8.0.30 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (driver quirk handled: MatrixOne describes a TEXT column as five characters however long its values are, so a no-declared-length column is bound at `long_bind_bytes` instead of re-reading every row — 3k rows/s before, 2.05M after; run from the tarball it needs `PLUGIN_DIR=` for the `mysql_native_password` client plugin; server side: declare a PRIMARY KEY or the hidden `__mo_fake_pk_col` shows up in `GetObjects`, and a parameter bound into a `BIT` column aborts the server, so ingest sends booleans as `TINYINT`) |
| OceanBase CE 4.4.2 (MySQL 5.7.25 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (no driver quirks and no tolerance the `mysql` entry does not already need — a distributed HTAP engine whose MySQL mode takes that entry's types unchanged; it is multi-tenant, so the login name carries the tenant (`User=root@test`), and run from the tarball it needs `PLUGIN_DIR=` for the `mysql_native_password` client plugin. Server side: the container needs `--ulimit nofile=20000` (`obd` refuses to deploy below it) and `MODE=SLIM`, which boots a prebuilt cluster — the default `MODE=MINI` creates its tenant from scratch and times out loading 118,610 timezone rows) |
| IBM Informix 15.0.1 (developer edition) | Db2 CLI driver (clidriver `libdb2.so`, DRDA) | PASS (driver quirks handled: no usable `SQL_C_WCHAR` parameters — UTF-8 on the narrow path instead — and `SQL_C_BIT` parameters break the DRDA stream, so booleans go as integers; 32-bit `SQLLEN` as for Db2; the clidriver names Informix's `BYTE` with IBM's own `SQL_BLOB` type code. The same `libdb2.so` drives Db2, so these are keyed on `SQL_DBMS_NAME` "IDS", not the driver name. Server side: `GL_USEGLU=1` for four-byte UTF-8, `DELIMIDENT=y` for the quoted identifiers ingest emits, and `DATETIME YEAR TO FRACTION(5)` timestamps) |
| Google Cloud Spanner (emulator) | psqlodbc 16 via PGAdapter 0.55 (PostgreSQL wire protocol) | PASS (driver quirks handled: psqlodbc executes a parameter array by inlining the values, writing a bound timestamp as `'...'::timestamp` — a type Spanner does not have — so a batch that binds a timestamp goes one row at a time; and every Spanner table must have a PRIMARY KEY, so generated ingest DDL adds a surrogate `GENERATED BY DEFAULT AS IDENTITY` key column. Both are keyed on a setting only PGAdapter has, since `version()` just says PostgreSQL 14.1. Server side: no 32-bit integer, no `TIMESTAMP WITHOUT TIME ZONE`, no modifier on `NUMERIC`; also ingests into and reads back an `INTERLEAVE IN PARENT` child table) |
| Arrow Flight SQL (sqlflite 1.5.5, DuckDB 1.1.1) | Arrow Flight SQL ODBC 0.9.7 (Dremio) | PASS, read side only — the driver has no `SQLBindParameter` at all, so nothing can be written through it (driver quirks handled: `SQLColumns` segfaults on the first `SQLFetch`, so `GetObjects` describes a zero-row SELECT instead; every `DECIMAL` is described as `(19, 0)`, so decimals are read as exact text) |
| GreptimeDB 1.1.4 (MySQL 8.4.2 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (a time-series store: every table must declare a `TIME INDEX` column, so generated ingest DDL adds one that defaults to the insert time and creates the table in append mode — without it GreptimeDB merges rows sharing a timestamp; its prepared-statement metadata describes every parameter as a string and then refuses one, so the connector runs with `NO_SSPS=1` and the Databend `_binary` quirk applies. Its PostgreSQL wire (4003) is *not* reachable: psqlodbc's connect handshake asks for `show transaction_isolation`, which GreptimeDB does not implement) |
| InfluxDB 3 Core (Arrow Flight SQL) | Arrow Flight SQL ODBC 0.9.7 (Dremio) | PASS, read side only — a second Flight SQL server behind the same driver, and it needed no new quirk (the `SQLColumns` one is shared with sqlflite). The entry cannot write for two independent reasons: InfluxDB 3's SQL is query-only — tables come into existence when line protocol is written to them, over the HTTP API — and the driver has no `SQLBindParameter`. Server side: a table is tags, fields and a nanosecond `time` column that is always spelled `time`, with no `DATE`, `DECIMAL` or binary type, so the entry reads `time` as `ts` and casts the date back in its own `SELECT` |
| StarRocks 4.1.4 (MySQL 8.0.33 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (an MPP columnar warehouse behind the MySQL wire: it prepares nothing but `SELECT`, so the connector runs with `NO_SSPS=1`, and the `_binary` literals it then writes for date/timestamp/binary parameters are sent as ordinary quoted text instead — that quirk, `temporal_binary_param_as_varchar`, was already documented and set but its implementation had been lost in a bad merge, so this restores it; ingest DDL falls back to standard SQL type names, whose fallback for a double is now `DOUBLE` rather than the ISO `DOUBLE PRECISION`, which StarRocks does not parse. Server side: no `ANSI_QUOTES` mode at all, so ingest quotes with backticks — the driver already asks for `SQL_IDENTIFIER_QUOTE_CHAR` — `DECIMAL(10,3)` is described at MySQL's display width `(12,3)`, and every `INSERT` is a load transaction costing a flat ~100 ms, so ingest is 10 rows/s for every client) |
| Apache Doris 2.1.0 (MySQL 5.7.99 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS (an MPP warehouse: every OLAP table has to declare how its rows are distributed, so generated ingest DDL appends `DISTRIBUTED BY RANDOM BUCKETS AUTO` and asks for a duplicate table with no key columns — without the latter Doris refuses any table whose first column is a string, float or double. Doris calls itself MySQL 5.7.99, so the quirk is keyed on `@@version_comment`. It reports no transaction support, which already brings in the Databend quirks (`_binary` parameter literals rewritten as text, portable ingest type names); server-side prepare handles only point `SELECT`s, so the connector runs with `NO_SSPS=1`. Server side: no binary column type, no `DOUBLE PRECISION` spelling, and `ANSI_QUOTES` is accepted but ignored, so identifiers are backtick-quoted) |
| Dremio 26.0.5 (OSS, Arrow Flight SQL) | Arrow Flight SQL ODBC 0.9.7 (Dremio) | PASS, read side only — the engine this driver was written for, and it needed no new quirk (the `SQLColumns` one is shared with sqlflite and InfluxDB 3). The server is not the limit here — `$scratch`, the writable source a stock dremio-oss ships, takes `CREATE TABLE ... AS SELECT`, and a table created there with a column list is an Iceberg table that takes `INSERT` — the driver is: `SQLBindParameter` is `HYC00 "Unsupported function"` even after a `SQLPrepare` that succeeds, so no parameter can reach the server and the entry builds its tables with literal CTAS. Unlike sqlflite this driver reports the declared *scale* of a `DECIMAL` (precision is still always 19), so decimals arrive exact rather than as text. Server side: the first admin user has to be created over the REST API before any login works, the query context comes from a `schema` connection property the driver forwards as a gRPC header, and a string literal containing an astral-plane character has to be written `_UTF8'…'` — Dremio's parser encodes an unprefixed one as ISO-8859-1 and fails planning |
| Microsoft Access `.mdb`/`.accdb` | MDB Tools 1.0 (`odbc-mdbtools`) | PASS, read side only — the driver executes no DDL/DML and has no `SQLBindParameter` (32-bit `SQLLEN`, as Db2) |
| ArcadeDB 26.9 (PostgreSQL wire protocol) | psqlodbc 16 (PostgreSQL wire protocol) | PASS, read side only — ArcadeDB has no `CREATE TABLE` at all (a table is a document type plus one `CREATE PROPERTY` per column), so `adbc_ingest`'s generated DDL has nowhere to go; queries, parameters and the catalog all work (driver quirks handled: psqlodbc's `SQLColumns` query is one ArcadeDB's parser rejects, and it answers success with an empty result, so `GetObjects` describes a zero-row SELECT instead; `SQLTables(SQL_ALL_TABLE_TYPES)` is likewise rejected, so `GetTableTypes` falls back to the types the server's own tables have. Server side: `BoolsAsChar=0`, timestamp literals only in ISO-8601 `T` form, and `@rid`/`@type`/`@cat` in every `SELECT *`) |
| Apache Ignite 2.17 | Ignite ODBC (built from the C++ sources the image ships) | PASS (driver quirks handled: `SQLBindParameter` refuses `SQL_WVARCHAR` outright and the driver's `SQL_C_WCHAR` buffers are `wchar_t`-sized, so strings take the UTF-8 narrow path as for Firebird; its column-wise parameter arrays test the NULL indicator of row 0 for every row, so a NULL below the first row is sent as garbage and the node dies with an `OutOfMemoryError` — parameter arrays are off here. Apache publishes no prebuilt Linux driver: `platforms/cpp` is built root-free with `-DWITH_ODBC=ON -DWITH_CORE=OFF`, which needs no JVM. Server side: every table is a cache, so it must declare a `PRIMARY KEY` — the generated ingest DDL cannot, so `adbc_ingest(mode="create")` is impossible and the entry appends into a keyed table instead; identifiers fold to upper case and the driver reports no identifier quote character) |
| OpenSearch 3.8 (SQL plugin, `/_plugins/_sql`) | OpenSearch SQL ODBC 1.6 (built from source — the project ships Windows and macOS binaries only) | PASS, read side only — the SQL plugin is a query interface (no `CREATE TABLE`, no `INSERT`: documents are written over the REST `_bulk` API) *and* the driver answers `SQLBindParameter` with "OpenSearch does not support parameters", so either reason alone makes the entry read-only (driver quirk handled: its **ANSI `SQLDriverConnect` cannot connect at all** — `CC_connect()` asks the server for the `SQL_ASCII` client encoding, which it does not support, unless `SQLDriverConnectW` set the unicode-driver flag first — and it fails with an empty diagnostic queue, so adbcbridge retries a connect that failed without saying why through `SQLDriverConnectW` — one that reported a real error, bad credentials say, is left alone. The Linux build itself needed three source fixes to a POSIX branch that had never been compiled, one of them a `sem_init()` given the semaphore's capacity as its initial count, which corrupts the result queue and segfaults on close. Server side: backtick identifiers, no `DECIMAL` and no binary type, and the driver's type table has no entry for the plugin's `timestamp`, so `ts` is described as a VARCHAR and read as text) |
| YDB 23.4 (PostgreSQL wire protocol) | psqlodbc 16 (PostgreSQL wire protocol) | PASS (driver quirks handled: every YDB table must declare a PRIMARY KEY, and no ingested column can be one -- a YDB key is implicitly NOT NULL -- so generated ingest DDL appends `adbc_pk SERIAL PRIMARY KEY`; `pg_catalog.pg_attribute` is empty, so psqlodbc's `SQLColumns` answers success with zero rows and `GetObjects` describes a zero-row SELECT instead. `version()` reports a plain PostgreSQL 16 banner, so both are keyed on the one thing that gives YDB away over ODBC: it answers `SHOW server_version` with that banner rather than a version number. Server side: its PG wire has no NULL bind parameter at all -- a `-1` parameter length is read as a zero-length value, silently storing `''` in a text column -- so the entry sets psqlodbc's `UseServerSidePrepare=0` and every NULL goes as a literal; the image ships no users and no environment variable for the PG feature flags, both of which the setup notes cover) |
| Vertica 25.3 (OpenText Analytics Database) | Vertica ODBC 25.1 (`libverticaodbc.so`, native wire protocol) | PASS — the one entry that needed no tolerance flags at all: every workload type is a native Vertica type and round-trips exactly, emoji and microseconds included. Its own protocol on 5433 is not a PostgreSQL wire, despite the port and the shared ancestry. (Driver quirk handled: Vertica's parameter arrays are a single native bulk load and beat the default multi-row `INSERT` eightfold — 17.1k rows/s against 139k — so it opts into `prefer_param_arrays`, the flag MariaDB Connector/ODBC already uses. The driver also refuses to load without a `vertica.ini` of its own, whose `DriverManagerEncoding = UTF-16` is what matches unixODBC's 2-byte `SQLWCHAR`; its default is UTF-32. Server side: `vertica/vertica-ce` is gone from Docker Hub, so the matrix runs `opentext/vertica-k8s` and creates the database with `vcluster` itself, pinned to 25.x — Vertica 26.1 dropped Community Edition and refuses the licence its own image ships) |
| TDengine 3.3.6 | taos-odbc (TDengine's own connector, built from source) | PASS, read side only — a TDengine table must start with a TIMESTAMP primary key that is non-NULL, distinct and inside the retention window, which neither the workload's positional `INSERT` nor `adbc_ingest`'s generated DDL can produce, so the entry reads tables its `setup` builds and bulk-ingests through `extra` into a timestamp-first table (driver quirks handled: no `SQL_C_TYPE_TIMESTAMP` conversion in either direction, so timestamp columns are read as text and timestamp parameters sent as text; a boolean parameter is taken only as `SQL_TINYINT`; the driver implements no `DECIMAL`, so that column is exact text) |
| MongoDB 7 + BI Connector 2.14 (MySQL 5.7.12 wire protocol) | MySQL Connector/ODBC 9.4 (MySQL wire protocol) | PASS, read side only — `mongosqld` presents MongoDB collections as SQL tables over the MySQL wire, and it is a query engine only (no DDL, no DML at all), so the entry's two collections are loaded into MongoDB with mongosh and the read side runs unchanged (driver quirk handled: `SQLColumns` *segfaults* inside Connector/ODBC for any table with a `DECIMAL` column — mongosqld's `information_schema` reports a NULL `NUMERIC_PRECISION` and the connector runs `strtol()` on it — so `GetObjects` describes a zero-row SELECT instead, the same `no_sql_columns` path the Flight SQL driver takes. Server side: `PLUGIN_DIR=` for `mysql_native_password` (its absence is another segfault, in the handshake), `NO_SSPS=1` because `COM_STMT_PREPARE` is refused, no binary and no DATE type, and an `_id` column in every `SELECT *`) |

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
| `adbc.odbc.max_bind_bytes` | widest value bound at the width the driver declares for it, in bytes (default 32768). Wider ones are bound at `long_bind_bytes` instead, or read with `SQLGetData` where that is not possible |
| `adbc.odbc.long_bind_bytes` | width, in bytes, to bind a column whose declared width is not a real bound — a `TEXT`/`NVARCHAR(MAX)`/`LONGTEXT`/`bytea` column, which drivers describe by what the *type* could hold (past `max_bind_bytes`), or by nothing at all (a width of 0, which is how psqlodbc describes PostgreSQL's `bytea`); default 2048. Values longer than this are read again in full, so this trades nothing but speed. Where enough of them turn out not to fit — the reader watches the first 256 rows and compares the bytes it had to re-read against what the block cursor is worth — it stops binding that column for the rest of the result set and reads it with `SQLGetData` instead, since a buffer the values do not fit in costs more than it saves |
| `adbc.odbc.rowset_bytes` | ceiling on a reader's bound rowset buffers, in bytes (default 8388608). The rowset holds `batch_size` rows unless that would cost more than this, in which case it holds as many as fit |
| `adbc.odbc.decimal_as_string` | `true` to return DECIMAL/NUMERIC as strings |
| `adbc.odbc.partitions` | how many partitions `AdbcStatementExecutePartitions` should split a query into — `0` (default) chooses from the table's size (its block count, its row estimate, or the span of its key, whichever the chosen strategy has), `1` never splits. A count the driver cannot honour is reduced, never faked: it will not hand out a slice that reads nothing. Set on the statement. See [Partitioned reads](#partitioned-reads-executepartitions) |
| `adbc.odbc.prefetch` | rowsets kept in flight on a background fetch thread, so `SQLFetch` for the next one overlaps the Arrow conversion of the current one — `0` (default) is off, `1` is double buffering, up to `8`. Settable on the database, the connection or the statement. See [Prefetch](#prefetch) |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — see [Native delegation](#native-delegation) |
| `adbc.odbc.delegate.driver` | force a specific native driver: a bare name (`postgresql`) or manifest name; a path only with `allow_paths` |
| `adbc.odbc.delegate.search_path` | extra directories to search for native drivers (`:`-separated); needs `allow_paths` |
| `adbc.odbc.delegate.allow_paths` | `true` to let the two options above name filesystem paths (default `false`) |
| `adbc.odbc.delegate.last_error` | read-only: why delegation did not happen |
| `adbc.odbc.delegated_to` | read-only: the native driver serving this database/connection, or `odbc` (empty before init) |
| `adbc.odbc.tune` | `true` (default) / `false` — may the driver add ODBC connection keywords of its own where it recognises the target driver? See [Connection keywords set for you](#connection-keywords-set-for-you) for the complete list; `false` sends your connection string through untouched |
| `adbc.odbc.sqllen_32bit` | `true`/`false` to force the 32-bit-`SQLLEN` driver quirk on or off. Autodetected from `SQL_DRIVER_NAME` (on for IBM Db2's `libdb2.so`), so you normally never set it. Turn it on for any other ODBC driver that was built with a 32-bit `SQLLEN`/`SQLULEN` on a 64-bit platform — the giveaway is undetected NULLs, garbage string lengths, and row counts of `4294967295`. Also settable on the connection and the statement. |

### Connection keywords set for you

Some ODBC drivers have connection keywords whose good value depends on how the
application reads a result set — something the driver cannot know and you should
not have to. Where adbcbridge recognises the target driver it fills those in
while it assembles the connection string, under three rules: a keyword **you**
set (in the connection string or in the DSN) is never overridden, nothing that
changes what a query returns is ever set, and `adbc.odbc.tune=false` turns the
whole thing off.

The complete list today is one keyword:

| driver | condition | what is added | why |
|---|---|---|---|
| psqlodbc (PostgreSQL and the ten other PostgreSQL-wire servers it drives) | you set `UseDeclareFetch=1` and no `Fetch` | `Fetch=8192` (`8 × adbc.odbc.batch_size`, clamped to 8192…65536) | `UseDeclareFetch=1` asks psqlodbc to stream the result set through a server-side cursor instead of buffering all of it client-side. Each `FETCH` then brings back `max(Fetch, rowset)` rows, so psqlodbc's default `Fetch=100` is inert — our rowset always wins it — and the cursor round-trips once per rowset. 1M rows of `(int4, float8, varchar(20), date)` at `batch_size` 1024: **0.70 s** at the default `Fetch` against **0.57 s** with this, which is exactly what the same read costs *not* streaming. Peak process RSS for that read is 158 MB streaming against 422 MB buffered |

psqlodbc's other keywords were swept and are deliberately **not** set:
`ByteaAsLongVarBinary`, `TextAsLongVarchar`, `MaxVarcharSize` and `UnknownSizes`
change the SQL types and widths the driver reports, and so the Arrow schema and
the DDL bulk ingest generates; `TrueIsMinus1` and `LFConversion` rewrite values;
`UseDeclareFetch` and `Protocol` are transaction semantics (a server-side cursor
and per-statement `SAVEPOINT`s, neither of which every PostgreSQL-wire server
behind psqlodbc has). Everything else measured flat, within ±4% of the default
on a 1M-row read.

If you do turn `UseDeclareFetch=1` on, note that it is genuinely a different
mode, not just a buffer size: the read becomes `O(Fetch)` in client memory
instead of `O(result set)`, it needs a server that implements `DECLARE … CURSOR
WITH HOLD` and `FETCH n IN …`, and rolling back a transaction with the cursor
still open leaves the statement handle needing a fresh cursor.

## Native delegation

Native speed where a native ADBC driver exists, ODBC everywhere else, one
install.

Some databases already have a first-class ADBC driver: PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL. Those drivers talk the wire protocol
and build Arrow directly, so they are faster than anything that has to go
through ODBC's row-oriented API — 1,000,000 PostgreSQL rows take 0.42 s through
`adbc_driver_postgresql` and 1.00 s through adbcbridge over psqlodbc.

So adbcbridge gets out of the way. When `AdbcDatabaseInit` recognizes a target
that a native driver handles, it loads that driver, initializes it with the
translated options, and from then on forwards every database, connection and
statement call straight to it. Result sets are the native driver's own
`ArrowArrayStream`, handed to the caller untouched: delegation costs one
function-pointer hop per ADBC call and nothing at all per row, and delegated
fetches measure the same as calling the native driver directly (0.43 s for the
million rows above, against 0.43 s native and 0.7–0.9 s over psqlodbc).

| target | delegated to |
|---|---|
| `uri=postgresql://…` / `postgres://…` | `postgresql` |
| `uri=sqlite:…`, `duckdb:…` | `sqlite`, `duckdb` |
| `uri=snowflake://…`, `bigquery://…` | `snowflake`, `bigquery` |
| `uri=grpc://…`, `grpc+tls://…` | `flightsql` |
| `uri=Driver=…psqlodbcw.so;Server=…` | `postgresql` (URI rebuilt from the ODBC keywords) |
| `uri=Driver=…sqlite3odbc.so;Database=…` | `sqlite` (the `Database=` path) |
| `dsn=…` | whatever the DSN's `Driver=` in `odbc.ini` maps to |
| anything else (Db2, Oracle, SQL Server, Teradata, …) | nobody — ODBC, as before |

### The delegated connection is the connection you configured

Rebuilding a native URI out of an ODBC connection string is only safe if every
keyword is accounted for. Dropping `SSLmode=verify-full` would turn a verified
TLS session into libpq's default (`sslmode=prefer`, no certificate check)
without a word to anyone, so adbcbridge does not drop anything:

* **Consumed**: `Driver`, `DSN`, `Server`/`Servername`/`Host`, `Port`,
  `Database`/`DB`/`Dbname`, `Uid`/`User`/`Username`, `Pwd`/`Password`.
* **Forwarded** as libpq URI parameters: `sslmode`, `sslrootcert`, `sslcert`,
  `sslkey`, `sslcrl`, `sslcompression`, `sslsni`, `gssencmode`,
  `channel_binding`, `krbsrvname`/`pgkrbsrvname`, `connect_timeout`,
  `application_name`, `options`, `target_session_attrs`, `require_auth`,
  `load_balance_hosts`, and every libpq setting inside psqlodbc's
  `pqopt={…}` block.
* **Ignored**: keywords that only steer the ODBC driver's own client-side
  behaviour (`UseDeclareFetch`, `Fetch`, `Protocol`, `BoolsAsChar`,
  `TextAsLongVarchar`, `RowVersioning`, sqliteodbc's `StepAPI`/`LongNames`, …)
  and driver-manager bookkeeping (`Description`, `Trace`, `UsageCount`, …).
* **Anything else stops delegation.** `ReadOnly=1`, `ConnSettings=…`,
  sqliteodbc's `FKSupport`/`JournalMode`/`LoadExt`, DuckDB's `access_mode`: in
  `auto` the connection quietly stays on ODBC (with the keyword named in
  `adbc.odbc.delegate.last_error`), in `always` it is an error. The same goes
  for a DSN whose keywords the driver manager cannot enumerate.

Values are percent-encoded into the URI, so a `Database=` or `Server=` that
contains `?`, `&` or `=` stays a database name instead of becoming extra libpq
parameters. `Server=/var/run/postgresql` becomes `?host=/var/run/postgresql`,
`Server=::1` becomes `[::1]`, and the ODBC `}}` escape inside a brace-quoted
value is a literal `}`.

### When delegation does not happen

Delegation is a best-effort optimization: if no native driver is installed, if
it cannot be loaded, or if the target cannot be represented, `auto` falls back
to ODBC and records why in `adbc.odbc.delegate.last_error` (`always` turns the
same situation into an error). Two cases deliberately do *not* fall back
silently:

* A **native URI that the native driver rejected** reports the native driver's
  own error. ODBC cannot parse `postgresql://…` at all, so falling back would
  replace "password authentication failed" with unixODBC's `[IM002] Data source
  name not found`.
* A **native URI on the ODBC path** (`delegate=never`, or no native driver
  installed) is translated into an ODBC connection string for an installed ODBC
  driver of the same family, or, if there is none, refused with a message that
  says so.

Options meant for a native driver (`adbc.*` other than `adbc.odbc.*`) are held
until that decision is made and passed on to the native driver; if delegation
does not happen, `AdbcDatabaseInit` reports the option as unknown rather than
dropping it. Connection options work the same way: a connection does not know
who will serve it until `AdbcConnectionInit`, so an option ODBC does not
recognize (Flight SQL's `adbc.flight.sql.rpc.call_header.*`, a Snowflake
connection setting, …) is held there too — replayed on the native connection,
through the same typed setter it was set with, or reported by
`AdbcConnectionInit` if the connection ends up on ODBC. ODBC-specific options
(`adbc.odbc.batch_size`, …) are meaningless to a native driver and are rejected
by it. All `adbc.odbc.delegate*` options are frozen once `AdbcDatabaseInit` has
run: setting them afterwards is `INVALID_STATE`, not a silent no-op.

### Finding the native driver

adbcbridge never links against the ADBC driver manager (that would be a
circular dependency); it resolves `AdbcLoadDriver` from whichever manager is
already in the process and looks in, in order: driver manifests
(`<name>.toml`) under `ADBC_DRIVER_PATH`, `$XDG_CONFIG_HOME/adbc/drivers` (or
`~/.config/adbc/drivers`) and `/etc/adbc/drivers`; Python wheel layouts
(`site-packages/adbc_driver_postgresql/`) next to whatever ADBC object is
already loaded; the directories in `adbc.odbc.delegate.search_path` /
`ADBC_ODBC_DELEGATE_PATH`; and finally `libadbc_driver_<name>.so` on the
dynamic loader's own search path. Only `AdbcLoadDriver` is used: the newer
`AdbcFindLoadDriver` changed its signature between driver manager releases.

Because a database option that names a shared library is a code-execution
primitive for any host that forwards caller-supplied options (BI tools, DBAPI
`db_kwargs`, connector configuration), `adbc.odbc.delegate.driver` accepts only
a bare driver name — letters, digits, `_` and `-` — by default, and
`adbc.odbc.delegate.search_path` is refused outright. Paths are opt-in:

```python
db_kwargs = {
    "uri": uri,
    "adbc.odbc.delegate.allow_paths": "true",       # or ADBC_ODBC_DELEGATE_ALLOW_PATHS=1
    "adbc.odbc.delegate.driver": "/opt/drivers/libadbc_driver_postgresql.so",
}
```

`ADBC_ODBC_DELEGATE_PATH` and `ADBC_DRIVER_PATH` are ignored for setuid/setcap
processes, adbcbridge refuses to delegate to itself, and a bare
`libadbc_driver_<name>.so` is never picked up from a directory that was merely
derived from a loaded object's parent (only the exact wheel layout is).

```python
# Off, for this database:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"})
# Off, for a whole deployment:
#   export ADBC_ODBC_DELEGATE=never
# Required, so that a missing native driver is an error instead of a slow path:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "always"})
```

Who served a connection is visible through `adbc_get_info()["driver_name"]`
(`ADBC PostgreSQL Driver` vs `ADBC ODBC Driver`) and through
`adbc.odbc.delegated_to`, which names the native driver (`postgresql`) or
answers `odbc`, on both the database and the connection.

Delegation needs the driver manager's loader to be reachable in the process,
and it is not implemented on Windows — there `auto` always takes the ODBC path
and `always` fails with a clear message. The native-URI-to-ODBC fallback is
unavailable on Windows for the same reason: it picks the ODBC driver to fall
back to by enumerating `odbcinst.ini`, which is not implemented there either, so
a native URI on the ODBC path is refused with an explanation instead.

## Upstream: giving back

Running 46 databases through one driver on three operating systems finds defects that belong
to other projects. They are reported upstream with a reproduction that needs no adbcBridge
in the stack — the first three are [lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239)
(the driver manager aborts on the first SQL error from a 4-byte-`SQLWCHAR` driver; found on
macOS, reproduced on Linux with a fake driver), [openlink/virtuoso-opensource#1469](https://github.com/openlink/virtuoso-opensource/issues/1469)
and [dremio/warpdrive#16](https://github.com/dremio/warpdrive/issues/16) (undocumented driver
widths). The full record, including a dozen findings documented but not yet filed, is
[`docs/UPSTREAM.md`](docs/UPSTREAM.md).

## Language packages

One driver library, five packages that find and load it. All five are built, tested and
attached to every [GitHub Release](https://github.com/singhpratech/adbcbridge/releases/tag/v0.1.0) — the first is v0.1.0 — together with the bare
libraries for Linux x86_64/aarch64, macOS arm64 and Windows x64. Registry publication
(PyPI first) follows.

| Language | Package | What it gives you | Where |
|---|---|---|---|
| Python | `adbcbridge` wheel, `py3-none-<platform>` with the library bundled | `adbcbridge.connect(uri=...)` → `adbc_driver_manager.dbapi` connection; `adbcbridge` CLI | [`python/`](python/README.md) |
| Rust | `adbcbridge` crate; the default `bundled` feature compiles the driver from the sources carried in the crate | `adbcbridge::load()?` → `ManagedDriver` | [`rust/`](rust/README.md) |
| C# | `AdbcBridge` NuGet (netstandard2.0, net8.0) with `runtimes/<rid>/native/` assets | `Driver.Load()`, `Driver.Connect(connectionString)` | [`csharp/`](csharp/README.md) |
| Java | `org.adbcbridge:adbcbridge` over `adbc-driver-jni`, natives inside the jar | `AdbcBridge.driver(allocator)`, `AdbcBridge.open(...)` | [`java/`](java/README.md) |
| Go | `github.com/singhpratech/adbcbridge/go` over `drivermgr` (cgo) | `adbcbridge.NewDriver(alloc)`, `adbcbridge.Open(...)` | [`go/`](go/README.md) |

Each package resolves the library the same way: an explicit `ADBCBRIDGE_LIBRARY` /
`ADBC_ODBC_DRIVER`, a copy shipped inside the package, the ADBC driver manifest named
`odbc`, the usual install directories, then a `build/` tree next to a checkout — and each
raises an error that lists every place it looked.

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

`import adbcbridge` does not import pyarrow — `adbc_driver_manager.dbapi` is
imported when you call `connect()`, after the ODBC driver named in the
connection string has been opened. That order matters for the handful of ODBC
drivers that need libstdc++'s thread-locals in static thread-local storage,
which importing pyarrow first makes impossible; see
[Troubleshooting](#troubleshooting). `adbcbridge.preload_odbc_driver(name_or_path)`
does the same thing on its own, for programs that use `adbc_driver_manager` or
pyodbc directly, and `ADBCBRIDGE_PRELOAD=0` switches the automatic one off.

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

The `rust/` crate (`adbcbridge`) builds the driver from source and loads it for you: `adbcbridge::load()?` replaces the `load_dynamic_from_filename` call below; see [`rust/README.md`](rust/README.md).

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
| `adbc.odbc.rows_per_insert` | rows of parameters per `INSERT` for **bulk ingest** — `0` (default) picks a group size automatically, `1` turns the rewrite off, any other value asks for that many. Instead of executing `INSERT INTO t VALUES (?,?)` once per row, ingest prepares `INSERT INTO t VALUES (?,?),(?,?),…` with K row-groups and binds K rows' worth of ordinary parameters per execute, which divides the round trips by K. Against PostgreSQL, where a batch instead goes as one array parameter per column, the same value sets the rows one such statement carries (default 10,000) and `1` turns that off too. See [Multi-row INSERT batching](#multi-row-insert-batching) and [PostgreSQL: one array parameter per column](#postgresql-one-array-parameter-per-column). |
| `adbc.odbc.ingest_connections` | connections a **bulk ingest** may spread itself over — `1` (default) keeps it on the caller's own connection in a single transaction. `N > 1` opens `N` further connections, hands each a share of the bound stream's batches and lets each run the multi-row `INSERT` path into the same table. **This trades atomicity for speed**: `N` connections are `N` transactions, so a failure can leave some batches committed. See [Parallel ingest](#parallel-ingest-trading-atomicity-for-speed). |
| `adbc.odbc.array_binding` | `true` (default) — binds each Arrow batch as a column-wise ODBC parameter array, so `executemany` (and ingest on a driver where arrays are the faster of the two) issues one `SQLExecute` per batch instead of one per row; `false` forces row-at-a-time. Drivers that do not honour `SQL_ATTR_PARAMSET_SIZE`, or that cannot account for every parameter set they were handed, fall back automatically; DuckDB and clickhouse-odbc, whose parameter arrays silently drop values, default to `false` and can be forced back on with this option. Reported rows-affected is identical in both modes. |

Bulk ingest and `executemany` also batch their commits: when the connection is in
autocommit and more than one row is bound, the driver turns autocommit off for the
duration and commits once at the end (rolling back if the execute fails), instead of
paying a commit per row. A transaction the caller opened themselves is left alone.

### Multi-row INSERT batching

`adbc_ingest` builds its own `INSERT`, so it can pack K rows into one statement:

```sql
INSERT INTO t ("a", "b") VALUES (?, ?), (?, ?), (?, ?), …   -- K row-groups
```

K rows' worth of scalar parameters go in per `SQLExecute`, over the same
`SQLBindParameter` calls and the same per-driver parameter handling as one row at a
time — no parameter arrays are involved, so it works on every driver that can bind
ordinary parameters. It is what makes ingest fast on the drivers whose parameter
arrays are unusable (DuckDB, MonetDB, clickhouse-odbc, QuestDB via psqlodbc) and on
the ones where an array is no cheaper than a loop (MySQL Connector/ODBC), and it is
faster than arrays on most of the drivers where arrays do work — so it is the default
for ingest, with parameter arrays kept ahead of it only for MariaDB Connector/ODBC,
whose arrays go out as a single `COM_STMT_BULK_EXECUTE`.

Only the `INSERT` that bulk ingest generates is ever rewritten. A query you wrote is
executed as written, whatever is bound to it.

PostgreSQL is a step ahead of this: there a batch goes as one array parameter per column
rather than as K row-groups of cells, and the multi-row form is what the ingest falls back
to. See [PostgreSQL: one array parameter per
column](#postgresql-one-array-parameter-per-column).

K is chosen from a parameter budget (2000 parameters, at most 1000 row-groups — SQL
Server's limits are 2100 and 1000) and clipped by the driver's `SQL_MAX_STATEMENT_LEN`.
ODBC has no "maximum parameters" question to ask, so the real ceiling is *probed*: a
`SQLPrepare` (or, for a driver that only objects later, the first `SQLExecute`) that is
refused halves K and asks again, and the answer is remembered on the connection. A
server with no multi-row `VALUES` at all is found the same way — by a two-row
`SQLPrepare` before anything is written — and ingest simply carries on as before:

| server | what happens |
|---|---|
| Oracle | `VALUES (…),(…)` is `ORA-00933`, so the probe re-asks with `INSERT ALL INTO t VALUES (…) INTO t VALUES (…) SELECT 1 FROM dual` and uses that |
| SQLite | 2000 parameters is over the limit of a 999-variable build; K halves until it prepares |
| ClickHouse | clickhouse-odbc prepares 500 row-groups and then refuses to execute them; K halves to 125 |
| Firebird (OdbcFb) | no multi-row `VALUES` and no `INSERT ALL`; the probe re-asks a third time with `INSERT INTO t (cols) SELECT CAST(? AS <type>), … FROM RDB$DATABASE UNION ALL SELECT …` — a bare `?` in a select list has no type Firebird can infer, and the `CAST` names the type ingest would have *created* for that column, so it cannot narrow anything the plain form would not |
| Cloud Spanner (PGAdapter) | 950 parameters per statement is a hard limit, and one that cannot be probed: a bigger statement prepares fine and drops the connection at execute, so it is declared and K is capped at 237 four-column rows |

A failure part way through is unchanged by any of this: the whole ingest is one
transaction, so it commits completely or leaves nothing behind.

## Partitioned reads (`ExecutePartitions`)

One connection reading a large table is one CPU decoding it. ADBC's partition contract
exists for exactly that: `AdbcStatementExecutePartitions` hands back N opaque
descriptors, and `AdbcConnectionReadPartition` turns any one of them back into a stream
— on any connection, in any order, in any process. adbcbridge implements both, so N
connections can read one query in parallel and the pieces concatenate.

```python
import concurrent.futures, pyarrow
import adbc_driver_manager.dbapi as dbapi

def open_conn():
    return dbapi.connect(driver="libadbc_driver_odbc.so", db_kwargs={
        "adbc.odbc.connection_string": "Driver=PostgreSQL Unicode;Server=...;"})

with open_conn() as conn, conn.cursor() as cur:
    cur.adbc_statement.set_options(**{"adbc.odbc.partitions": "8"})
    cur.adbc_statement.set_sql_query("SELECT id, val, txt, dt FROM bench")
    descriptors, schema, _ = cur.adbc_statement.execute_partitions()

def read(descriptor):                      # one connection per partition
    with open_conn() as conn:
        stream = conn.adbc_connection.read_partition(descriptor)
        return pyarrow.RecordBatchReader._import_from_c(stream.address).read_all()

with concurrent.futures.ThreadPoolExecutor(len(descriptors)) as pool:
    table = pyarrow.concat_tables(list(pool.map(read, descriptors)))
```

`bench/partition_bench.py` is that client, benchmarked against the native
`adbc_driver_postgresql`; the numbers are in
[`bench/BENCHMARKS.md`](bench/BENCHMARKS.md).

**How the split is made.** Every strategy produces the same shape: an ordered SQL
expression that is never NULL for a row of the table, and N-1 boundary values cutting it
into N half-open intervals, with the first slice unbounded below and the last unbounded
above. Half-open intervals put a row that lands exactly on a boundary in one slice and
only one, so repeated values are neither doubled nor dropped; the unbounded ends make
the union of the slices the whole domain of the expression, so boundaries computed from
stale metadata cost balance but never rows.

Three strategies, tried in that order:

| # | expression | applies when | boundaries from |
|---|---|---|---|
| 1 | `ctid` | PostgreSQL, and the relation has a heap | `pg_relation_size` (block count) |
| 2 | the leading primary-key column | there is a single-table primary key whose leading column is an integer, `NOT NULL`, and ordered | one `SELECT MIN(k), MAX(k)` |
| 3 | `yb_hash_code(k)` | YugabyteDB, where the primary key is hash-partitioned | fixed: the hash is a `uint16` |

**1 — the PostgreSQL heap.** Every heap tuple has a `ctid` of `(block, offset)` and `tid`
compares lexicographically, so the table's blocks are a total order that slices the heap
with no index, no key column and no sort:

```sql
SELECT … FROM t WHERE ctid >= '(lo,0)'::tid AND ctid < '(hi,0)'::tid
```

Tuple offsets are 1-based, so `'(N,0)'` is a point strictly *between* blocks: no tuple
can land on both sides of a boundary or on neither. PostgreSQL 14+ executes each slice as
a TID Range Scan, so a slice reads only its own blocks rather than filtering a full
sequential scan. This is the best split where it exists — no index needed, balanced in
bytes rather than key values, and read sequentially — so it stays first.

**2 — the key range.** `ctid` is a heap detail, and several PostgreSQL-wire servers do
not store tables in a heap: CockroachDB answers `SELECT ctid` with `42703`, YugabyteDB
with `0A000`. What they do have is a primary key, which is indexed, ordered and
`NOT NULL` — the three things the split shape needs:

```sql
SELECT … FROM t WHERE "id" >= 125001 AND "id" < 250001
```

The gate is built out of ODBC's own catalog calls — `SQLPrimaryKeys` for the leading key
column, `SQLColumns` for its type and nullability — so it is not tied to any one server's
system tables. It also reaches tables PostgreSQL itself could not split before: a
declaratively partitioned parent has no heap of its own, and now gets N partitions where
it used to get one.

The primary key is required, not merely preferred: a range predicate on an *unindexed*
column makes every slice scan the whole table, so N slices would be N times the server
work. That is also why there is no modulo-or-hash split (`WHERE mod(hash(k), N) = i`),
which balances perfectly and is never index-usable, and no `LIMIT`/`OFFSET` split, where
the server must produce and discard `OFFSET` rows per slice.

**3 — YugabyteDB's tablet hash.** YugabyteDB's default `PRIMARY KEY (id)` is
*hash*-partitioned, and against a hash-partitioned key `WHERE id >= a AND id < b` is not
an index condition at all — it is a `Storage Filter` over a `Seq Scan`. A key-range split
there would be correct and N times slower than not splitting. So the ordered expression
becomes the tablet hash itself, over the fixed range `[0, 65536)`:

```sql
SELECT … FROM t WHERE yb_hash_code("id") >= 8192 AND yb_hash_code("id") < 16384
```

On any PostgreSQL-wire server the driver reads `pg_index.indoption` — where YugabyteDB
records hash partitioning as bit `0x4` — before it will emit a key-range predicate, and
declines to split at all if it cannot read it.

**When it falls back to one partition.** A wrong split is silent data loss rather than an
error, so the driver splits only what it can prove, and returns a single descriptor
carrying the original query — always correct, never slower than not calling
`ExecutePartitions` — for everything else:

| case | why |
|---|---|
| anything but a bare `SELECT <list> FROM <one table>` | a `WHERE`, `JOIN`, `ORDER BY`, `GROUP BY`, `LIMIT`, `DISTINCT`, `UNION`, subquery, CTE, aggregate or any parenthesis at all |
| any quoting or comment in the SQL | the scanner does not track quotes, so it refuses rather than mistake a keyword inside a literal for a keyword |
| a statement with bound parameters | a descriptor carries SQL text; there is nowhere to put them |
| no heap *and* no usable primary key | a view or foreign table; a table with no primary key, or one whose leading key column is text, numeric, a date, or nullable |
| a unique index that is not the primary key | choosing between several is not a choice the catalog makes for us, so the driver does not make it either |
| a PostgreSQL-wire server that will not say how its key is ordered | the difference between an index condition and N full scans is not something to guess |
| an empty table, or a key with one distinct value | nothing to slice |
| `adbc.odbc.partitions=1` | the explicit off switch |

Nothing here ever *guesses* a partition column: each strategy either proves its
expression is total and `NOT NULL` over the table from the catalog, or hands back one
partition.

**Snapshot semantics.** Partitions are read on separate connections and therefore under
separate snapshots. Against a table being written concurrently, the union of the slices
is not a point-in-time view — that is inherent to ADBC's partition model, which has
nowhere to carry a shared snapshot, and is why partitioning is opt-in. Against a table
that is not being written, the slices reproduce the unpartitioned read exactly: same
rows, same schema, no duplicates, no gaps, whatever order they are read in.

When the connection is [delegated to a native ADBC driver](#native-delegation), both
entry points forward to that driver and its own partitioning (or its own refusal)
applies.

## Prefetch

`SQLFetch` blocks on the socket while the CPU is idle, and the conversion into Arrow then
runs while the socket is idle. `adbc.odbc.prefetch=1` overlaps them: a background thread
fetches the next rowset into a second set of bound buffers while the calling thread
converts the current one. Higher values keep more rowsets in flight, up to 8.

```python
conn = dbapi.connect(driver="libadbc_driver_odbc.so", db_kwargs={
    "adbc.odbc.connection_string": "...",
    "adbc.odbc.prefetch": "1"})
```

It is **off by default**, because whether it is safe is a property of the ODBC driver
underneath and no driver can be asked. Two things make it safe where it does engage:

* The statement handle is owned by exactly one thread at a time. The fetch thread owns it
  from `pthread_create` to `pthread_join` and the calling thread touches it only outside
  that window, so no two ODBC calls on one handle are ever concurrent.
* It engages only when every column is bound at a width that cannot truncate. Repairing a
  truncated value means `SQLGetData` or `SQLFetchScroll` on a row the fetch thread has
  already read past, so a column bound narrower than its declared width (a `TEXT` or
  `NVARCHAR(MAX)`; see `adbc.odbc.long_bind_bytes`) turns prefetch off for that query
  rather than racing for the cursor. So does a column the driver cannot bind at all, and
  a driver that fetches one row at a time.

Falling back is silent and changes nothing about the rows. Errors are unchanged too: a
failure part way through a result set arrives at the same row it would have without
prefetch, and releasing a stream early stops the fetch thread before the handle is freed.

**It is worth very little in practice**, and the reason is worth knowing: most ODBC
drivers have already buffered the whole result set client-side by the time `SQLFetch` is
called, so there is no socket wait left to hide. Measured on 1,000,000 rows (medians of
5, `bench/prefetch_bench.py`):

| driver | prefetch=1 | prefetch=2 |
|---|---:|---:|
| psqlodbc, default | 1.00× | 1.01× |
| psqlodbc, `UseDeclareFetch=1` | 1.06× | 1.10× |
| sqliteodbc | 1.01× | 1.01× |
| MariaDB Connector/ODBC | 1.04× | 0.93× |

Only psqlodbc's cursor mode leaves a real socket wait inside `SQLFetch`, and even there
it buys 6–10%. For a big PostgreSQL read, [partitioning](#partitioned-reads-executepartitions)
is the mechanism that pays.

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
```

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
}
```

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

## Troubleshooting

[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) covers the failures whose
symptom points somewhere other than their cause. Two worth knowing about here:

**`Can't open lib '<path>' : file not found`, for a file that is there.**
unixODBC loads driver libraries through libltdl, which reports every failure to
load one as `file not found` — the `dlerror()` explaining why is thrown away
before the driver manager sees it. adbcbridge opens the same path itself when a
connection fails that way and puts the real reason into the ADBC error, so the
message says `Permission denied`, or a missing dependency, or the one below,
instead of pointing at a file that is plainly present. A driver that genuinely
is not there still reports `No such file or directory`.

**`cannot allocate memory in static TLS block`, after importing pyarrow.**
MySQL Connector/ODBC — alone among the 18 driver libraries in the compatibility
matrix — needs libstdc++'s thread-locals in static TLS, and importing pyarrow
(directly, or through pandas or `adbc_driver_manager.dbapi`) permanently pins
libstdc++ to dynamic TLS instead. The driver then cannot be loaded in that
process at all; it reproduces with plain pyodbc, with no ADBC in sight. Loading
the ODBC driver *before* that import settles it, which is what the adbcbridge
Python package does: `import adbcbridge` does not import pyarrow, and
`adbcbridge.connect()` opens the driver named in the connection string first.
Where the import order is not yours to choose, `LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6`
works. Raising `glibc.rtld.optional_static_tls` does not: static TLS surplus was
never what ran out. The full account, the per-driver table and the
`readelf`-based check for a driver not listed there are in the doc.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build/test loop, the
`clang-format` + `pre-commit` setup, and what to include in a bug report.

## License

Apache-2.0. See `NOTICE` for vendored Apache Arrow components.
