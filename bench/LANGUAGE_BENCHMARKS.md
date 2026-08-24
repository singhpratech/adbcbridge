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
| python | postgres | 278,075 | 1,892,520 | — | — |
| rust | postgres | 482,822 | 2,078,465 | 77,045 | 2,091,423 |
| csharp | postgres | 401,694 | 1,937,816 | 18,926 | 966,055 |
| java | postgres | 450,072 | 1,617,704 | 222,359 | 3,261,189 |
| go | postgres | 395,941 | 1,987,447 | 13,199 | 399,315 |
| python | timescaledb | 325,634 | 1,830,410 | — | — |
| rust | timescaledb | 362,032 | 1,913,228 | 26,462 | 1,966,551 |
| csharp | timescaledb | 445,510 | 1,586,136 | 5,260 | 825,467 |
| java | timescaledb | 430,060 | 1,545,053 | — | — |
| go | timescaledb | 453,823 | 1,746,570 | 11,634 | 434,616 |
| python | azuresqledge | 114,282 | 2,067,218 | — | — |
| rust | azuresqledge | 130,239 | 2,407,563 | 68,266 | 2,885,924 |
| csharp | azuresqledge | — | — | — | — |
| java | azuresqledge | 129,600 | 1,902,476 | — | — |
| go | azuresqledge | 126,103 | 2,289,731 | 14,040 | 628,402 |
| python | oracle | 26,756 | 66,025 | — | — |
| rust | oracle | 30,547 | 103,686 | 1,853 | 136,334 |
| csharp | oracle | 29,474 | 122,490 | 361 | — |
| java | oracle | 25,448 | 115,517 | — | — |
| go | oracle | 20,507 | 84,970 | 369 | 86,725 |
| python | db2 | 37,880 | 1,636,045 | — | — |
| rust | db2 | 35,952 | 1,610,567 | 209,563 | 6,527,260 |
| csharp | db2 | 37,095 | — | 36,280 | — |
| java | db2 | 39,158 | 1,436,101 | — | — |
| go | db2 | 39,263 | 1,389,903 | 37,120 | — |
| python | vertica | 147,765 | 822,347 | — | — |
| rust | vertica | 117,441 | 657,974 | 264,453 | 955,205 |
| csharp | vertica | 159,480 | 892,250 | 23,269 | — |
| java | vertica | 120,723 | 950,085 | — | — |
| go | vertica | 125,288 | 868,684 | 15,345 | 348,314 |
| python | dolt | 78,252 | 1,292,970 | — | — |
| rust | dolt | 60,071 | 1,400,010 | 3,140 | 1,465,804 |
| csharp | dolt | 59,479 | 1,324,561 | 2,884 | 865,254 |
| java | dolt | 74,784 | 1,314,240 | — | — |
| go | dolt | 90,180 | 1,317,608 | 6,252 | 326,524 |
| python | matrixone | 69,189 | 1,785,885 | — | — |
| rust | matrixone | 124,948 | 1,835,853 | 4,615 | 2,074,257 |
| csharp | matrixone | 161,802 | 1,889,677 | 3,343 | 1,056,299 |
| java | matrixone | 178,350 | 1,681,416 | — | — |
| go | matrixone | 142,330 | 1,817,333 | — | — |
| python | greptimedb | 109,302 | 909,235 | — | — |
| rust | greptimedb | — | — | — | — |
| csharp | greptimedb | — | — | — | — |
| java | greptimedb | — | — | — | — |
| go | greptimedb | — | — | — | — |
| python | questdb | 131,909 | 1,390,435 | — | — |
| rust | questdb | — | — | — | — |
| csharp | questdb | — | — | — | — |
| java | questdb | — | — | — | — |
| go | questdb | — | — | — | — |
| python | materialize | 24,021 | 174,074 | — | — |
| rust | materialize | — | — | — | — |
| csharp | materialize | — | — | — | — |
| java | materialize | — | — | — | — |
| go | materialize | — | — | — | — |
| python | firebird | 6,380 | 298,613 | — | — |
| rust | firebird | — | — | — | — |
| csharp | firebird | — | — | — | — |
| java | firebird | — | — | — | — |
| go | firebird | — | — | — | — |
| python | arcadedb | — | 397,502 | — | — |
| rust | arcadedb | — | — | — | — |
| csharp | arcadedb | — | — | — | — |
| java | arcadedb | — | — | — | — |
| go | arcadedb | — | — | — | — |
| python | opensearch | — | 120,772 | — | — |
| rust | opensearch | — | — | — | — |
| csharp | opensearch | — | — | — | — |
| java | opensearch | — | — | — | — |
| go | opensearch | — | — | — | — |
| python | tdengine | — | 739,772 | — | — |
| rust | tdengine | — | — | — | — |
| csharp | tdengine | — | — | — | — |
| java | tdengine | — | — | — | — |
| go | tdengine | — | — | — | — |
| python | duckdb | 305,140 | — | — | — |
| rust | duckdb | 307,568 | — | — | — |
| csharp | duckdb | 301,677 | — | — | — |
| java | duckdb | 277,168 | — | — | — |
| go | duckdb | 331,034 | — | — | — |
| python | columnstore | 58,205 | 1,269,570 | — | — |
| rust | columnstore | 54,005 | 1,098,912 | 471,175 | 1,412,067 |
| csharp | columnstore | 57,592 | 1,103,138 | 9,985 | 510,386 |
| java | columnstore | 49,136 | 1,168,204 | — | — |
| go | columnstore | 56,189 | 1,212,993 | — | — |
| python | mssql | 166,036 | 2,295,405 | — | — |
| rust | mssql | 165,692 | 2,400,560 | 143,400 | 2,986,862 |
| csharp | mssql | 148,956 | 2,207,242 | 7,130 | 1,338,724 |
| java | mssql | 161,599 | 2,074,965 | — | — |
| go | mssql | 180,778 | 2,218,070 | — | — |
| python | tidb | 87,520 | 1,452,344 | — | — |
| rust | tidb | 69,693 | 1,418,597 | 4,549 | 1,570,622 |
| csharp | tidb | 87,614 | 1,473,099 | 4,090 | 936,230 |
| java | tidb | 66,598 | 1,362,064 | — | — |
| go | tidb | 53,105 | 1,462,807 | 4,317 | 506,776 |
| python | percona | 129,634 | 1,475,921 | — | — |
| rust | percona | 114,478 | 1,497,547 | 8,953 | 1,743,255 |
| csharp | percona | 86,247 | 1,479,073 | 7,723 | 931,114 |
| java | percona | 88,903 | 1,355,899 | — | — |
| go | percona | 67,979 | 1,483,600 | — | — |
| python | oceanbase | 66,421 | 1,331,081 | — | — |
| rust | oceanbase | 96,455 | 1,332,754 | 8,837 | 1,501,355 |
| csharp | oceanbase | 79,363 | 1,317,632 | 7,714 | 908,556 |
| java | oceanbase | 106,844 | 1,177,530 | — | — |
| go | oceanbase | 76,641 | 1,336,221 | 9,694 | 320,819 |
| python | mongodbbi | — | 144,477 | — | — |
| rust | mongodbbi | — | — | — | — |
| csharp | mongodbbi | — | — | — | — |
| java | mongodbbi | — | — | — | — |
| go | mongodbbi | — | — | — | — |
| python | monetdb | 130,315 | 1,325,973 | — | — |
| rust | monetdb | 110,307 | 1,418,697 | — | — |
| csharp | monetdb | 168,401 | 1,229,021 | 9,094 | 986,876 |
| java | monetdb | 155,632 | 1,181,124 | — | — |
| go | monetdb | 181,147 | 419,844 | — | — |
| python | yugabyte | 23,548 | 1,462,800 | — | — |
| rust | yugabyte | 28,419 | 1,583,608 | 5,730 | 1,553,695 |
| csharp | yugabyte | 37,763 | 1,538,978 | 2,446 | 803,392 |
| java | yugabyte | 23,528 | 1,264,506 | — | — |
| go | yugabyte | 23,498 | 1,634,803 | — | — |
| python | cloudberry | 9,776 | 1,822,285 | — | — |
| rust | cloudberry | 8,828 | 1,957,488 | 977 | 1,935,721 |
| csharp | cloudberry | 8,184 | 1,681,786 | 650 | 936,947 |
| java | cloudberry | 8,717 | 1,672,687 | — | — |
| go | cloudberry | 9,037 | 1,831,128 | 667 | 368,333 |
| python | cratedb | 40,251 | 682,843 | — | — |
| rust | cratedb | — | — | — | — |
| csharp | cratedb | — | — | — | — |
| java | cratedb | — | — | — | — |
| go | cratedb | — | — | — | — |
| python | spanner | 7,870 | 205,396 | — | — |
| rust | spanner | — | — | — | — |
| csharp | spanner | — | — | — | — |
| java | spanner | — | — | — | — |
| go | spanner | — | — | — | — |
| python | flightsql | — | 1,247,578 | — | — |
| rust | flightsql | — | — | — | — |
| csharp | flightsql | — | — | — | — |
| java | flightsql | — | — | — | — |
| go | flightsql | — | — | — | — |
| python | ignite | — | 934,431 | — | — |
| rust | ignite | — | — | — | — |
| csharp | ignite | — | — | — | — |
| java | ignite | — | — | — | — |
| go | ignite | — | — | — | — |
| python | dremio | — | 1,230,968 | — | — |
| rust | dremio | — | — | — | — |
| csharp | dremio | — | — | — | — |
| java | dremio | — | — | — | — |
| go | dremio | — | — | — | — |

The sqlite rows are the ones this file was first published with. The postgres
rows were re-measured with the databases below and **supersede** the ones
recorded before the write-path rework — that run read python 82,514, rust
74,684, csharp 81,223, java 72,331 and go 75,641 rows/s of ADBC ingest, and
every language is now 3.4–6.5× that against the same server. The fetch column
moved far less — between −1% (csharp) and +37% (java) — which is about the size
of this host's run-to-run noise.

## Why a cell is empty

A `—` in a *native* column is a comparison this language has no client for here:
the Java benchmark needs a `<DB>_JDBC` URL and the pom only carries the SQLite
and PostgreSQL drivers, so every other database reads `—` for JDBC.

A `—` in an *ADBC* column is a step that did not finish. Each one below was run
to the same 10,000-row ingest / 100,000-row fetch workload as the rest of the
table, with `adbc.odbc.delegate=never`, and the reason recorded rather than the
workload changed. The python row exists in several of these because
`bench/matrix_bench.py` connects with **autocommit on**, where the other four
benchmarks turn autocommit off and commit once — which is the workload, and
which three of these servers cannot do.

| Database | What fails, and whose fault it is |
|---|---|
| **questdb** | *Server.* With autocommit off QuestDB accepts the ingest — `adbc_ingest` returns 10,000 — but after `COMMIT` the table holds 0 rows, so every benchmark fails its row-count check (`wrong row count 0 != 10000`). The same ingest with autocommit on stores all 10,000. Go additionally dies with SIGSEGV inside psqlodbc's `SQLFreeHandle` while closing the prepared `INSERT` of the `database/sql` comparison, and C#'s comparison hits `ERROR: duplicate statement [name=_PLAN0x…]` — psqlodbc's server-side plan names colliding against QuestDB. |
| **materialize** | *Server.* Materialize runs one statement per transaction: the `CREATE TABLE` of a create-mode ingest fails with `[25000] this transaction can only execute a single statement`, and everything after it with `[25P02] current transaction is aborted`. |
| **firebird** | *Server.* Firebird does not make a table created inside an open transaction visible to a later statement in that same transaction, so the ingest fails at its first `INSERT` with `[42S02] (-204) Dynamic SQL Error … Table unknown`. `matrix_bench.py` documents this and runs Firebird with autocommit on. |
| **greptimedb** | *Server, surfaced by the ODBC driver.* GreptimeDB has no transactions (`SQL_TC_NONE`), so MySQL Connector/ODBC rejects `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `NotImplemented` and the connection never opens. Every step of all four benchmarks fails at connect. |
| **opensearch** | *Driver.* The OpenSearch SQL ODBC driver is read-only and refuses `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)` too, so the four autocommit-off benchmarks cannot connect. There is no `CREATE TABLE`/`INSERT` in the SQL plugin either, so no ingest exists to time. |
| **arcadedb**, **tdengine** | *Benchmark harness, and the entry.* Both entries are `read_only` — ArcadeDB has no `CREATE TABLE` (its DDL is `CREATE DOCUMENT TYPE` plus one `CREATE PROPERTY` per column) and every TDengine table must start with a `TIMESTAMP` primary key, which no generated DDL emits — so there is no create-mode ingest for any language. On top of that `bench/rust/conn.py` exports the entry's `setup` through the environment, and for these two `setup` *is* the literal bulk load (100,000 rows / 20,000 rows), so the benchmark binaries cannot even be exec'd: `Argument list too long` (E2BIG). |
| **oracle** | *ODBC driver.* Any row-array (block) fetch of a character column segfaults inside Oracle's own `libsqora.so.23.1`, in `bcoReturnColData` ← `bcoCacheFetchNext` ← `SQLFetch`, and takes the process with it — from every language and from plain `adbc_driver_manager`, and `tests/compat/test_matrix.py oracle` segfaults identically. It is the *second* `SQLFetch` that dies: 1,000 rows read fine, 2,000 do not, and `adbc.odbc.batch_size=1` reads all 20,000 rows of the same column. The numeric and `DATE` columns read fine at any size. The ingest column above is the full 10,000-row workload; only the fetch step was shortened, so the process survived to print it. |
| **azuresqledge** (csharp) | *Undetermined, reproducible.* `bench_cs` aborts with glibc's `corrupted double-linked list (not small)` before printing anything, on every one of six full-workload runs. It needs both halves of the workload: `--rows 10000 --fetch-rows 1000` and `--rows 100 --fetch-rows 100000` both finish, `--rows 10000 --fetch-rows 100000` crashes even at `--reps 1`. Rust, Java and Go run the same workload against the same msodbcsql18 and the same server without trouble. |
| **db2** (csharp fetch) | *Binding.* `[ODBC] SQLGetData failed` on all 4 runs of the 100,000-row read, while python, rust, go and java read the same 100,000 rows through the same driver and server. Size-dependent: 5,000 and 50,000 rows read fine from C#, 100,000 fails even at `--reps 1`. The `System.Data.Odbc` comparison fails separately with `Arithmetic operation resulted in an overflow` — the clidriver's 32-bit `SQLLEN` reaching a client that assumes 64-bit. |
| **db2** (go/csharp native fetch) | *Driver/client.* `alexbrainman/odbc` reports `SQLGetDiagRec failed: ret=-2`; both are the 32-bit `SQLLEN` again. The `odbc-api` numbers in the rust row are for the same reason not to be read as real throughput. |
| **matrixone** (go native) | *Server.* MatrixOne rejects the quoted-identifier `INSERT`/`SELECT` that `database/sql` prepares with `SQL parser error`. |
| **vertica** (csharp native fetch) | *Client.* `System.Data.Odbc` raises `Unable to cast object of type 'System.Int64' to type 'System.Int32'` — Vertica has one 64-bit integer type and the comparison reads it as `int`. |

The C# ADBC *fetch* is the one step in this table that is not reliable. Besides
the two rows above it failed intermittently with a bare `[ODBC] SQLFetch failed`
on postgres (2 runs in 4) and once on timescaledb, both of which then read the
same 100,000 rows on a re-run — and it is the only binding here that does so.
Its two hard failures (Db2's `SQLGetData`, Azure SQL Edge's heap corruption) are
both at 100,000 rows and both go away below ~50,000, which is the same shape.

For **arcadedb**, **opensearch** and **tdengine** the python *ADBC fetch* is not
the table above but the entry's pre-loaded `adbc_big` read end to end (100,000
documents for the first two, 20,000 rows for TDengine), which is what
`matrix_bench.py` times on a `read_only` entry. It is comparable with itself
across runs, not with the other rows.

These numbers were taken on a host shared with two other benchmark agents, load
average 2.5–7.9 and swap exhausted. The ingest column moves ±25–60% run to run
on the fast servers (postgres ADBC ingest measured 483k and 603k for rust, 402k
and 728k for csharp in separate runs); the fetch column is stable to a few
percent. Read them as ratios.
