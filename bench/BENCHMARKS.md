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
entire result set.** *Measured: 2.7x.* `ReaderBind()` does:

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

**3. Bound the *rowset* in bytes, not just each value.** `max_bind_bytes` caps
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

**4. Reserve Arrow buffers up front.** *Estimated: 5-15% of our 30 ms, ~1% end-to-end
here.* After `ArrowArrayStartAppending`, call `ArrowArrayReserve(&batch,
rows_per_fetch)` — we know exactly how many rows the batch will hold. For bound
`FETCH_CHAR`/`FETCH_BINARY` columns we also know the maximum byte width
(`elem_size * rows_per_fetch`), so the data buffers can be reserved too. Removes
the realloc/memcpy chain on every batch. Cheap, local, no behaviour change.

**5. Bulk-append fixed-width columns instead of one call per value.**
*Estimated: most of the remaining ~30 ms, ~5% end-to-end here.* For
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

## Files

- `bench/fetch_bench.py` — the benchmark harness (all three paths, batch-size
  sweep, per-column attribution, unbound-column cliff, cProfile).
- `bench/odbc_floor.c` — raw `SQLBindCol`/`SQLFetch` floor, built on demand by
  the harness with `cc -O2 ... -lodbc`.
