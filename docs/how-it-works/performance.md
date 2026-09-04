<!-- SPDX-License-Identifier: Apache-2.0 -->
# Performance, with the conditions attached

Every figure here names the machine and the load it was taken under; the benchmark files in [`bench/`](../../bench/README.md) carry the spreads and the commands that produced them.

## Machines behind the numbers

Every figure in this repository was measured on one of four ordinary machines (the three below, plus the 4-core / 8 GB Windows laptop of the first Windows campaign, kept as history in the Windows benchmark files) — no
cloud instances, no dedicated benchmark hosts — and the files say which. Read the
numbers with these in mind:

| | Linux (reference host) | macOS | Windows |
|---|---|---|---|
| Machine | laptop, Intel Core i9-13900HK, 14 cores / 20 threads, 31 GiB | Apple M4 Max, 16 cores, 64 GB, arm64 | laptop, Intel Core i9-13900HK, 14 cores / 20 threads, 32 GB |
| OS | Linux Mint 22.3, kernel 7.0 | macOS 26.5.2 | Windows 11 Home 23H2 (build 22631) |
| Driver manager | unixODBC 2.3.12 | unixODBC 2.3.12 from source (iODBC from source for iODBC-only vendor drivers) | the OS's own (odbc32), ANSI code page 1252 |
| Compiler | gcc 13.3 | Apple clang 21 | MSVC 19.44 (VS 2022 Build Tools) |
| Databases | 53 of 53 verified, Docker containers on the same machine (the full fleet often idling alongside); the matrix benchmarks below cover the 46 of the 2026-08-25 campaign | 53 of 53 results: 44 pass, 8 without an obtainable driver or runnable server, 1 not run; servers in Docker Desktop, some amd64 images under emulation | 53 of 53 results: 48 pass, 3 fail inside the vendor driver, 2 without an obtainable driver; servers in Docker Desktop on WSL2 (20 GB VM cap) |
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
| **adbcBridge `fetch_arrow_table()`** | **0.48 s** | 1.0× |
| pyodbc `fetchall()` → `pyarrow.Table` | 1.16 s | 2.4× slower |
| pyodbc `fetchall()` → `pandas.DataFrame` | 1.32 s | 2.8× slower |
| raw `SQLBindCol`+`SQLFetch`, no Arrow (floor) | 0.44 s | 0.93× |

The bridge runs within 7% of the raw ODBC floor; the remaining cost is the ODBC driver itself.

That floor is also the ceiling for a single connection, so beating a native ADBC driver
means doing work it does not: splitting one query across several connections. Against
the native `adbc_driver_postgresql`, adbcBridge on one connection is 0.36× on 10 M
rows; at eight partitions it reads 1 M rows **1.2–1.5× faster on a quiet host**, and at
twelve partitions 10 M rows 1.55× faster — mean of three, a fresh process per run, sides interleaved,
every read checksum-compared against a reference. The same 1 M read on a host with
46 idle containers came in at 0.97×, so the number to plan around is the low end, and
bulk *ingest* does not clear parity at all (0.73–1.02×: an `INSERT` writes twice the
WAL of the `COPY` the native driver uses). See
[Partitioned reads](partitioned-reads.md) and `bench/BENCHMARKS.md`.

### Bulk ingest

`adbc_ingest` sends one `INSERT INTO t VALUES (…),(…),…` per K rows rather than one
statement per row, inside a single transaction. K is probed against the driver — SQLite's
999-variable limit, ClickHouse preparing 500 row-groups and then refusing to execute them,
and Oracle rejecting the multi-row form outright (it falls back to `INSERT ALL … SELECT 1
FROM dual`) are all discovered at run time and remembered on the connection. It works on
every driver that can bind a parameter, including the eight whose parameter arrays are unusable
(`no_param_arrays`; MySQL Connector/ODBC accepts them and walks them row by row), and `adbc.odbc.rows_per_insert` overrides the choice. 10,000 rows, rows/s:

| Database | one statement per row | multi-row |
|---|---:|---:|
| ClickHouse 26 (300 rows) | 16 | **911** |
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

#### Multi-row INSERT batching

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
for ingest, with parameter arrays kept ahead of it only for MariaDB Connector/ODBC, whose
arrays go out as a single `COM_STMT_BULK_EXECUTE`, and Vertica's driver, which turns an
array into one native bulk load.

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
| Oracle | on releases without a multi-row `VALUES`, `VALUES (…),(…)` is `ORA-00933` and the probe re-asks with `INSERT ALL INTO t VALUES (…) INTO t VALUES (…) SELECT 1 FROM dual`; Oracle 23.26 takes the plain form and the fallback is never reached |
| SQLite | 2000 parameters is over the limit of a 999-variable build; K halves until it prepares |
| ClickHouse | clickhouse-odbc prepares the first multi-row form and then refuses to execute it; K halves until a form runs |
| Firebird (OdbcFb) | no multi-row `VALUES` and no `INSERT ALL`; the probe re-asks a third time with `INSERT INTO t (cols) SELECT CAST(? AS <type>), … FROM RDB$DATABASE UNION ALL SELECT …` — a bare `?` in a select list has no type Firebird can infer, and the `CAST` names the type ingest would have *created* for that column, so it cannot narrow anything the plain form would not |
| Cloud Spanner (PGAdapter) | 950 parameters per statement is a hard limit, and one that cannot be probed: a bigger statement prepares fine and drops the connection at execute, so it is declared and K is capped at 237 four-column rows |

A failure part way through is unchanged by any of this: the whole ingest is one
transaction, so it commits completely or leaves nothing behind.

#### PostgreSQL: one array parameter per column

Against PostgreSQL the driver goes one step further and sends a whole *column* of a batch
as a single array parameter, letting the server expand it:

```sql
INSERT INTO t ("a", "b", "c", "d")
  SELECT * FROM unnest(?::bigint[], ?::float8[], ?::text[], ?::date[])
```

That is one parameter per column however many rows the statement carries (10,000 by
default), instead of one bound cell per value. On 1,000,000 four-column rows it takes the
single-connection ingest from 2.33 s to 1.40 s and the sixteen-connection ingest from
0.58 s to 0.34 s, with about a third less total CPU (all of it on the client; server CPU
is unchanged). It is on by default and needs no option.

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
the default, is atomic: one connection, one transaction.

Two further things to know. Fanning out needs the target table to be visible to the worker
connections, so when the caller is inside its own transaction (autocommit off) the
`CREATE TABLE` is uncommitted and invisible; the driver quietly keeps the ingest on one
connection instead, which is correct and atomic and merely slower. And it is a
server-side-parallelism play: on a single-writer database it backfires — SQLite's ingest
of 200,000 rows goes from 0.224 s to 0.273 s on four connections, the workers contending
for the one write lock.

Even at its best this does not catch the native PostgreSQL driver, which ingests with
`COPY … (FORMAT binary)`. The array form closes most of the gap — at 1,000,000 rows and
sixteen connections the two are within noise of each other (0.9–1.0x over repeated runs,
best 0.317 s against 0.322 s) (the multi-row form alone reaches 0.77x) — but not all of it, and at 10,000,000 rows it stays about 1.4x behind. The
reason is not the statement shape: an `INSERT` writes **twice the WAL** a `COPY` does
(96.4 MB against 48.8 MB for the same million rows), because `COPY` batches tuples into
one WAL record per page and `INSERT` writes one per row. That is what caps the parallel
curve — over half of the backends' time at N=16 is spent waiting on WAL and buffer locks —
and no statement an ODBC driver can send gets underneath it, `COPY` itself being
unreachable through the ODBC API. See [bench/BENCHMARKS.md](../../bench/BENCHMARKS.md) for the
measurements. A caller who needs native ingest speed against PostgreSQL should let the
driver [delegate](delegation.md) to `adbc_driver_postgresql`.

MariaDB and Vertica keep ODBC parameter arrays, which are faster there. Firebird has no
multi-row `VALUES` in its dialect and takes a `UNION ALL` of typed one-row `SELECT`s
instead (5,974 → 7,924 rows/s).

### Rust

`bench/rust/` runs the same read and bulk-ingest workload from Rust, comparing the bridge
against the [`odbc-api`](https://crates.io/crates/odbc-api) and
[`arrow-odbc`](https://crates.io/crates/arrow-odbc) crates talking to the same ODBC driver.
Per-database results are in [`bench/RUST_BENCHMARKS.md`](../../bench/RUST_BENCHMARKS.md).

### Every language

`bench/csharp/`, `bench/java/` and `bench/go/` run that same ingest-and-fetch workload from
C#, Java and Go. [`bench/LANGUAGE_BENCHMARKS.md`](../../bench/LANGUAGE_BENCHMARKS.md) puts all five
bindings — Python, Rust, C#, Java and Go — side by side on the same table, so you can see how
much of the cost is the driver and how much is the language's driver manager.
