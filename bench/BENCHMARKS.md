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

The remaining floor is the driver's own per-statement cost. clickhouse-odbc is
the outlier: it speaks HTTP, has no transactions to batch and no usable parameter
arrays, so its ingest rate stays at the driver's request-per-row rate.

## Files

- `bench/fetch_bench.py` — the benchmark harness (all three paths, batch-size
  sweep, per-column attribution, unbound-column cliff, cProfile).
- `bench/odbc_floor.c` — raw `SQLBindCol`/`SQLFetch` floor, built on demand by
  the harness with `cc -O2 ... -lodbc`.
- `bench/ingest_bench.py`, `bench/matrix_bench.py` — the write path.
- `bench/verify_array_binding.py` — differential check that array binding and the
  row-at-a-time fallback produce identical data and identical rows-affected.
