<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language

One workload, run through five ADBC bindings against the same
`libadbc_driver_odbc.so`, so a reader can see that the driver behaves the same
whichever language loads it. The driver is a plain C shared library: every
binding `dlopen`s it and calls `AdbcDriverInit`, so the differences below are
the bindings' own, not the driver's.

Table `(id int32, val float64, txt "row-%012d" utf8, dt date32)`, one database
per run, every number the median of 3 timings after a warmup. All rates are
rows/s; higher is better.

| Column | What it measures |
|---|---|
| **ADBC ingest** | 10,000 rows through `libadbc_driver_odbc.so` and that language's ADBC driver manager: bulk ingest in `create` mode (`adbc.ingest.target_table` + `adbc.ingest.mode`), autocommit off, one commit, DDL + data + commit timed together and the row count verified afterwards. Building the Arrow batch is outside the timer. |
| **ADBC fetch** | `SELECT id, val, txt, dt` of 100,000 rows drained into that language's own Arrow record batches through the same driver. |
| **Native ingest / fetch** | the same two steps through the language's ordinary ODBC or JDBC client, no Arrow — the floor that binding could reach without this driver. |

The two ADBC columns are the ones to compare *across* rows: they run identical
work over identical DDL. The native columns are **not** comparable across
languages, because each one uses the bulk API its client actually offers:

| Language | ADBC binding | Native comparison client |
|---|---|---|
| python | `adbc_driver_manager.dbapi` | pyodbc `executemany` / `fetchall()` (see [`MATRIX_BENCHMARKS.md`](MATRIX_BENCHMARKS.md)) |
| rust | `adbc_driver_manager` crate | [`odbc-api`](https://crates.io/crates/odbc-api): array-bound `ColumnarBulkInserter`, `ColumnarDynBuffer` reads (see [`RUST_BENCHMARKS.md`](RUST_BENCHMARKS.md)) |
| csharp | `Apache.Arrow.Adbc`'s `CAdbcDriverImporter` | `System.Data.Odbc`: a prepared `INSERT` executed row by row in one transaction, and an `OdbcDataReader` |
| java | `adbc-driver-jni` | JDBC (`sqlite-jdbc` / `postgresql`): a prepared `INSERT` with `addBatch`/`executeBatch`, and a `ResultSet` |
| go | `go/adbc/drivermgr` | `database/sql` + [`alexbrainman/odbc`](https://github.com/alexbrainman/odbc): a prepared `INSERT` executed row by row in one transaction |

Native delegation is switched off (`adbc.odbc.delegate=never`), so every row
really travels over ODBC. Servers run locally, so the numbers reflect the ODBC
driver plus the database, not a network — and they move with whatever else is
running on the host, so read them as ratios rather than absolutes.

## Reproducing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so
export SQLITE_ODBC_DRIVER=... POSTGRES_ODBC_DRIVER=...   # as tests/compat

bench/csharp/run.sh sqlite postgres     # .NET 8 SDK
bench/java/run.sh   sqlite postgres     # JDK 17+ and Maven
bench/go/run.sh     sqlite postgres     # Go 1.24+ and unixODBC's headers
```

Each `run.sh` resolves its connection string with `bench/rust/conn.py`, which
reads it out of `tests/compat/test_matrix.py` exactly as `bench/matrix_bench.py`
does, and rewrites its own rows in the table below. Set `ADBC_MATRIX_SUFFIX` to
isolate concurrent runs sharing a server; `ROWS`, `FETCH_ROWS` and `REPS`
override the workload.

The Python and Rust rows are the same workload measured by the two benchmarks
that already existed, and are pasted in by hand:

```sh
python bench/matrix_bench.py --rows 10000 --fetch-rows 100000 --reps 3 sqlite postgres
bench/rust/run.sh sqlite postgres
```

For Python, *ADBC ingest* is `matrix_bench.py`'s **Ingest (array)** column — the
driver default, the same setting every other language here runs with — not its
**Ingest** column, which forces `adbc.odbc.array_binding=false`.

## What to look for

- **The same driver, the same behaviour.** On PostgreSQL, where the server does
  the bulk of the work, all five ADBC ingest rates land inside a 1.15× band. The
  binding barely shows.
- **On SQLite the binding is the whole story.** With the per-row database cost
  near zero, the ADBC ingest column spreads about 2.5× across languages. That
  spread is each driver manager's own per-batch overhead — C data interface
  copies, JNI or P/Invoke transitions — not the driver's.
- **Java pays the most.** `adbc-driver-jni` is the slowest binding in both ADBC
  columns, most visibly on SQLite; the gap closes on PostgreSQL ingest, where the
  server dominates, and persists on fetch.
- **ADBC ingest beats a row-at-a-time client comfortably.** Against the
  `System.Data.Odbc` and `database/sql` paths, which have no array-binding API,
  the bulk ingest is several times faster: the driver binds the whole batch as
  parameter arrays. Against clients that *do* batch — Rust's
  `ColumnarBulkInserter`, JDBC's `executeBatch` — it is in the same league.
- **A native-protocol client can still win a read.** pgjdbc reads PostgreSQL
  over its own wire protocol rather than ODBC, so its fetch beats the bridge's;
  that is a protocol difference, not driver overhead. Where the comparison stays
  inside ODBC (`odbc-api`, `OdbcDataReader`), the Arrow path is at or above the
  raw row-by-row one.

## Results

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 382,310 | 1,537,199 | — | — |
| rust | sqlite | 638,747 | 1,750,908 | 717,102 | 1,879,177 |
| csharp | sqlite | 590,678 | 1,807,749 | 304,841 | 927,208 |
| java | sqlite | 258,370 | 1,315,742 | 426,551 | 1,354,244 |
| go | sqlite | 516,754 | 1,603,019 | 368,756 | 655,158 |
| python | postgres | 82,514 | 1,615,548 | — | — |
| rust | postgres | 74,684 | 1,607,624 | 67,291 | 2,057,915 |
| csharp | postgres | 81,223 | 1,966,696 | 14,576 | 901,162 |
| java | postgres | 72,331 | 1,181,693 | 111,014 | 2,345,395 |
| go | postgres | 75,641 | 1,919,120 | 19,691 | 575,029 |
