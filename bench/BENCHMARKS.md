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
MySQL/MariaDB, `VARCHAR(MAX)` on SQL Server. Reading one back, `SQLDescribeCol`
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
**1.31x** on `(id, bytea)`, and it costs nothing anywhere else. Output is
byte-identical: a table of 5/200/2,000/20,000-byte `bytea` and
10/300/5,000/40,000-character `text` values, with NULLs and empty values, hashes
the same before and after at `batch_size` 1024 and at 7, and the Arrow schema is
unchanged.

psqlodbc's `ByteaAsLongVarBinary=0` reaches the same block cursor from the other
end — it makes `bytea` a `SQL_VARBINARY(255)` — and was measured at 0.5425 s
against 0.5304 s for the reader fix on the seven-column query, i.e. no better,
while it also moves `SQLGetTypeInfo`'s name for `SQL_LONGVARBINARY` from `bytea`
to `lo` and so breaks the DDL bulk ingest generates. It is not set.

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

## Files

- `bench/fetch_bench.py` — the benchmark harness (all three paths, batch-size
  sweep, per-column attribution, unbound-column cliff, cProfile).
- `bench/odbc_floor.c` — raw `SQLBindCol`/`SQLFetch` floor, built on demand by
  the harness with `cc -O2 ... -lodbc`.
- `bench/ingest_bench.py`, `bench/matrix_bench.py` — the write path.
- `bench/verify_array_binding.py` — differential check that array binding and the
  row-at-a-time fallback produce identical data and identical rows-affected.
- `tests/c/test_multirow.c`, `test_multirow_ingest` in `tests/test_sqlite.py` — the
  multi-row `INSERT` text and its behaviour (NULLs in every row-group position, a batch
  that is not a multiple of K, a single-row batch, a failure that must leave no rows,
  and the parameter-ceiling search).
