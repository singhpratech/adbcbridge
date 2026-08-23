<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbcbridge read-path benchmarks

Fetching 1,000,000 rows of `(int, double, 20-char text, date)` out of SQLite,
comparing adbcbridge against the usual Python ODBC path.

Reproduce with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so \
  python bench/fetch_bench.py --rows 1000000 --reps 5 --per-column --unbound-penalty
SQLITE_ODBC_DRIVER=... python bench/fetch_bench.py --rows 1000000 --sweep 9   # batch_size sweep
```

## Headline

adbcbridge is **2.4x faster than `pyodbc.fetchall()` -> `pyarrow.Table`** and
**2.8x faster than `pyodbc.fetchall()` -> `pandas.DataFrame`**, and it lands
within **7% of the raw-ODBC floor** — the cost of `SQLBindCol` + `SQLFetch`
with no Arrow work at all. On this workload there is very little headroom left
inside our own code; see [Where the time goes](#where-the-time-goes).

## Machine and software

| | |
|---|---|
| CPU | Intel Core i9-13900HK, 14 cores / 20 threads, 5.4 GHz boost, 24 MiB L3 |
| RAM | 31 GiB (~23 GiB in use by other work during the runs) |
| OS / kernel | Linux Mint 22.3, kernel 7.0.0-28-generic, x86_64 |
| Compiler | gcc 13.3.0, CMake 3.28.3 |
| Driver manager | unixODBC 2.3.12 |
| ODBC driver | SQLite3 ODBC (sqliteodbc) 0.99991 |
| Python | 3.12.3 |
| Packages | adbc-driver-manager 1.12.0, pyarrow 25.0.1, pyodbc 5.3.0, pandas 3.0.5 |

Data: a 50.2 MiB SQLite file, `CREATE TABLE bench (id INTEGER, val DOUBLE, txt
VARCHAR(20), dt DATE)`, 1,000,000 rows, no nulls, no index; queried as
`SELECT id, val, txt, dt FROM bench`. adbcbridge returns
`int32, double, string, date32[day]` — 40.0 MB of Arrow buffers.

The machine was not otherwise quiesced (other agents were running), so numbers
carry roughly +-3% run-to-run noise, with occasional outliers up to +40%.
Every figure below is a median of repeated runs after a discarded warmup, and
the batch-size sweep interleaves the sizes round-robin so drift cannot favour
one of them.

## Results

1,000,000 rows, median of 5 runs after 1 warmup, `adbc.odbc.batch_size=8192`,
driver built `-DCMAKE_BUILD_TYPE=Release`:

| Path | Median | Throughput | Relative |
|---|---:|---:|---:|
| **(a) adbcbridge `cur.fetch_arrow_table()`** | **0.476 s** | **2.10 M rows/s** | **1.00x** |
| (b) pyodbc `fetchall()` -> `pyarrow.Table` | 1.155 s | 0.87 M rows/s | 2.43x slower |
| (c) pyodbc `fetchall()` -> `pandas.DataFrame` | 1.319 s | 0.76 M rows/s | 2.77x slower |
| *reference*: stdlib `sqlite3.fetchall()`, no ODBC | 0.540 s | 1.85 M rows/s | 1.13x slower |
| *floor*: raw `SQLBindCol`+`SQLFetch`, no Arrow | 0.443 s | 2.26 M rows/s | 0.93x |

For (b) and (c), `fetchall()` alone accounts for 0.949 s and 0.940 s; the
remaining 0.21 s and 0.38 s is building the Arrow table / DataFrame from the
list of `pyodbc.Row` objects. So adbcbridge beats pyodbc's *row materialisation
alone*, before any columnar conversion is charged to it. Note pyodbc's path
infers `int64` for `id` where we return the declared `int32`.

The *floor* row is `bench/odbc_floor.c`: it binds every column exactly the way
`src/odbc_reader.c` does, drains the result set with a block cursor, touches
one byte per value and converts nothing. It is the speed of the driver manager
plus sqliteodbc plus SQLite, and is the hard limit for any ODBC->Arrow bridge
on this stack.

The same table with the `-DCMAKE_BUILD_TYPE=Debug` build (which is `-O0` — see
optimisation #1):

| Path | Median | Throughput |
|---|---:|---:|
| adbcbridge `fetch_arrow_table()` (Debug `-O0`) | 0.522 s | 1.91 M rows/s |
| *floor* (always built `-O2`) | 0.449 s | 2.23 M rows/s |

### Effect of `adbc.odbc.batch_size`

9 interleaved rounds (Debug) / 7 (Release), 1M rows, median:

| `batch_size` | Debug `-O0` | vs 1024 | Release `-O3` | vs 1024 | Arrow batches |
|---:|---:|---:|---:|---:|---:|
| 1024 | 0.538 s | — | 0.489 s | — | 977 |
| 8192 | 0.529 s | -1.7% | 0.487 s | -0.3% | 123 |
| 65536 | 0.527 s | -2.0% | 0.481 s | -1.6% | 16 |

**Batch size barely matters on this stack**, and the reason is instructive.
sqliteodbc materialises the entire result set during `SQLExecDirect`, so by the
time we call `SQLFetch` the rows are already in the driver's memory. Raising
`SQL_ATTR_ROW_ARRAY_SIZE` from 1024 to 65536 cuts the number of `SQLFetch`
calls from 978 to 17, but the floor's fetch-loop time is flat (0.100 s ->
0.106 s — very slightly *worse*, because 65536 x 81-byte text rows is a 5.3 MiB
buffer that no longer fits in L2). The 1-2% that batch size does buy us is
almost entirely our own per-batch overhead: one `ArrowArrayInitFromSchema` +
`ArrowArrayFinishBuildingDefault` per batch, amortised over more rows.

This conclusion is specific to an embedded, eagerly-materialising driver. On a
client/server driver where `SQLFetch` costs a network round trip per rowset,
the row array size is the dominant tuning knob and the gain from 1024 -> 8192
would be large. **8192 is the better default**: it is never worse here, and it
is much better there. See optimisation #6.

### Per-column cost

`SELECT <one column> FROM bench`, 1M rows, Release, `batch_size=8192`, ours vs
the raw-ODBC floor for the same query:

| Column | Arrow type | ours | floor | of which `SQLExecDirect` | **our conversion** |
|---|---|---:|---:|---:|---:|
| `id` | `int32` | 0.139 s | 0.111 s | 0.097 s | 28 ms |
| `val` | `double` | 0.199 s | 0.206 s | 0.150 s | ~0 ms |
| `txt` | `string` | 0.126 s | 0.111 s | 0.098 s | 16 ms |
| `dt` | `date32` | 0.153 s | 0.134 s | 0.100 s | 19 ms |
| all four | | 0.510 s | 0.448 s | 0.339 s | 62 ms |

`val` is the most expensive column by a wide margin — and **none of that is
ours**. sqliteodbc spends 0.150 s vs 0.097 s in `SQLExecDirect` producing
doubles; our `ArrowArrayAppendDouble` loop is lost in the noise. Conversely
`id` and `dt` are where our per-value append work actually shows up.

## Where the time goes

`perf` is unavailable on this machine (`kernel.perf_event_paranoid = 4`, no
sudo, no `CAP_PERFMON`; valgrind/callgrind is not installed either), so the
breakdown below comes from cProfile plus differential measurement against
`bench/odbc_floor.c`, which is instrumented to time `SQLExecDirect` and the
`SQLFetch` loop separately.

cProfile around one `fetch_arrow_table()` (Debug build) attributes 0.375 s to
`cursor.execute()` and only 0.192 s to `fetch_arrow_table()` itself. That is
the whole story in one line: **most of the time is spent before we have fetched
a single row.** The floor program confirms it from the C side.

Release build, `batch_size=8192`, per 1,000,000 rows:

| Stage | Time | Share | Whose code |
|---|---:|---:|---|
| `SQLExecDirect` — sqliteodbc runs the query and materialises every row | 338 ms | **71%** | sqliteodbc + SQLite |
| `SQLFetch` loop — copying rowsets into our bound buffers | 105 ms | **22%** | unixODBC + sqliteodbc |
| Arrow conversion + ADBC/stream plumbing | ~30 ms | **6-7%** | **ours** |
| total | 476 ms | 100% | |

Measured by 12 alternating runs of ours and the floor: overhead is 33.6 ms by
minimum, 27.1 ms by median — about **8 ns per value** across 4M values. In the
Debug `-O0` build the same overhead is ~73 ms, i.e. **`-O0` more than doubles
the cost of our conversion loop**.

Reading `src/odbc_reader.c`, that ~30 ms is spent in:

- `AppendValue()` (one call per value): a switch on `c->kind`, then a nanoarrow
  `ArrowArrayAppendInt`/`AppendDouble`/`AppendString`, each of which
  re-dispatches on the Arrow storage type, bounds-checks, appends to the
  validity bitmap and may grow a buffer. Four columns x 1M rows = 4M calls.
- `ReaderNextBatch()` per batch: `ArrowArrayInitFromSchema` (walks the schema
  and parses each child's format string) plus `ArrowArrayStartAppending` and
  `ArrowArrayFinishBuildingDefault`. 123 times at `batch_size=8192`, 977 times
  at 1024 — this is exactly the 1-2% that the batch-size sweep moves.
- Buffer growth: nothing reserves capacity up front, so every column's data and
  offset buffers walk a geometric realloc chain on each new batch.

The important consequence: **on this stack, micro-optimising `AppendValue`
cannot win more than ~6% end-to-end.** The optimisations that matter are the
ones that change *which code path we take*, not how fast the current path runs.

## Postgres: native vs bridge vs floor

The SQLite numbers above are dominated by `SQLExecDirect`, which hides everything
our own code does. Repeating the exercise against a **client/server** driver —
PostgreSQL 16 in Docker via psqlodbc, 1,000,000 rows of
`(int4, float8, text 'row_N', date)` — puts our code back on the critical path and
exposed two structural problems that the SQLite benchmark could not see.

Medians of 5 runs, `batch_size=8192`, Release build. *floor* is `bench/odbc_floor.c`
against the same query; *native* is `adbc_driver_postgresql` (libpq, no ODBC).

| Path | before | after | | |
|---|---:|---:|---|---|
| native `adbc_driver_postgresql` | 0.42 s | 0.42 s | | |
| **adbcbridge (ODBC)** | **1.10 s** | **0.67 s** | | **1.64x faster** |
| — of which execute | 0.54 s | 0.43 s | floor 0.44 s | |
| — of which fetch + convert | 0.45 s | 0.25 s | floor 0.24 s | |
| *floor*: raw `SQLBindCol`+`SQLFetch` | 0.68 s | 0.68 s | | |
| pyodbc `fetchall()` -> `pyarrow.Table` | 1.45 s | 1.49 s | | |

We went from **1.6x the raw-ODBC floor to within a couple of percent of it**, on
both halves independently. What is left is psqlodbc and libpq; the remaining gap
to *native* is the ODBC boundary itself (text-protocol decoding inside psqlodbc),
which no bridge can remove.

### What the two halves were

**Execute (0.54 s -> 0.43 s): a wasted `SQLPrepare` round trip.** Every ADBC DBAPI
client calls `AdbcStatementPrepare` before `AdbcStatementExecuteQuery`, even for a
one-shot query with no parameters — `_prepare_execute()` in `dbapi.py` does it
unconditionally. We turned that straight into `SQLPrepare` + `SQLExecute`: two
server round trips where `SQLExecDirect` needs one. On an embedded driver that is
free, which is why the SQLite benchmark never showed it.

`AdbcStatementPrepare` now only *records* the request. The real `SQLPrepare` is
issued when something actually needs it: parameters get bound
(`src/odbc_bind.c` already did this lazily), `AdbcStatementExecuteSchema` is
called, or the statement is executed a **second** time — the point at which a
prepared statement starts paying for itself. Measured in isolation:
`SQLPrepare`+`SQLExecute` 0.533 s vs `SQLExecDirect` 0.423 s, i.e. the entire
execute-side gap.

**Fetch (0.45 s -> 0.25 s): one column was disabling the block cursor.** This is
optimisation #2 below, and this benchmark was silently paying its full price.
psqlodbc reports `text` as `SQL_LONGVARCHAR` (with `column_size` 8190), and
`ClassifyColumn()` refused to bind *any* `LONGVARCHAR`. One unbound column sets
`rows_per_fetch = 1`, so all 1,000,000 rows were fetched one at a time, with a
`SQLGetData` per column per row. A `text` column in the select list is not a
corner case — it is most real queries.

Two fixes were tried:

- **`SQLSetPos(SQL_POSITION)` + `SQLGetData` on a block cursor** — the textbook
  answer, and what optimisation #2 recommended. It is **much worse** on psqlodbc:
  fetch went 0.45 s -> **3.90 s**, about 3.5 µs per `SQLSetPos`. Rejected.
- **Bind the long column at its declared width and repair only what truncates.**
  The indicator tells us the value's true length, so a value that did not fit is
  re-read in full with `SQLGetData` (positioning with `SQLSetPos` only for that
  row). The common case — every value fits — costs nothing, and long values stay
  lossless. This is what shipped.

Binding a long column is gated on the driver advertising
`SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER` in `SQL_GETDATA_EXTENSIONS`, so
the repair is always available when we rely on it. What the drivers report:

| Driver | `SQL_GETDATA_EXTENSIONS` | binds long columns |
|---|---|---|
| psqlodbc, MariaDB, MySQL, Oracle, DuckDB | `0x0f` / `0x1f` | yes |
| SQLite3 ODBC, clickhouse-odbc | `0x0b` (no `SQL_GD_BLOCK`) | no — unchanged |
| MS ODBC 18 for SQL Server | `0x04` (no `SQL_GD_BOUND`/`ANY_ORDER`) | no — unchanged |

So on three of the eight drivers in the compat matrix the read path is
byte-for-byte what it was, and the drivers that do get the new path are exactly
the ones that advertise support for it.

**That gate turned out to be too trusting.** Three of those drivers advertise
`SQL_GD_BLOCK | SQL_GD_BOUND | SQL_GD_ANY_ORDER` and then do not honour
`SQLSetPos` — see [The `TEXT` column cliff](#the-text-column-cliff), which
corrects them in `OdbcDetectQuirks` and adds a second repair route for the
drivers that have none.

### Column-at-a-time conversion

Separately, `ReaderNextBatch()` now converts a rowset **column by column** instead
of value by value (optimisations #4 and #5). A bound column's rowset buffer is
already a contiguous C array, so:

- `int8/16/32/64`, `uint8/16/32/64`, `float`, `double` are a single `memcpy` —
  the ODBC buffer already has the exact Arrow layout (checked with
  `_Static_assert` on every `SQL*` type width).
- `date32`, `time32`, `timestamp` run one tight conversion loop into a reserved
  buffer, with no nanoarrow append dispatch per value.
- `string`/`binary` size the data buffer from the indicator array in one pass,
  then bulk-append data and offsets.
- Validity is appended in runs, and nanoarrow's laziness is preserved: a rowset
  with no nulls never materialises a validity bitmap at all.
- Each batch reserves its fixed-width buffers up front, removing the per-batch
  realloc chain.

Rowsets that contain a skipped or failed row (`SQL_ROW_NOROW` / `SQL_ROW_ERROR`),
unbound columns, truncated values, and the text-parsed kinds (`decimal`,
`time64`, `timestamptz` — where the parse dominates anyway) all fall back to the
original per-value path, so behaviour is unchanged there.

Worth **16 ms per 4M values** on this query (fetch 0.265 s -> 0.249 s, ~4 ns/value)
— modest here because psqlodbc still dominates, but it is the part that scales:
it is pure CPU, so its share grows as the driver gets faster. On SQLite the same
change moved us from 93% to 95% of the floor (0.473 s vs a 0.451 s floor).

### Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# floor
cc -O2 -o odbc_floor bench/odbc_floor.c -lodbc
./odbc_floor "Driver=$PSQL_ODBC_DRIVER;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;" \
  "SELECT i::int AS id, (i*0.5)::float8 AS val, 'row_'||i AS txt,
          DATE '2024-01-01' + (i % 365) AS dt FROM generate_series(1,1000000) i" 8192
```

## The `TEXT` column cliff

Everything above reads a table declared `VARCHAR(20)`. Read a table that a bulk
*ingest* created and the same query was 2.4x slower — 0.69 M rows/s against
SQLite where `odbc-api` did 2.04 M — because of one column.

### Root cause

An Arrow `utf8` column has no length, so `adbc_ingest` creates the widest text
type the database has: `longvarchar` on SQLite, `TEXT`/`LONGTEXT` on
MySQL/MariaDB, `NVARCHAR(MAX)` on SQL Server (see [Ingest DDL: two servers where
the widest text type is a trap](#ingest-ddl-two-servers-where-the-widest-text-type-is-a-trap)
for why that one and Db2's are not simply whatever `SQLGetTypeInfo` names).
Reading one back, `SQLDescribeCol`
answers with what the *type* could hold rather than with anything the table
holds:

| Driver | `SQLDescribeCol` on the `txt` column | bound buffer it implies |
|---|---:|---:|
| sqliteodbc | `SQL_LONGVARCHAR`, 65,536 chars | 262,145 B |
| MariaDB Connector/ODBC | `SQL_LONGVARCHAR`, 16,777,215 | 67 MB |
| MySQL Connector/ODBC | `SQL_WLONGVARCHAR`, 16,777,215 | 134 MB |
| psqlodbc | `SQL_LONGVARCHAR`, 8,190 | 32,761 B |
| msodbcsql 18 | `SQL_LONGVARCHAR`, 2,147,483,647 | 8.6 GB |

`ClassifyColumn()` refused to bind anything that wide, and **one unbound column
costs the whole result set its block cursor**: `SQLGetData` needs the cursor
positioned on a single row, so `ReaderBind()` dropped `SQL_ATTR_ROW_ARRAY_SIZE`
to 1. Every column then came back one row per `SQLFetch`, through the per-value
append path instead of the column-at-a-time one — 100,000 `SQLFetch` calls where
odbc-api made 13. odbc-api never falls off this cliff because it clamps text
buffers to a caller-chosen width (`arrow-odbc`'s `with_max_text_size`, 1024 in
`bench/rust`) and keeps its 8192-row rowset.

### The fix

Bind such a column narrow — `adbc.odbc.long_bind_bytes`, 2 KiB — and read the
values that overflow it again, in full. What made that possible was finding a
repair route that each driver actually honours, rather than trusting
`SQL_GETDATA_EXTENSIONS`, which three of the five drivers overstate:

| Driver | `SQLSetPos` + `SQLGetData` on a bound column of a block cursor | `SQLFetchScroll(SQL_FETCH_ABSOLUTE)` on the forward-only cursor |
|---|---|---|
| sqliteodbc | advertises `SQL_GD_BOUND`; reads the wrong row | **works** (advertised only for the static cursor) |
| MariaDB Connector/ODBC | advertises all three bits; answers row 1 and `SQL_NO_DATA` for the rest | **works** |
| MySQL Connector/ODBC | **works** | refused, as advertised |
| psqlodbc | **works** | refused, as advertised |
| msodbcsql 18 | does not advertise `SQL_GD_BOUND`; `SQLSetPos` fails | refused, as advertised |
| DuckDB | advertises all three bits; `SQLSetPos` fails outright | refused |

So the reader now knows two ways back to a clipped value — re-read it where it
sits (`getdata_repair`), or re-read its whole rowset one row at a time with
`SQLFetchScroll(SQL_FETCH_ABSOLUTE)` and resume (`refetch_repair`) — and binds a
length-less column only when it has one. The drivers that overstate their
`SQL_GETDATA_EXTENSIONS` are corrected in `OdbcDetectQuirks`, and sqliteodbc's
understated `SQL_CA1_ABSOLUTE` likewise. Where neither route exists — SQL Server —
the column stays unbound exactly as before.

Two smaller pieces come with it:

- `adbc.odbc.rowset_bytes` (8 MiB) caps the rowset by bytes as well as by rows,
  so a wide bound column shrinks the rowset instead of allocating hundreds of
  megabytes. It also cut psqlodbc's rowset from 33 MB to 8 MB.
- A clipped value used to be returned as a silent prefix. It is now always read
  again, on every driver that can — `tests/c/test_driver.c` reads a 200,000-byte
  value off a column sqliteodbc describes as 255 characters and gets all of it,
  where before it got 1,020 bytes and no diagnostic.

**Why 2 KiB.** Because sqliteodbc null-fills a bound buffer, its cost is the
buffer width, not the value length. 100,000 rows, `SELECT *`, one column bound at:

| bound width | 1 KiB | 2 KiB | 4 KiB | 16 KiB | 32 KiB | 256 KiB (declared) |
|---|---:|---:|---:|---:|---:|---:|
| rows/s | 1.72 M | 1.63 M | 1.46 M | 0.99 M | 0.73 M | 0.17 M |

2 KiB is the widest setting that costs nothing over 1 KiB, and it is wide enough
that the repair path stays exceptional. If it does not — if most rowsets need
repairing — the reader notices after four of them and drops to one row per
fetch, which is what an unbound column would have cost from the start.

### Results

`SELECT *` of 100,000 rows of `(int32, float64, utf8, date32)` from a table
`adbc_ingest` created, `adbc.odbc.delegate=never`, medians of three
before/after rounds interleaved so drift cannot favour one build:

| Database | before | after | |
|---|---:|---:|---:|
| SQLite (sqliteodbc) | 691,000 | **1,645,000** | **2.38x** |
| MariaDB 11 (maodbc) | 1,971,000 | **2,717,000** | **1.38x** |
| MySQL 8.4 (Connector/ODBC) | 1,213,000 | **1,443,000** | **1.19x** |
| PostgreSQL 16 (psqlodbc) | 1,826,000 | 1,867,000 | 1.02x |
| SQL Server 2022 (msodbcsql 18) | 826,000 | 825,000 | 1.00x |

And against the Rust crates on the same query (`bench/rust`, `SELECT id, val,
txt, dt`, 100,000 rows, two interleaved rounds), as a fraction of `odbc-api`'s
raw row-set read:

| Database | ADBC before | ADBC after | odbc-api | before | after |
|---|---:|---:|---:|---:|---:|
| SQLite | 735,000 | **1,910,000** | 2,050,000 | 0.36x | **0.94x** |
| MariaDB | 1,842,000 | **2,860,000** | 2,890,000 | 0.62x | **1.02x** |
| MySQL | 1,209,000 | **1,513,000** | 1,760,000 | 0.69x | **0.92x** |
| PostgreSQL | 1,940,000 | 2,054,000 | 2,090,000 | 0.93x | 0.98x |
| SQL Server | 831,000 | 844,000 | 810,000 | ~1.0x | ~1.0x |

SQL Server is unchanged by design: msodbcsql offers neither repair route, so
`VARCHAR(MAX)` stays unbound there and the read stays one row per `SQLFetch`.
Its `odbc-api` figure swings between 0.44 M and 0.87 M rows/s run to run, so read
that row as "about the same", not to three digits.

The `VARCHAR(20)` numbers at the top of this file are unaffected — that column
is bound at its declared 81 bytes either way (1,000,000 rows: 2.10 M rows/s
before, 2.12 M after).

## PostgreSQL, revisited: where the last 2.6x is

The section above got the bridge to within a couple of percent of the raw-ODBC
floor on PostgreSQL. That left a 2.6x gap to the *native* `adbc_driver_postgresql`,
and the obvious explanations for it — psqlodbc's text protocol, and its
`Fetch=100` cursor granularity — are **both wrong**. What the gap actually is,
and what was done about it:

Medians of 7-9 interleaved runs, PostgreSQL 16 in `adbcbridge-pg`, psqlodbc 16.00,
`batch_size` 1024, `adbc.odbc.delegate=never`, `fetch_arrow_table()`, each rep a
fresh process pinned with `taskset -c 3`. Run-to-run spread was 1-4% (p25..p75)
with the box otherwise busy at load average 2-6.

| Query | before | after | native | floor |
|---|---:|---:|---:|---:|
| `(int4, float8, varchar(20), date)`, 1,000,000 rows | 0.5705 s (1.75 M rows/s) | 0.5704 s (1.75 M) | 0.1920 s (5.21 M) | 0.4428 s (2.26 M) |
| same, 100,000 rows | 0.0561 s (1.78 M) | 0.0563 s (1.78 M) | 0.0192 s (5.21 M) | 0.0465 s (2.15 M) |
| `(int4, text, varchar, numeric, bool, timestamp, bytea)`, 500,000 rows | 0.6330 s (790 k) | **0.5304 s (943 k)** | 0.2030 s (2.46 M) | |
| `(int4, bytea)`, 500,000 rows | 0.1820 s (2.75 M) | **0.1385 s (3.61 M)** | 0.0715 s (6.99 M) | |

The four-column query does not move, and that is the honest headline: **on a
query with no length-less column there was nothing left to win.** A C-level
measurement of the same 1M-row read (no Python, no pyarrow) attributes 0.44 s to
`SQLExecDirect` (0.21 s) plus 124 `SQLFetch` calls (0.23 s) plus **7.3 ms** of
Arrow conversion — 1.6% of wall clock. We are marginally *faster* than
`bench/odbc_floor.c` on the same query. The remaining 2.6x is inside psqlodbc.

### The width-0 cliff: `bytea`

The `TEXT` column cliff has a twin. psqlodbc describes PostgreSQL's `bytea` as
`SQL_LONGVARBINARY` with **`column_size` 0** — no width at all, however long the
values are — and `ApplyBindWidth()` refused to bind a column of width 0 outright.
One unbound column sets `rows_per_fetch = 1`, so a `SELECT` with a `bytea` in it
fetched all 500,000 rows one at a time, with a `SQLGetData` per column per row.

A width of 0 from a type that has no declared length is not a different
situation from the 8190 the same driver invents for `text`: both are guesses, and
the reader already knows how to bind a guess — at `adbc.odbc.long_bind_bytes`
(2 KiB), re-reading in full whatever overflows it. So it now does, whenever the
driver offers a repair route (`getdata_repair` / `refetch_repair`, as before).
A width of 0 from a type that *does* have a declared length still means the
driver is saying nothing usable, and such a column stays unbound.

Worth **1.19x** on a realistic seven-column row with one `bytea` in it and
**1.31x** on `(id, bytea)`. Output is byte-identical: a table of
5/200/2,000/20,000-byte `bytea` and 10/300/5,000/40,000-character `text` values,
with NULLs and empty values, hashes the same before and after at `batch_size`
1024 and at 7, and the Arrow schema is unchanged.

It did **not** cost nothing anywhere else, which the paragraph above used to
claim: every value in those tables fitted in the bound width or was rare enough
not to matter. Values that do not fit cost 1.4x, and the next section is what was
done about that.

psqlodbc's `ByteaAsLongVarBinary=0` reaches the same block cursor from the other
end — it makes `bytea` a `SQL_VARBINARY(255)` — and was measured at 0.5425 s
against 0.5304 s for the reader fix on the seven-column query, i.e. no better,
while it also moves `SQLGetTypeInfo`'s name for `SQL_LONGVARBINARY` from `bytea`
to `lo` and so breaks the DDL bulk ingest generates. It is not set.

### The other side of the cliff: values that never fit

Binding a guessed width is a bet, and the bet loses when the values are bigger
than the guess. The driver then reads every value **twice**: once decoding it
into the rowset buffer — in full, because it has to report the length that did
not fit — and once more for the `SQLGetData` that repairs it. An unbound column
decodes it once. A 2 KiB buffer in front of 3 KiB values is therefore *worse*
than no buffer at all, and by more than the block cursor is worth.

`AdaptBindWidth()` (src/odbc_reader.c) takes the bet and then checks it: it
watches what the first 256 rows of the result set actually cost and, if the
values do not fit, unbinds the column for the rest of the read — back to
`SQLGetData` and a one-row rowset, which is what the reader did before the
change above. The decision is per column and per result set, taken once, and
only where the repair route is the in-place one (`getdata_repair`; drivers that
repair by re-reading a whole rowset already drop to one row per fetch after four
repaired rowsets, and their cost is per rowset rather than per row).

**The rule is bytes re-read, not rows truncated.** A truncation costs the second
decode, which is proportional to the value — 7 µs for a 3 KiB value, 54 µs for a
64 KiB one, about 0.8 µs/KiB over a 5 µs fixed part — while the block cursor
saves a flat ~0.2 µs on each row that does *not* truncate. The two cross at
roughly **256 re-read bytes per row**, which is the threshold. A "most rows
truncate" rule would get the mixed case backwards: a table where 1% of the rows
hold 64 KiB and the rest 64 B truncates on 1 row in 100 and is still 21% slower
bound, because 1% of 64 KiB is 655 bytes per row of double decoding.

The 256 rows the decision is taken on are fetched in 128-row rowsets rather than
the full one, so that a result set of only a few thousand rows does not pay for
the window: 3,000 rows of 64 KiB learn in 0.02 s instead of 0.09 s. Arrow batch
sizes do not change — the probe rowsets fill the first batch between them.

50,000 rows of `(int4, bytea)`, `batch_size` 1024, medians of 7 interleaved runs,
fresh process per rep, `taskset -c 3`, PostgreSQL 16 in Docker, psqlodbc 16.00,
`adbc.odbc.delegate=never`, connection strings identical across variants. Each
value is distinct and self-identifying (a re-read that landed on the wrong row
would be caught) and the column is `STORAGE EXTERNAL`, so the server does not
compress the values away:

The multiplier is speed against the unbound column, so above 1.00x is faster than
leaving the column unbound and below it is slower:

| value size | unbound (before a9d1af5) | always bound (a9d1af5) | adaptive (now) |
|---|---:|---:|---:|
| 64 B | 0.0279 s | 0.0225 s (1.24x) | 0.0216 s (**1.29x**) |
| 512 B | 0.1125 s | 0.0997 s (1.13x) | 0.0946 s (1.19x) |
| 1 KiB | 0.1989 s | 0.1824 s (1.09x) | 0.1799 s (1.11x) |
| 2 KiB | 0.4768 s | 0.4474 s (1.07x) | 0.4413 s (1.08x) |
| 3 KiB | 0.7419 s | 1.0906 s (**0.68x**) | 0.7199 s (**1.03x**) |
| 4 KiB | 0.8150 s | 1.1802 s (0.69x) | 0.8172 s (1.00x) |
| 16 KiB (12,000 rows) | 0.6924 s | 0.9721 s (0.71x) | 0.6895 s (1.00x) |
| 64 KiB (3,000 rows) | 0.7223 s | 0.9942 s (0.73x) | 0.7512 s (0.96x) |

The 2 KiB row is the last one that fits: `long_bind_bytes` is 2,048 and a 2,048-byte
value does not truncate. Everything below the line keeps a9d1af5's win; everything
above it is back to what an unbound column cost, and the 64 KiB read — 4% short of
it in this run, 0.7397 s against 0.7401 s in a repeat — is the one where the
window is visible at all: 3,000 rows is twelve probe rowsets, so 8% of the table is
read before the decision.

Mixed tables, 50,000 rows, same conditions — the case an adaptive rule can get
wrong, since the column is worth giving up long before most rows truncate:

| shape | unbound | always bound | adaptive |
|---|---:|---:|---:|
| 1% of rows 64 KiB, 99% 64 B | 0.1510 s | 0.1833 s (0.82x) | 0.1518 s (0.99x) |
| 10% of rows 4 KiB | 0.1140 s | 0.1302 s (0.88x) | 0.1079 s (1.06x) |
| 25% of rows 4 KiB | 0.2386 s | 0.3167 s (0.75x) | 0.2226 s (1.07x) |
| 50% of rows 4 KiB | 0.4623 s | 0.6295 s (0.73x) | 0.4511 s (1.02x) |

And the columns that must not move (medians of 13):

| query | unbound | always bound | adaptive |
|---|---:|---:|---:|
| `(int4, float8, varchar(20), date)`, 200,000 rows | 0.1026 s | 0.1033 s | 0.1028 s |
| `text`, 512 characters, 50,000 rows | 0.0450 s | 0.0454 s | 0.0452 s |
| seven columns with a 64-byte `bytea`, 200,000 rows | 0.2905 s | 0.2332 s | 0.2360 s |
| `text`, 40,000 characters, 3,000 rows | 0.3277 s | 0.3243 s | **0.1876 s** |

The four-column query has no length-less column and never enters the machinery.
The 512-character `text` column is bound at the 8,198 bytes psqlodbc's 8,190 implies
and never truncates, so nothing about it changes. The last row is a case the
`text` binding had always been losing: 40,000-character values truncate into that
8 KiB buffer on every row, and giving the binding up is worth **1.75x** — a gain
that has nothing to do with `bytea` and was there before a9d1af5.

Two things that were tried and did not work, before settling on the one-row rowset:

* **Unbinding the column but keeping the block cursor for the others.** psqlodbc
  supports `SQLSetPos` + `SQLGetData` on a *bound* column of a block cursor —
  that is the repair route the reader relies on — but answers `SQL_NO_DATA` for
  every row after the first when the column is not bound. So a column that stops
  being bound takes the rowset down to one row with it, and the columns after it
  (ODBC wants the `SQLGetData` columns last, in increasing order).
* **Binding the column one byte wide** instead of unbinding it, so that the
  buffer write costs nothing and every row goes through the repair path. Still
  1.04 s on the 3 KiB table against 0.71 s unbound: what the repair costs is the
  second decode and the `SQLGetData` against a block cursor, not the buffer it
  truncates into.

### Streaming reads: `UseDeclareFetch=1` is now free

psqlodbc's default (`UseDeclareFetch=0`) is not a cursor at all: it drains the
whole result set into its own tuple store during `SQLExecDirect`. That costs
memory in proportion to the *result*, not to `batch_size` — 422 MB peak RSS for
a 1M-row read of a 65 MB table — and a big enough scan is an OOM waiting to
happen. `UseDeclareFetch=1` turns it into a server-side cursor and 158 MB, but
used to cost 22% throughput, because each `FETCH` returns
`max(Fetch, SQL_ATTR_ROW_ARRAY_SIZE)` rows and psqlodbc's default `Fetch` of 100
is always beaten by our rowset, so the cursor round-trips once per rowset.

With `adbc.odbc.tune` on (the default), setting `UseDeclareFetch=1` and no
`Fetch` now gets `Fetch=8192` — eight rowsets per round trip — and streaming
becomes free:

| 1,000,000 rows, `batch_size` 1024 | time | peak RSS |
|---|---:|---:|
| default (whole result set buffered client-side) | 0.5738 s | 422 MB |
| `UseDeclareFetch=1`, auto-tuned `Fetch` | 0.5723 s | 158 MB |
| `UseDeclareFetch=1`, `adbc.odbc.tune=false` | 0.6976 s | |
| `UseDeclareFetch=1;Fetch=1024` (caller's value, never overridden) | 0.7116 s | |

`UseDeclareFetch=1` is **not** set by default: it needs a server that implements
`DECLARE … CURSOR WITH HOLD` and `FETCH n IN …`, and psqlodbc drives eleven
PostgreSQL-wire databases in the compat matrix (CrateDB, QuestDB, Materialize,
RisingWave, …) whose support for that is unknown and untested here. It is an
opt-in with a documented cost, not a default.

Two other things came out of making it usable:

- **Rolling back with a cursor open used to wedge the statement for good.**
  With `UseDeclareFetch=1`, `SQLEndTran(SQL_ROLLBACK)` invalidates psqlodbc's
  cursor state behind the driver manager's back: unixODBC then believes the
  cursor is closed, answers `SQLCloseCursor` with 24000 itself, the driver never
  hears, and every later execute on that statement fails `[HY010] The cursor is
  open`. The reader now takes a fresh statement handle on the first use after a
  rollback (`tests/test_sqlite.py::test_statement_reuse_after_rollback`).
- The connection string is assembled once, at connect, and the auto-tune adds at
  most one short keyword. That matters: the **length** of a psqlodbc connection
  string moves its fetch loop by up to 10% all on its own, across a malloc
  size-class boundary, with semantically empty padding. Any A/B on psqlodbc
  connection options has to pad every variant to the same length or it measures
  the allocator.

### What the gap is not

Measured and rejected, so nobody re-runs them:

- **The text protocol is worth ~12%, not 3.7x.** The same 1M-row result set is
  61,666,789 bytes of psqlodbc text against 55,000,203 bytes of the native
  driver's `COPY … (FORMAT binary)` — 6.7 MB more, well under 10 ms of memcpy.
  psqlodbc cannot be made to ask for binary anyway: `convert.c:4149` is a literal
  `/* result format is text */ *resultFormat = 0;`, and no connection keyword or
  `pqopt` libpq keyword reaches it.
- **`Fetch` is inert at the default.** psqlodbc's default is
  `UseDeclareFetch=0`, so there is no cursor and no per-100-row round trip:
  `strace -c -e trace=network` counts 8 client messages for a 100,000-row read.
- **The real cost is psqlodbc's allocator.** During `SQLExecDirect` it calls
  `malloc` `(3 + ncols)` times and `free` 3 times **per row** — 7,000,054 mallocs
  and 3.33 GB requested for 1M x 4 columns, against libpq's 1 malloc + 1 free per
  row. Execute time tracks the malloc count at ~33 ns each across a 1-to-8-column
  sweep. Then `SQLFetch` pays text-to-C conversion, worst for `date` (98 ms/1M
  values) and `float8` (57 ms, exactly 1,000,000 `strtod` calls). Neither is
  reachable from this side of the ODBC boundary.
- **Asking for a different C type does not dodge it.** Binding `date` as
  `SQL_C_CHAR` to parse the text ourselves costs 213-268 ms per 1M values against
  91 ms for `SQL_C_TYPE_DATE`; `SQL_C_WCHAR` is worse than `SQL_C_CHAR` for every
  column.
- **Our own conversion loop has ~7 ms of headroom per 1M rows** (string bulk copy
  4.2 ms, `date32` 3.0 ms, +2.7 ms more at 10% NULLs; fixed-width columns are a
  `memcpy` already at memory bandwidth). Making all of it free would be 1.6% of
  the query. Deliberately not spent.
- **The ANSI psqlodbc build is ~5% faster** on a query with a text column
  (0.5499 s against 0.5774 s for `psqlodbcw.so`, 7 interleaved runs): binding
  `SQL_C_CHAR` against a Unicode driver makes unixODBC translate every value
  UTF-8 -> UTF-16 -> UTF-8. That is a packaging choice for the *caller* — and only
  correct with `client_encoding=UTF8` — so it is documented, not taken.
- **Where a native ADBC driver exists, delegation already wins all of it.**
  `adbc.odbc.delegate=auto` (the default) reports `delegated_to: postgresql` and
  reads the four-column 1M-row query in 0.204 s. The ODBC numbers above are what happens where no native driver is
  installed.

## Optimisation suggestions, ranked by expected gain

**1. Never ship or benchmark the `-O0` build.** `-DCMAKE_BUILD_TYPE=Debug`
compiles with no `-O` flag at all (`-g -std=gnu11 -fPIC ...`). Debug -> Release
is 0.522 s -> 0.476 s, **-8.8% end-to-end**, and it removes ~55% of our own
conversion cost (73 ms -> 33 ms). `CMakeLists.txt` already defaults to Release
when `CMAKE_BUILD_TYPE` is unset, so this is purely about what the README and
CI tell people to build. Zero risk, largest verified single gain. On a driver
where the fetch loop rather than `SQLExecDirect` dominates, this is worth
considerably more than 8.8%.

**2. Stop letting one unbound column force row-at-a-time fetching for the
entire result set.** *Measured: 2.7x, then 2.4x again.* **Done, twice** — and
not the way this entry proposed either time. `SQLSetPos` per row turned out to
be 8x *worse* on psqlodbc, so long columns are bound and only the values that
truncate are re-read; see
[Postgres: native vs bridge vs floor](#postgres-native-vs-bridge-vs-floor).
Binding them *at their declared width* then left the same cliff in place for
every driver whose declared width is a type maximum, which
[The `TEXT` column cliff](#the-text-column-cliff) closes. The original text
follows, since it is still the clearest statement of the problem.
`ReaderBind()` used to do:

```c
r->rows_per_fetch = all_bound ? (SQLULEN)r->opts.batch_size : 1;
```

A single column that cannot be bound — any `LONGVARCHAR`/`LONGVARBINARY`, any
column reporting `column_size == 0`, or any whose `elem_size` exceeds
`max_bind_bytes` — collapses `rows_per_fetch` to 1, so *every* column of *every*
row then goes through a separate `SQLGetData` round trip. Forcing this path by
setting `adbc.odbc.max_bind_bytes=8` costs **0.476 s -> 1.294 s (2.72x)** in
Release and 2.77x in Debug.

This is not a corner case. The default `max_bind_bytes` is 32768 and text
columns are sized `column_size * 4 + 1`, so any `VARCHAR(8193)` or wider trips
it — as does every unsized `TEXT` column on drivers that report `column_size`
as 0. Real schemas hit this constantly.

The fix is standard ODBC: keep the block cursor, and for the unbound columns
position on each row with `SQLSetPos(hstmt, row + 1, SQL_POSITION,
SQL_LOCK_NO_CHANGE)` before calling `SQLGetData`. Gate it on
`SQLGetInfo(hdbc, SQL_GETDATA_EXTENSIONS, ...) & SQL_GD_BLOCK` and fall back to
today's `rows_per_fetch = 1` when the driver does not advertise it. Highest-value
change in our own code, by a wide margin.

**3. Bound the *rowset* in bytes, not just each value.** **Done** —
`adbc.odbc.rowset_bytes`, default 8 MiB; see
[The `TEXT` column cliff](#the-text-column-cliff). The original entry:
`max_bind_bytes` caps
`c->elem_size`, but the allocation in `ReaderBind()` is
`calloc(rows_per_fetch, elem_size)` **per column**. At `batch_size=65536` with a
column that sizes out at the 32768-byte cap, that is a 2.1 GB allocation for one
column — a real OOM risk that grows as users raise `batch_size` (and one that
gets worse if #2 lets us raise `max_bind_bytes`). Compute the total bound row
width after `DescribeColumns`, then clamp
`rows_per_fetch = min(batch_size, target_rowset_bytes / row_width)` with
`target_rowset_bytes` a few MiB and a floor of 1. This also keeps the rowset
inside L2, which the sweep showed matters slightly at 65536. Correctness and
robustness win that unblocks #2; no throughput cost.

**4. Reserve Arrow buffers up front.** **Done.** *Estimated: 5-15% of our 30 ms, ~1% end-to-end
here.* After `ArrowArrayStartAppending`, call `ArrowArrayReserve(&batch,
rows_per_fetch)` — we know exactly how many rows the batch will hold. For bound
`FETCH_CHAR`/`FETCH_BINARY` columns we also know the maximum byte width
(`elem_size * rows_per_fetch`), so the data buffers can be reserved too. Removes
the realloc/memcpy chain on every batch. Cheap, local, no behaviour change.

**5. Bulk-append fixed-width columns instead of one call per value.** **Done**
(and extended to string/binary/date/time/timestamp; measured 16 ms per 4M values
on Postgres). *Estimated: most of the remaining ~30 ms, ~5% end-to-end here.* For
`FETCH_I32`/`FETCH_I64`/`FETCH_F64`/`FETCH_F32` the bound buffer is already a
contiguous C array in exactly the Arrow layout. When a rowset contains no nulls
for that column (check the indicator array in one pass — the common case), the
whole rowset can be a single `memcpy` into the data buffer plus a bulk validity
fill, replacing `rows_fetched` calls to `ArrowArrayAppendInt`. The per-column
table shows `id` costing us 28 ms and `dt` 19 ms, which is precisely this loop.
`FETCH_DATE`/`FETCH_TIMESTAMP` still need per-value conversion from
`DATE_STRUCT`/`TIMESTAMP_STRUCT`, but can skip the nanoarrow append dispatch by
writing into a reserved buffer directly. Worth doing mostly because it scales:
on a driver whose `SQLExecDirect` is cheap, this becomes the dominant cost.

**6. Raise the default `batch_size` from 1024 to 8192.** *Measured: -1.7%
(Debug) / -0.3% (Release) here; much larger on client/server drivers.*
`ADBC_ODBC_DEFAULT_BATCH_SIZE` is 1024, which produces 977 Arrow batches for 1M
rows — more per-batch setup for us and more chunks for every downstream
consumer. 8192 is never slower in these runs and cuts the batch count to 123.
Pair with #3 so the larger default cannot blow up memory on wide rows.

**7. Hoist the per-batch schema walk.** *Estimated: <1% at 8192, ~2% at 1024.*
`ArrowArrayInitFromSchema` re-parses every child's format string on each
`ReaderNextBatch`. Build one array template at reader-init time and reinitialise
cheaply per batch. Only worth doing after #4/#5, and mainly for small
`batch_size` values.

### What is *not* worth optimising

`SQLExecDirect` at 71% is sqliteodbc reading the whole table into its own memory
before returning, and no change on our side of the ODBC boundary can touch it.
It is also the reason batch size looks irrelevant here. Any conclusion about
tuning drawn from this benchmark should be re-validated against a client/server
ODBC driver, where the 71%/22%/7% split will look completely different and
optimisations #2, #5 and #6 will matter far more than they do here.

## A note on the write path

Everything above is the read path. Bulk ingest and `executemany` are measured
separately by `bench/ingest_bench.py` (SQLite) and `bench/matrix_bench.py` (every
database in the compatibility matrix); the per-database numbers live in
`bench/MATRIX_BENCHMARKS.md`.

Two things dominate write throughput, and neither is Arrow work:

1. **Commits.** With the connection in autocommit, every bound row is its own
   transaction — a round trip and, for most engines, an fsync. Ingest and
   `executemany` therefore turn autocommit off for the duration of the execute
   and commit once at the end. This is worth two to three orders of magnitude on
   its own and applies to every driver that reports `SQL_TXN_CAPABLE`.
2. **Executes.** With `adbc.odbc.array_binding` (default on), each Arrow batch is
   bound as a column-wise ODBC parameter array and goes out in one `SQLExecute`
   instead of one per row. Fixed-width columns are bound straight onto the Arrow
   buffers; variable-length columns are staged into a per-column buffer sized to
   the batch's longest value. Drivers whose parameter arrays cannot be trusted
   (DuckDB, clickhouse-odbc, Firebird's OdbcFb) opt out and keep the batched commit.

Ingesting 20,000 rows of `(int32, float64, string, date32)` into a table the
benchmark created, on a connection in autocommit, rows per second. "Before" is
the same code with neither change; `ab` is `adbc.odbc.array_binding`, forced on
the statement so both paths can be measured on every driver:

| Database | Before, ab=off | Before, ab=on | After, ab=off | After, ab=on (default) |
|---|---:|---:|---:|---:|
| SQLite 3.45 | 340 | 424 | 315,287 | 279,621 |
| DuckDB | 10,131 | wrong row count | 16,421 | *quirked off* |
| PostgreSQL 16 | 410 | **wrong: reported 2 rows** | 9,087 | 76,071 |
| MariaDB 11 | 280 | **error 22007** | 11,209 | 40,706 |
| MySQL 8.4 | 120 | 132 | 8,459 | 7,944 |
| SQL Server 2022 | 94 | 101 | 6,859 | 122,768 |
| Oracle 23ai | 313 | 303,154 | 10,679 | 244,875 |
| IBM Db2 12.1 | 373 | 251,599 | 15,183 | 270,476 |
| Firebird 5 | 96 | silently drops rows | 5,301 | *quirked off* |
| ClickHouse 26 | 16 | silently drops rows | 16 | *quirked off* |

Read the columns as the two changes in turn. **After, ab=off** is the batched
commit alone: 25x to 900x, and it is what the drivers with no usable parameter
arrays get. **After, ab=on** adds the parameter arrays on top, worth another 4x
to 18x wherever the driver turns them into one round trip; MySQL's connector
walks the array itself, so there it is a wash, and SQLite's driver is already
in-process.

The "Before, ab=on" column is why array binding used to be opt-in: forced on, it
reported two rows for a 20,000-row insert on PostgreSQL and failed outright on
MariaDB. Those are the bugs this work fixed. Three drivers -- DuckDB,
clickhouse-odbc and Firebird's OdbcFb -- accept the parameter-array attributes
and then quietly execute only part of the array, so they opt out in
`OdbcDetectQuirks` and keep the batched commit.

Numbers were taken while the same containers were serving other work, so treat
them as orders of magnitude rather than to three significant figures.

The remaining floor is the driver's own per-statement cost, one execute per row on
every driver with no usable parameter arrays. That floor is what the next section
removes.

### Multi-row INSERT batching

Parameter arrays are an ODBC feature, and five of the drivers in the matrix get them
wrong (DuckDB, MonetDB, clickhouse-odbc, Firebird's OdbcFb, psqlodbc against QuestDB)
while a sixth, MySQL Connector/ODBC, accepts an array and then walks it row by row
inside the driver. All six were left at one `SQLExecute` — for a client/server database
one network round trip — per row.

Multi-row `INSERT` needs no ODBC feature at all. Bulk ingest writes its own `INSERT`,
so instead of executing

```sql
INSERT INTO t ("a", "b", "c", "d") VALUES (?, ?, ?, ?)          -- N times
```

it prepares

```sql
INSERT INTO t ("a", "b", "c", "d") VALUES (?,?,?,?), (?,?,?,?), …   -- K row-groups
```

and binds K rows' worth of ordinary scalar parameters per execute: parameter
`r * ncols + c + 1` is row `r`, column `c`. Every per-driver parameter rule
(`bool_param_as_int`, `decimal_param_as_varchar`, `bigint_param_as_string`,
`null_param_as_varchar`, `wchar_as_utf8`, …) applies to each row-group exactly as it
does to a single row, because it is the same `SlotFromArrow` + `SQLBindParameter` code.
Round trips drop by a factor of K.

It turned out to be faster than parameter arrays on nearly every driver where arrays
work too, so it is the default for ingest; arrays stay ahead of it only for MariaDB
Connector/ODBC, which sends a bound array as one `COM_STMT_BULK_EXECUTE`. Only the
`INSERT` bulk ingest generates is rewritten — a query the caller wrote is executed as
written.

**Choosing K.** ODBC has no `SQLGetInfo` for "how many parameters will you take"; the
nearest thing is `SQL_MAX_STATEMENT_LEN`, and most drivers answer 0 = unknown even for
that. So K starts from a budget and is probed downwards:

* 2000 parameters, at most 1000 row-groups — SQL Server's own limits are 2100
  parameters and 1000 `VALUES` row constructors, and every other tested backend takes
  more. Four columns therefore give K = 500.
* clipped by `SQL_MAX_STATEMENT_LEN` where the driver gives one (sqliteodbc says 16 KB,
  which caps a 121-column table at 42 row-groups) and by a 16 MB parameter-scratch
  budget.
* then `SQLPrepare`d. A refusal halves K and asks again, down to 2; so does a refusal
  from the *first* `SQLExecute`, before anything has been written. The ceiling that
  survives is remembered on the connection, so the search is paid once.
* `adbc.odbc.rows_per_insert` overrides the budget (0 = automatic, 1 = off).

**Falling back.** Before the ingest transaction opens, a two-row `SQLPrepare` asks
whether the server has the form at all. Three answers:

| server | probe result |
|---|---|
| Oracle | `VALUES (…),(…)` is `ORA-00933`; the probe re-asks with `INSERT ALL INTO t VALUES (…) INTO t VALUES (…) SELECT 1 FROM dual` (a keyed quirk on `sqora`, consulted only after the standard form has actually been refused) and uses that |
| SQLite | 2000 parameters is over a 999-variable build's limit; K halves until it prepares |
| ClickHouse | clickhouse-odbc prepares 500 row-groups and refuses to execute them; K halves to 125 |
| Firebird 5 (OdbcFb) | no multi-row `VALUES`, and a `UNION ALL` of `SELECT ? … FROM RDB$DATABASE` will not prepare either (the placeholders need an explicit `CAST`, which would change truncation semantics on append). The probe fails, the connection remembers, and ingest carries on one row at a time |

The whole ingest is still one transaction, so a row the server refuses part way through
rolls the lot back and leaves no half table.

Ingesting `(int32, float64, string, date32)` into a table the benchmark created, on a
connection in autocommit; **before** is `adbc.odbc.rows_per_insert=1`, which takes
exactly the previous code path, **after** is the default. Median of three, the two arms
interleaved so that host load hits both equally. Rows per second:

| Database | 10,000 rows before | after | 50,000 rows before | after |
|---|---:|---:|---:|---:|
| SQLite 3.45 | 400,705 | 459,961 | 634,417 | 866,080 |
| DuckDB | 24,057 | **344,597** | 20,342 | **403,228** |
| PostgreSQL 16 | 102,508 | 312,025 | 104,609 | 417,866 |
| MariaDB 11 | 67,256 | 63,864 | 426,110 | 437,503 |
| MySQL 8.4 | 16,857 | 36,824 | 20,784 | **200,247** |
| SQL Server 2022 | 106,176 | 106,830 | 110,135 | 199,216 |
| MonetDB 11.55 | 12,190 | **138,902** | 11,010 | **206,382** |
| Oracle 23ai | 475 | **23,628** | 514 | **44,008** |
| IBM Db2 12.1 | 670 | 1,010 | — | — |
| Firebird 5 | 4,420 | 4,477 | — | — |
| ClickHouse 26 (300 rows) | 16 | **911** | — | — |
| CockroachDB 26 | 1,205 | 14,259 | — | — |

The biggest wins are exactly the drivers the change was for: DuckDB 14x, MonetDB 11x,
ClickHouse 57x, MySQL 10x at 50,000 rows, Oracle 50–86x (Oracle's parameter arrays are
accepted and then abandoned part way through a batch, so it had been paying close to
one execute per row). PostgreSQL and SQL Server gain 2–4x by preferring the multi-row
form over their working parameter arrays; MariaDB keeps its arrays and is unchanged;
Firebird has no multi-row form to use and is unchanged. Db2's clidriver is not
round-trip bound on this workload, so it gains little.

At 10,000 rows the fixed cost of the `CREATE TABLE` and the first prepare is a visible
share of the total, which is why SQL Server and MySQL look flatter there than at 50,000.
These were taken while the same host was serving other containers; treat them as
ratios, not as three significant figures.

## Beating the native driver: partitioned reads

The section above ends at a wall. adbcbridge reads PostgreSQL within a couple of percent
of the raw-ODBC floor, and the floor is 1.6x slower than the native
`adbc_driver_postgresql` — because the floor *is* psqlodbc's text-protocol decoding, and
no bridge can remove the ODBC driver from underneath itself. Shaving the inner loop
cannot win this; the gap is not in our code.

So the way past it is to do work the native driver does not do. ADBC's own
`AdbcStatementExecutePartitions` / `AdbcConnectionReadPartition` exist for splitting one
query across several connections, and PostgreSQL's `ctid` makes an exact split cheap: see
[Partitioned reads](../README.md#partitioned-reads-executepartitions) in the README for
the mechanism and the rules for when the driver refuses to split.

Reproduce with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
POSTGRES_ODBC_DRIVER=/path/to/psqlodbcw.so \
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python bench/partition_bench.py --pg postgresql://adbc:adbc@127.0.0.1:5432/adbc \
    --table bench10m --reps 5
```

### Setup

PostgreSQL 16.15 in Docker (`--memory=4g`, `shared_buffers=1GB`, `work_mem=64MB`),
`bench1m` / `bench10m` = `(id int4, val float8, txt varchar(32), dt date)`, 65 MB /
651 MB of heap, `VACUUM ANALYZE`d, queried as `SELECT id, val, txt, dt FROM …`. Same
machine as the rest of this file (i9-13900HK, 14C/20T); host load average was 3.8–7.1
throughout, so a quarter to a third of the machine was already busy with other work.
Treat the ratios as solid and the third significant figure as noise.

Every timing is **wall clock, end to end** — opening the connections, executing,
fetching and building the Arrow table — because that is what a caller waits for. The
variants are interleaved within each repetition so a drift in load lands on all of them,
and the median of 5 is reported with the spread. Every adbcbridge run is checksummed
(count, sum, min and max of every column) against the single-connection read, and the
benchmark aborts on a mismatch: none occurred.

### 1,000,000 rows

| variant | median | min | max | Mrow/s | vs native |
|---|---:|---:|---:|---:|---:|
| adbcbridge, N=1 | 0.526 s | 0.511 | 0.544 | 1.90 | 0.40x |
| adbcbridge, N=2 | 0.298 s | 0.283 | 0.305 | 3.35 | 0.70x |
| adbcbridge, N=4 | 0.199 s | 0.197 | 0.224 | 5.02 | 1.04x |
| **adbcbridge, N=8** | **0.156 s** | 0.145 | 0.166 | **6.43** | **1.34x** |
| native `adbc_driver_postgresql` | 0.208 s | 0.207 | 0.225 | 4.81 | 1.00x |
| *floor*: raw `SQLBindCol`+`SQLFetch` | 0.493 s | 0.483 | 0.513 | 2.03 | — |

### 10,000,000 rows

| variant | median | min | max | Mrow/s | vs native |
|---|---:|---:|---:|---:|---:|
| adbcbridge, N=1 | 5.145 s | 5.047 | 5.415 | 1.94 | 0.36x |
| adbcbridge, N=2 | 2.747 s | 2.601 | 2.888 | 3.64 | 0.68x |
| adbcbridge, N=4 | 1.712 s | 1.507 | 1.747 | 5.84 | 1.10x |
| **adbcbridge, N=8** | **1.216 s** | 1.207 | 1.308 | **8.22** | **1.54x** |
| native `adbc_driver_postgresql` | 1.875 s | 1.774 | 1.894 | 5.33 | 1.00x |
| *floor*: raw `SQLBindCol`+`SQLFetch` | 4.898 s | 4.745 | 5.004 | 2.04 | — |

**The honest summary: parity at N=4, ~1.5x at N=8.** One connection is 0.36x native,
which is the ODBC boundary and has not moved. Four connections draw level (1.04x at 1 M,
1.10x at 10 M). Eight are a third to half again faster than the native driver. That is
the whole result — the win is not in our per-row code, it is in using four to eight cores
where the native driver uses one.

Note also that N=1 (5.145 s) and the raw ODBC floor (4.898 s) are within 5% of each
other at 10 M rows, which is the same finding as the section above at a ten times larger
size: our Arrow conversion is nearly free, and the read is psqlodbc.

### Where the speed-up stops, and why

Scaling is real but sublinear, and it flattens hard past eight:

| step | 1M | 10M |
|---|---:|---:|
| N=1 -> N=2 | 1.76x | 1.87x |
| N=2 -> N=4 | 1.50x | 1.60x |
| N=4 -> N=8 | 1.28x | 1.41x |
| N=8 -> N=16 (10 M only) | — | 1.07x |

Three measurements say what the limit is and, just as usefully, what it is not.

**It is not the Python GIL.** Reading the same descriptors on a `ProcessPoolExecutor`
instead of a `ThreadPoolExecutor` is not faster — 0.80x to 0.99x across N=1..16, i.e.
slightly *slower* once the Arrow tables have to be pickled back. The ODBC fetch and the
Arrow conversion both release the GIL, so threads are already parallel.

**It is not that the slices get less efficient.** One slice of an 8-way split of
`bench10m` takes 0.731 s read on its own; one eighth of the 5.801 s single-connection
read is 0.725 s. A slice does exactly its proportional share of work at exactly the
unsliced rate — the `ctid` range scan costs nothing extra.

**It is CPU, most of it on the client.** Measuring the server container's cgroup CPU
against the client process's:

| N | wall | client CPU (user+sys) | server CPU | cores busy |
|---|---:|---:|---:|---:|
| 1 | 6.13 s | 7.79 s | 1.83 s | ~1.6 |
| 4 | 2.00 s | 8.60 s | 2.59 s | ~5.6 |
| 8 | 1.42 s | 11.27 s | 3.37 s | ~10.3 |

Two things follow. First, the work is overwhelmingly client-side: psqlodbc decoding
PostgreSQL's text protocol costs four times what the server spends producing it, which is
the same thing the floor measurement says. Second, doing it eight ways costs **1.52x more
total CPU** than doing it once (14.6 core-seconds against 9.6) — per-connection setup,
eight simultaneous 8 MiB rowset buffers and eight Arrow builders competing for 24 MiB of
L3. At N=8 about ten cores are busy on a fourteen-core machine that was already a quarter
loaded, so N=16 adds only 7%: there are no more cores to give.

The practical reading is that N should be roughly the number of cores you can spare, that
four to eight is where the returns are, and that a bigger table pushes the useful N up
(the 10 M numbers scale better than the 1 M ones at every step, because the fixed
per-connection cost is amortised further).

### Prefetch: measured, and largely worthless

The other mechanism tried was overlapping `SQLFetch` with the Arrow conversion on a
background thread (`adbc.odbc.prefetch`; see [Prefetch](../README.md#prefetch)). It
works, it is correct, and it is worth almost nothing:

| driver | 1,000,000 rows | prefetch=0 | prefetch=1 | prefetch=2 |
|---|---|---:|---:|---:|
| psqlodbc, default | PostgreSQL 16 | 0.534 s | 0.535 s (1.00x) | 0.528 s (1.01x) |
| psqlodbc, `UseDeclareFetch=1` | PostgreSQL 16 | 0.651 s | 0.617 s (1.06x) | 0.592 s (1.10x) |
| sqliteodbc | SQLite 3.45 | 0.597 s | 0.592 s (1.01x) | 0.591 s (1.01x) |
| MariaDB Connector/ODBC | MariaDB 11 | 0.303 s | 0.291 s (1.04x) | 0.324 s (0.93x) |

The reason is structural, not a shortcoming of the implementation: **there is no socket
wait inside `SQLFetch` to hide.** psqlodbc in its default mode drains the entire result
set into client memory during `SQLExecDirect`, so every subsequent `SQLFetch` is a memory
copy; sqliteodbc is in-process and has no socket at all; MariaDB Connector/ODBC likewise
buffers the result. Only psqlodbc's cursor mode (`UseDeclareFetch=1`, which is *slower*
overall) leaves real network waits in `SQLFetch`, and that is the one row of the table
where prefetch earns anything — 6–10%. At depth 2 on MariaDB it is a net loss, because a
second 8 MiB rowset buffer costs more in cache than the overlap saves.

This is why prefetch is off by default and why partitioning is the mechanism that carries
the result. SQL Server was not measured: the only SQL Server container on this host
belongs to another workload and starting a second one was out of scope.

## SQL Server's `VARCHAR(MAX)`: the cliff is real, and it is small

The [`TEXT` column cliff](#the-text-column-cliff) above closes wherever a truncated bound
value can be re-read. msodbcsql 18 was the one driver left offering neither repair route,
so a `VARCHAR(MAX)` column still costs the whole result set its block cursor. This
section is what a standalone C probe against msodbcsql 18.06.0002 / SQL Server 2022
16.00.4250 found when asked what the driver *reports* and, separately, what it *does*.

### What it reports

```
SQL_GETDATA_EXTENSIONS              = 0x00000004
    ANY_COLUMN=0 ANY_ORDER=0 BLOCK=1 BOUND=0 OUTPUT_PARAMS=0
SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1 = 0x0000e001   ABSOLUTE=0
SQL_STATIC_CURSOR_ATTRIBUTES1       = 0x0008124f   ABSOLUTE=1
SQL_DYNAMIC_CURSOR_ATTRIBUTES1      = 0x0001fe47   ABSOLUTE=1
```

No `SQL_GD_BOUND`, and no `SQL_CA1_ABSOLUTE` on the forward-only cursor the reader uses.
Both repair routes end in `SQLGetData` on a bound column, so both are ruled out on paper.

### What it does

The probe ran `SQLGetData` on a bound column and `SQLFetchScroll(SQL_FETCH_ABSOLUTE)`
under all four cursor types crossed with all four `SQL_ATTR_CONCURRENCY` values, and
again with `SQL_ATTR_CURSOR_SCROLLABLE` set. The driver does not lie in either direction:

* `SQLGetData` on a **bound** column fails in every one of the sixteen combinations —
  `HY109 Invalid cursor position` on a forward-only cursor (where `SQLSetPos` itself is
  refused), `07009 Invalid Descriptor Index` on STATIC, DYNAMIC and KEYSET, where
  `SQLSetPos(n, SQL_POSITION)` succeeds but the column being bound makes `SQLGetData`
  illegal. That is `SQL_GD_BOUND=0` honoured exactly.
* `SQLFetchScroll(SQL_FETCH_ABSOLUTE)` is refused on a forward-only cursor
  (`HY106 Fetch type out of range`, and from the server "the fetch type absolute cannot
  be used with forward only cursors") and **works correctly** on STATIC, DYNAMIC and
  KEYSET: it returns the right row, and a plain `SQLFetch` afterwards resumes at the
  right place. `SQL_ATTR_CURSOR_SCROLLABLE` changes nothing that the cursor type has not
  already decided. `STATIC` with any concurrency other than `SQL_CONCUR_READ_ONLY` is
  `HYC00 Optional feature not implemented`.
* msodbcsql 18 has no `SQL_SOPT_SS_*` statement attribute and no connection keyword that
  bears on this; `SQL_SOPT_SS_CURSOR_OPTIONS` selects fast-forward-only server cursors,
  which are forward-only and so cannot reposition either.

So a repair route exists, but only behind a scrollable cursor — and that is the trap.

### Why the repair costs more than the cliff

A scrollable cursor is a **server** cursor. It has to be asked for before `SQLExecute`,
and the moment SQL Server opens one the read stops being a streamed default result set.
Reading 100,000 rows of `(int, float, VARCHAR(50), date, VARCHAR(MAX))`, medians of 5
straight through the ODBC API:

| how the wide column is read | median | rows/s |
|---|---:|---:|
| forward-only, rowset 1, `SQLGetData` (**what the reader does today**) | 0.042 s | 2,353,434 |
| static server cursor, rowset 1024, bound at 2 KiB | 0.171 s | 583,401 |
| static server cursor, rowset 1024, `SQLSetPos` + `SQLGetData` per row | 6.645 s | 15,050 |
| static server cursor, rowset 1, `SQLGetData` per row | 12.784 s | 7,822 |

The cheapest repair is **4x slower** than the cliff it repairs, and the one that keeps
`SQLGetData` is 150x slower. The third route — re-reading just the wide column through a
second statement keyed on the row's identity — cannot be done without changing results:
an arbitrary `SELECT` has no key to key on, and msodbcsql reports no bookmark support
(`SQL_CA1_BOOKMARK=0`) on any cursor a forward-only read could use.

### And the cliff is far smaller than the fetch count suggests

The "100,000 `SQLFetch` calls instead of 13" framing counts ODBC calls, not round trips.
It matters **only if the rowset size is set before the cursor is opened**, which the
reader does not do: `ReaderBind()` runs on the first batch, after `SQLExecDirect`, because
it needs `SQLDescribeCol` first. That ordering turns out to decide everything. Counting
`sys.dm_exec_connections.num_reads` and `sys.dm_exec_cursors` around a 100,000-row read:

| `SQL_ATTR_ROW_ARRAY_SIZE` set… | fetches | server reads | server cursors | median |
|---|---:|---:|---:|---:|
| …before `SQLExecDirect`, rowset 1 | 100,000 | 1 | 0 | 0.028 s |
| …before `SQLExecDirect`, rowset 2 | 50,000 | 50,002 | 1 | 4.332 s |
| …before `SQLExecDirect`, rowset 1024 | 98 | 99 | 1 | 0.114 s |
| …after `SQLExecDirect`, rowset 1 | 100,000 | — | — | 0.026 s |
| …after `SQLExecDirect`, rowset 1024 | 98 | — | — | 0.022 s |

Asking for a rowset **before** execution makes msodbcsql open a server cursor and pay one
round trip per rowset — 50,002 of them at rowset 2. Asking **after** keeps the default
result set: one server read for the whole table, and every rowset size from 1 to 8192
lands within noise of 4.6 M rows/s. Dropping to rowset 1 on this driver therefore costs
the *ODBC call overhead* and nothing else.

What the wide column actually costs, end to end through `fetch_arrow_table()`, 100,000
rows, medians of 7 (host load average 3.9):

| table | median | rows/s |
|---|---:|---:|
| `(int, float, VARCHAR(50), date)` | 0.0219 s | 4,556,557 |
| the same plus `VARCHAR(50)` holding the same short values | 0.0305 s | 3,283,791 |
| the same plus `VARCHAR(MAX)` holding the same short values | 0.0516 s | 1,938,393 |

The honest gap is the last two rows — 0.0305 s against 0.0516 s — because the first row
is reading one column fewer. At the ODBC layer the whole of what a perfect fix could
recover is 0.042 s → 0.034 s, **19%**, of which only 10% is the block rowset and the rest
is binding instead of `SQLGetData`.

**Conclusion: no change.** The cliff is structural on msodbcsql — the driver genuinely
offers no repair route on a forward-only cursor, and the probe confirms it does what it
says. It is also worth 19% rather than the 100x the fetch count implies, and every repair
route measured costs between 4x and 150x more than that.

## Ingest DDL: two servers where the widest text type is a trap

Chasing why Db2 bulk ingest ran at half of `odbc-api`'s rate turned up something else
entirely, and the same defect on SQL Server. An Arrow `utf8` column has no width, so
generated ingest DDL asks the driver for its `SQL_LONGVARCHAR` type. On two servers that
type is one the server barely supports.

### Db2: `LONG VARCHAR`

`SQLGetTypeInfo(SQL_LONGVARCHAR)` on IBM's clidriver answers **`LONG VARCHAR`**, which IBM
deprecated in Db2 9. Writing 20,000 rows of `(INTEGER, DOUBLE, <string>, DATE)` straight
through the ODBC API with no adbcbridge in the way, medians of 3 — this is the *server*,
not the bridge:

| `txt` column type | multi-row `VALUES`, K=500 | parameter arrays, 8192 |
|---|---:|---:|
| `VARCHAR(20)` | 429,865 rows/s | 432,857 rows/s |
| `VARCHAR(4000)` | 448,002 rows/s | 376,764 rows/s |
| `VARCHAR(32672)` | **516,459 rows/s** | 419,435 rows/s |
| `CLOB(1048576)` | 402,356 rows/s | 326,541 rows/s |
| `LONG VARCHAR` | **737 rows/s** | 7,519 rows/s |

Every string type Db2 has is within 25% of the fastest. `LONG VARCHAR` alone is 700x off
it. It is also close to unusable: `ORDER BY`, `GROUP BY` and `DISTINCT` on one are all
`SQL0134N`, "improper use of a string column" — so a table adbcbridge created could not be
sorted or grouped on its own text column.

This is what made Db2 look like a bridge problem. In-driver instrumentation of a
20,000-row ingest showed 160,000 `SQLBindParameter` calls costing 0.052 s in total and all
the rest of the time — 141 ms per 250-row `SQLExecute` — inside the driver. `strace`
showed 40 `sendto`/`recvfrom` pairs for 5,000 rows: not round trips, not `SQLDescribeParam`
(2.2 µs a call), not the 32-bit `SQLLEN` quirk, not the statement-length ceiling
(Db2 reports `SQL_MAX_STATEMENT_LEN` = 2,097,152, ample), and not DDL (`CREATE TABLE` is
9 ms). It was the column type all along, which is also why multi-row batching "gave Db2
almost nothing": the `LONG VARCHAR` write cost swamps every path equally.

The fix (`ddl_string_as_max_varchar`) asks for the widest VARCHAR the driver reports
instead — `VARCHAR(32672)` on Db2. That holds all but the last 28 bytes of what `LONG
VARCHAR` could, and unlike `LONG VARCHAR` it can be sorted, grouped and de-duplicated.

Db2 bulk ingest through `adbc_ingest`, autocommit off, DDL + data + commit, row count
verified afterwards, medians of 5–7 interleaved (host load average 1.4–2.2):

| | before | after | |
|---|---:|---:|---:|
| 20,000 rows, multi-row `VALUES` (default) | 3,960 rows/s | 116,591 rows/s | **29x** |
| 20,000 rows, `adbc.odbc.array_binding=true` | 3,948 rows/s | 117,412 rows/s | **30x** |
| 20,000 rows, row-at-a-time | 3,338 rows/s | 36,711 rows/s | 11x |
| 100,000 rows, multi-row `VALUES` | 600 rows/s | 440,492 rows/s | **734x** |
| 100,000-row read back | 92,989 rows/s | 1,565,590 rows/s | **17x** |

The 100,000-row row is the striking one: with `LONG VARCHAR` the cost per row grew with
the row count, so the collapse got worse the more there was to write. It is linear again.

Note the first two rows against each other: multi-row `VALUES` and parameter arrays are
now within 1% on Db2, so `prefer_param_arrays` is **not** warranted here — the hypothesis
that `odbc-api` won by using arrays where we prefer multi-row `VALUES` is not what was
happening. Both paths were writing into the same pathological column type.

### SQL Server: `TEXT`

The same question asked of msodbcsql: `SQLGetTypeInfo(SQL_LONGVARCHAR)` answers **`text`**,
which Microsoft deprecated in SQL Server 2005. A `text` column cannot be sorted (error
306), grouped (306), de-duplicated (`SELECT DISTINCT`, 421) — or even compared:
`WHERE s = 'a'` is error 402, "the data types text and varchar are incompatible". A table
adbcbridge created on SQL Server could not be filtered on its own string column.

`NVARCHAR(MAX)`, which is what Microsoft documents as the replacement, holds the same
2 GB, is Unicode rather than the database's code page, and is faster both ways.
20,000-row ingest and 100,000-row read through adbcbridge, medians of 5, interleaved:

| `txt` DDL type | ingest rows/s | fetch rows/s |
|---|---:|---:|
| `TEXT` (before) | 172,081 | 859,215 |
| `VARCHAR(MAX)` | 220,123 | 2,962,631 |
| `NVARCHAR(MAX)` (after) | **233,303** | **3,172,747** |
| `NVARCHAR(4000)` | 242,106 | 4,848,066 |

`ddl_string_type_name` carries the fixed spelling, since `MAX` is a word rather than a
number and cannot be derived from `SQLGetTypeInfo`.

Both fixes are guarded by `text_sortable` in `tests/compat/test_matrix.py`, which reads an
ingest-created table back with `ORDER BY`, `GROUP BY` and `DISTINCT` on its text column.
It is opt-in rather than universal because the same restriction is genuine on some servers
whatever adbcbridge does — Oracle's `SQL_LONGVARCHAR` is `CLOB`, and `ORDER BY` on a
`CLOB` is ORA-00932 — so it is claimed only where it has been checked: sqlite, duckdb,
mssql and db2.

## The 1.2x threshold against the native drivers

The bar: **mean of three runs, at least 1.2x faster than the native ADBC driver, on both
fetch and ingest.** `bench/native_threshold.py` measures exactly that and prints
`PASS`/`FAIL` against it. It runs a fresh process per timed run and interleaves the sides
(A,B,A,B,A,B, never all-A-then-all-B), so a drift in machine load lands on both. Every
clock covers the whole job end to end — opening connections, executing, and building the
Arrow table for fetch; opening connections, consuming the Arrow table, executing and
committing for ingest. Correctness is checked alongside speed: each ingest is compared
against the source table by row count and a per-column checksum computed **in SQL on the
server** (so it does not depend on the driver under test), and each fetch against a
reference read. A checksum mismatch is a `FAIL` whatever the clock says.

```
ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \
  python bench/native_threshold.py --database postgres --rows 1000000 --runs 3 \
      --partitions 8 --ingest-connections 16
```

### Machine

20-core i9-13900HK, PostgreSQL 16.15 in Docker (`shared_buffers=2GB`, `work_mem=256MB`),
psqlodbc, `adbc_driver_postgresql` and `adbc_driver_sqlite` from the same venv. Four
columns: `bigint, double precision, text, date`. **The host was not quiet**: two other
builds were running throughout and the one-minute load average sat between 5 and 11 on 20
cores. That inflates the spread on both sides but not the ratio between them, and every
figure below is the mean of three interleaved runs with the min and max shown. The ingest
rows were re-measured when the array form landed, on the same host and no quieter — a
desktop compositor sat at 2.7 cores for the whole session and the one-minute load average
moved between 1.7 and 10.

### PostgreSQL

Ingest here is the **array form** described in
[PostgreSQL array ingest](#postgresql-array-ingest-what-it-buys-and-what-it-does-not);
the figures the multi-row `INSERT` reached before it are in that section.

| rows | axis | ours (mean) | native (mean) | spread ours | spread native | ratio | verdict |
|---:|---|---:|---:|---|---|---:|---|
| 1 M | fetch, 8 partitions | 0.186 s | 0.339 s | 0.168–0.200 | 0.334–0.346 | **1.83x faster** | **PASS** |
| 1 M | ingest, 1 connection | 1.264 s | 0.357 s | 1.208–1.321 | 0.341–0.372 | 0.28x (3.5x slower) | **FAIL** |
| 1 M | ingest, 12 connections | 0.354 s | 0.331 s | 0.328–0.372 | 0.325–0.342 | 0.94x | **FAIL** |
| 1 M | ingest, 16 connections | 0.317 s | 0.322 s | 0.309–0.331 | 0.317–0.325 | 1.02x (0.9–1.0x over repeats) | **FAIL** |
| 10 M | fetch, 12 partitions | 1.265 s | 2.074 s | 1.263–1.268 | 2.059–2.089 | **1.64x faster** | **PASS** |
| 10 M | ingest, 16 connections | 3.486 s | 2.417 s | 3.193–3.751 | 2.366–2.478 | 0.69x (1.44x slower) | **FAIL** |

Fetch clears the bar at both sizes. Ingest does not, at any connection count — it reaches
parity at 1 M rows and stays about 1.4x behind at 10 M.

### SQLite

| rows | axis | ours (mean) | native (mean) | ratio | verdict |
|---:|---|---:|---:|---:|---|
| 200 k | fetch | 0.148 s | 0.167 s | 1.13x faster | **FAIL** (marginal) |
| 200 k | ingest, 1 connection | 0.224 s | 0.087 s | 0.39x | **FAIL** |
| 1 M | fetch | 0.715 s | 0.380 s | 0.53x | **FAIL** |
| 1 M | ingest, 1 connection | 1.114 s | 0.344 s | 0.31x | **FAIL** |

SQLite fails both axes. Fetch is close to parity at 200 k rows — where the native driver's
fixed costs still matter — and falls behind as the row count grows and per-row conversion
starts to dominate; there is no `ctid` to split on, so partitioning does not apply and the
read stays on one connection. Parallel ingest makes SQLite *slower* (0.273 s against
0.224 s at 200 k on four connections): one file, one writer, and the workers simply
contend for the write lock.

### Why PostgreSQL ingest cannot reach 1.2x this way

The native driver ingests with `COPY … (FORMAT binary)` through libpq. That is not a
faster version of what we do, it is a cheaper mechanism. Measured on the 1 M-row load,
client CPU by `getrusage` and server CPU from the container's `cpu.stat`:

| | wall | client CPU | server CPU | total CPU |
|---|---:|---:|---:|---:|
| native, `COPY` binary | 0.359 s | 0.185 s | 0.248 s | **0.43 s** |
| ours, multi-row `INSERT`, 1 connection | 2.330 s | 0.996 s | 1.019 s | **2.02 s** |

We burn **4.7x the CPU per row** — about 2.0 µs against 0.43 µs. Half of it is ours
(a `SQLBindParameter` per cell, plus the Arrow-to-text conversion) and half is the
server's (parsing SQL text where `COPY` parses a binary stream). Note also that at one
connection the wall clock is well above the sum of the two CPU figures: client and server
ping-pong synchronously rather than overlapping.

Parallelism converts that CPU into wall time until the machine runs out of cores, and
that is exactly what it does — 2.20 s to 0.505 s, a 4.6x speed-up — but it cannot make
the work smaller, and it does not scale cleanly either: total CPU *grows* with the worker
count (2.0 CPU-s at N=1, 3.0 at N=8, 4.6 at N=16), so the curve flattens at 12–16 workers.
Splitting the workers across separate target tables instead of one made no difference
(0.437 s against 0.525 s at N=16, inside the noise), so this is not PostgreSQL's relation
extension lock — it is contention and scheduling cost against a box that is already busy.

Raising the parameter ceiling is worth a little on its own but nothing like enough: at
K=1000 instead of the default 500 (`ADBC_ODBC_MULTIROW_MAX_PARAMS`, 2000, divided by four
columns) the single-connection load goes from 2.23 s to 1.98 s, about 12%. Going the other
way confirms the multi-row form is the right one: `adbc.odbc.rows_per_insert=1`, which
falls back to parameter arrays and row-at-a-time, takes 9.73 s.

The one lever big enough to matter is to stop sending row values as SQL text at all. That
is the array form below. It is now implemented, it is worth a lot, and it still does not
reach the bar.

### Parallel ingest: scaling

1 M rows, PostgreSQL, `adbc.odbc.ingest_connections`, checksum-verified at every N. The
first two rows are the multi-row `INSERT` form; the third is the same driver with the
array form of the next section, which is what runs against PostgreSQL now:

| connections | 1 | 4 | 8 | 10 | 12 | 16 | 20 | 24 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| multi-row, wall (s) | 2.33 | 0.98 | 0.90 | — | 0.53 | 0.58 | — | — |
| multi-row, 10 M rows, wall (s) | 24.45 | — | 5.01 | — | 4.51 | **4.26** | 4.52 | — |
| array form, wall (s) | 1.40 | 0.49 | 0.51 | 0.37 | 0.33 | **0.34** | — | 0.36 |

The array-form row is means of 3–5 interleaved runs taken over about twenty minutes on a
host whose one-minute load average moved between 1.7 and 10 (a desktop compositor pinned
at 2.7 cores throughout); the spread within an N is comparable to the difference between
N=10 and N=16, so read it as "flat from 12 onwards", not as a ranking. The shape is the
same as before — the curve flattens where the server, not the client, becomes the limit —
but it flattens lower and it gets there with about a third less total CPU.

A stream of a single 1 M-row batch fans out just as well as one of many batches (0.487 s
at N=12), because the driver slices batches longer than
`ADBC_ODBC_INGEST_SLICE_ROWS` before queueing them.

## PostgreSQL array ingest: what it buys, and what it does not

Against PostgreSQL the driver sends a whole column of a batch as **one array parameter**
and lets the server expand it, instead of binding K×ncols separate cells:

```sql
INSERT INTO t ("a", "b", "c", "d")
  SELECT * FROM unnest(?::bigint[], ?::float8[], ?::text[], ?::date[])
```

That is `ncols` parameters per statement however many rows it carries. See
[Bulk ingest](../README.md#bulk-ingest) for the guard (it is a PostgreSQL-only quirk, and
several servers speak the PostgreSQL wire protocol without being PostgreSQL) and for the
Arrow types it covers. Everything below is 1,000,000 rows of
`(bigint, float8, text, date)` into PostgreSQL 16.15 in Docker, over psqlodbc 16.

### The floor, measured without the driver in the way

A standalone C program against psqlodbc — no adbcbridge, no Arrow, values generated in a
loop — writing 1,000,000 rows of `(bigint, float8, text, date)` into a fresh table on one
connection. Wall clock, client CPU by `getrusage`, server CPU from the container's
`cpu.stat`:

| shape | rows per statement | wall | client CPU | server CPU | total CPU |
|---|---:|---:|---:|---:|---:|
| multi-row `INSERT` | 250 | 2.113 s | 0.837 s | 1.063 s | 1.90 s |
| multi-row `INSERT` | 1000 | 1.789 s | 0.696 s | 0.900 s | 1.60 s |
| `unnest` arrays | 1000 | 1.487 s | 0.366 s | 0.983 s | 1.35 s |
| `unnest` arrays | 10000 | **1.337 s** | **0.325 s** | 0.920 s | **1.25 s** |
| `unnest` arrays | 100000 | 1.371 s | 0.314 s | 0.986 s | 1.30 s |
| native `COPY` binary (reference) | — | 0.359 s | 0.185 s | 0.248 s | **0.43 s** |

The array form does exactly what it was supposed to do on **our** side: client CPU falls
from 0.70 to 0.33 CPU-s, and 0.26 of the 0.33 that is left is nothing but rendering the
values as text (a formatting-only run of the same loop, with no server at all, costs
0.259 CPU-s). Round trips fall by a factor of 10–100. What it does *not* touch is the
server: 0.92 CPU-s against 0.90 for the multi-row form. Ten thousand rows per statement is
the sweet spot and is what `ADBC_ODBC_ARRAY_INGEST_ROWS` is set to; 1,000 gives most of it
and 100,000 gives nothing more.

### Where the server's 0.92 CPU-s goes

Same probe, with the statement replaced so that each stage can be measured on its own
(1 M rows, four arrays, 10,000 rows per statement):

| statement | server CPU |
|---|---:|
| `SELECT array_length(?::bigint[],1) + …` — parse the four array literals only | 0.315 s |
| `SELECT count(*) FROM unnest(?::bigint[], …)` — and expand them | 0.501 s |
| `INSERT INTO t SELECT * FROM unnest(…)` — and insert the rows | 0.920 s |

So parsing is 0.32, expansion 0.19, and the insert itself 0.42 — against `COPY`'s **0.248
for the whole job, insert included**. Making the parse cheaper cannot close that; the
insert alone is already 1.7x `COPY`'s total.

### The wall the insert hits: WAL

`COPY` does not write rows through the executor. It batches them into `heap_multi_insert`,
which emits one WAL record per page-batch of tuples; `INSERT` emits one per row, whatever
shape the statement has. WAL bytes generated by 1,000,000 rows of identical data into an
identical table, from `pg_current_wal_lsn()` before and after:

| | WAL bytes |
|---|---:|
| multi-row `INSERT` | 96,406,344 |
| `unnest` array `INSERT` | 96,305,696 |
| native `COPY` binary | 48,765,856 |

**Exactly 2x**, and the array form does not change it by 0.1%. (`CREATE TABLE AS SELECT`
is no different: 88.3 MB against 88.3 MB for the same `INSERT … SELECT`.)

That is also why parallelism stops paying. Sampling `pg_stat_activity` 25 times during a
16-way array ingest, 357 active-backend observations:

| state | share |
|---|---:|
| running on CPU | 46% |
| `LWLock` / `WALInsert` | 20% |
| `LWLock` / `BufferContent` | 18% |
| `LWLock` / `WALWrite` | 10% |
| `Lock` / `extend`, other | 6% |

Over half the backends' time is spent waiting on WAL and buffer locks, and every extra
worker makes that worse rather than better. Twice the WAL records and twice the WAL bytes
is a property of `INSERT` in PostgreSQL 16, not of how the statement is written, so no
statement an ODBC driver can send gets underneath it.

### And `COPY` really is out of reach

`SQLExecDirect(hstmt, "COPY t FROM STDIN")` through psqlodbc does not fail — it **hangs**.
libpq puts the connection into `PGRES_COPY_IN` and psqlodbc has no API through which an
application could call `PQputCopyData`, so the call never returns and the process has to be
killed. Embedding the data in the statement text (`COPY … FROM STDIN;\n1\ta\n\\.\n`, which
is psql's `\copy`, not the wire protocol) does not reach the server as data either.

### End to end, through the driver

Interleaved A/B of two builds of this driver that differ only in whether the quirk is
set, plus the native driver, one timed ingest per process, 1 M rows, PostgreSQL 16.15:

| | 1 M, N=1 (3 runs) | 1 M, N=16 (5 runs) | 10 M, N=16 (3 runs) |
|---|---:|---:|---:|
| multi-row `INSERT` | 2.745 s | 0.446 s | 7.035 s (5.18–8.49) |
| `unnest` arrays | **1.397 s** | **0.340 s** | **4.024 s** (3.34–5.11) |
| native `COPY` binary | 0.409 s | 0.334 s | 2.878 s (2.80–2.92) |
| array form against multi-row | **1.97x faster** | **1.31x faster** | **1.75x faster** |
| array form against native | 0.29x | **1.02x** | 0.72x |

And through `bench/native_threshold.py` itself, mean of three, checksum-verified. "Before"
is the same harness against a build with the quirk forced off, run within the hour:

| N | ours before | ours after | native | ratio before | ratio after |
|---:|---:|---:|---:|---:|---:|
| 1 | 2.215 s | 1.264 s | 0.357 s | 0.16x | 0.28x |
| 12 | — | 0.354 s | 0.331 s | — | 0.94x |
| 16 | 0.416 s | 0.317 s | 0.322 s | 0.77x | **1.02x** |
| 16, 10 M rows | — | 3.486 s | 2.417 s | — | 0.69x |

**Verdict: still `FAIL`.** At 1 M rows the array form takes ingest from 0.77x of the
native driver to parity. Three whole-harness runs at N=16 over the session gave 1.02x
(load 5.0), 0.94x (load 6.9) and 0.86x (load 4-6 with a 10 M benchmark finishing
alongside); the multi-row form measured 0.77x under the same conditions and never came
near 1x. Call it **0.9-1.0x**, and note that we are the side that needs eight cores while
the native driver needs one and a half, so a busy host costs us more than it costs the
reference. The bar is 1.2x, which needs 0.268 s against the 0.317 s of the best run. At 10 M rows it improves the absolute time by 1.75x but stays
at 0.69–0.72x: the native driver's `COPY` amortises better at that size, so the ratio does
not move. **The 10 M figures are the least trustworthy on this page** — the same build
measured 3.34 s, 3.49 s, 3.62 s and 5.11 s across four runs of the same load within half an
hour, and the multi-row build swung from 5.18 s to 8.49 s. A 10 M-row ingest writes about
a gigabyte of WAL and the host's checkpointing and page cache dominate the spread. The
1 M-row numbers repeat to within a few percent and are the ones to read.

Where the remaining 1.2x would have to come from, and why none of it is available:

- **Not from more workers.** N=24 is no better than N=16 (0.361 s against 0.340 s), and the
  wait-event sample above says why.
- **Not from a cheaper statement.** The insert phase alone (0.42 CPU-s) already exceeds
  `COPY`'s whole cost (0.248), because of the WAL asymmetry.
- **Not from a cheaper client.** Everything the client still spends — 0.33 CPU-s at N=1 —
  is 16-way parallel at N=16, i.e. about 20 ms of the 317 ms.
- **The fixed cost is now a visible share.** Opening the connections, the `CREATE TABLE`
  and the commit cost 82–114 ms at N=16 against 12 ms at N=1 (measured by ingesting 64
  rows), roughly a third of the 317 ms. That is PostgreSQL forking sixteen backends, it is
  inside the clock because a caller waits for it, and the driver already opens the sixteen
  connections concurrently on the worker threads.

So the array form is kept for what it is — a 1.97x on the default single-connection
ingest, a 1.31x at N=16, and 35% less total CPU — and not because it reaches the bar. A
caller who needs native ingest speed against PostgreSQL should let the driver
[delegate](../README.md#native-delegation) to `adbc_driver_postgresql`, which is what
`adbc.odbc.delegate=auto` does with a `postgresql://` URI; the benchmarks here set
`delegate=never` on purpose, to measure the ODBC path.

## Files

- `bench/fetch_bench.py` — the benchmark harness (all three paths, batch-size
  sweep, per-column attribution, unbound-column cliff, cProfile).
- `bench/odbc_floor.c` — raw `SQLBindCol`/`SQLFetch` floor, built on demand by
  the harness with `cc -O2 ... -lodbc`.
- `bench/partition_bench.py` — the partitioned read: adbcbridge at N partitions on N
  threads over N connections, against native `adbc_driver_postgresql` and the raw ODBC
  floor, with a full-result checksum at every N.
- `bench/prefetch_bench.py` — `adbc.odbc.prefetch` at several depths against any ODBC
  driver, checksummed against the depth-0 read.
- `bench/ingest_bench.py`, `bench/matrix_bench.py` — the write path.
- `bench/native_threshold.py` — the 1.2x threshold harness: adbcbridge against the
  native ADBC driver on both fetch and ingest, fresh process per run, interleaved,
  mean of three, checksum-verified, `PASS`/`FAIL`.
- `bench/verify_array_binding.py` — differential check that array binding and the
  row-at-a-time fallback produce identical data and identical rows-affected.
- `tests/test_partitions.py`, `tests/c/test_partition.c` — partition equivalence (every
  partition count over every table size, dead tuples, an empty table, descriptors read out
  of order and concurrently), the single-partition fallback, and the SQL scanner.
- `tests/test_prefetch.py` — prefetch equivalence, an error mid-stream, and aborting a
  stream while the fetch thread is running.
- `text_sortable` in `tests/compat/test_matrix.py` — an ingest-created table read back
  with `ORDER BY`, `GROUP BY` and `DISTINCT` on its text column, which is what Db2's
  `LONG VARCHAR` and SQL Server's `TEXT` refuse.
- `tests/test_parallel_ingest.py` — parallel ingest: equality with the
  single-connection path, one huge batch sliced across workers, an empty stream, fewer
  batches than connections, a failure injected mid-stream, and `NOT NULL`/`PRIMARY KEY`
  violations by one worker.
- `tests/test_pg_array_ingest.py` — the PostgreSQL array form: that it is what runs (a
  statement trigger records `current_query()`), that every awkward value round-trips
  (empty strings, the word `NULL`, braces, commas, quotes, backslashes, newlines, emoji),
  a randomised differential against the row-at-a-time path, the fallbacks (an unsupported
  column type, a target column with no assignment cast, a date the renderer will not
  spell), values wide enough to narrow a statement, and composition with parallel ingest.
- `tests/c/test_multirow.c`, `test_multirow_ingest` in `tests/test_sqlite.py` — the
  multi-row `INSERT` text and its behaviour (NULLs in every row-group position, a batch
  that is not a multiple of K, a single-row batch, a failure that must leave no rows,
  and the parameter-ceiling search).
