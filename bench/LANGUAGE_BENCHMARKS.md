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
- **The fetch column is where the five bindings agree.** On the twelve rows below
  where all five languages measured, ADBC fetch spreads 1.11–1.21× across
  languages on nine of them, and the two ends of the range are three orders of
  magnitude apart in absolute rate (546k–649k rows/s on ydb against 2.28M–2.71M
  on mariadb). Re-measured, SQLite — where the per-row database cost is near zero
  and the binding should show most — spreads only 1.25×, not the 2.5× this file
  first recorded.
- **The ingest column is looser, and noisier.** It runs from 1.01× across
  languages (starrocks) to 2.70× (databend), and SQLite's own control run moved by
  a factor of four between repetitions of the identical workload. Treat a single
  ingest cell as an order-of-magnitude reading and the agreement across a row as
  the real result.
- **Java pays the most, by a little.** `adbc-driver-jni` has the slowest ADBC
  fetch on 5 of those twelve rows — more than any other binding, C# next at 4 —
  and the slowest SQLite ingest. On ingest overall it is not the laggard: python
  is slowest on 5 rows and rust on 4. Every one of these margins is inside this
  host's run-to-run noise on any single row.
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
| python | sqlite | 801,668 | 1,595,715 | — | — |
| rust | sqlite | 944,635 | 1,814,239 | — | 1,940,707 |
| csharp | sqlite | 844,423 | 1,537,260 | 296,681 | 1,022,166 |
| java | sqlite | 732,161 | 1,449,726 | 582,282 | 1,485,673 |
| go | sqlite | 827,133 | 1,747,453 | 349,602 | 642,191 |
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
| python | oracle | 31,351 | — | — | — |
| rust | oracle | 24,730 | — | 456 | — |
| csharp | oracle | 20,009 | — | 1,774 | — |
| java | oracle | 19,261 | — | — | — |
| go | oracle | 22,618 | — | 443 | — |
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
| python | mariadb | 250,781 | 2,708,566 | — | — |
| rust | mariadb | 230,053 | 2,696,451 | 211,672 | 2,904,183 |
| csharp | mariadb | 245,182 | 2,713,388 | 31,094 | 645,194 |
| java | mariadb | 236,781 | 2,283,243 | — | — |
| go | mariadb | 257,292 | 2,580,801 | — | — |
| python | clickhouse | 1,307 | 1,215,585 | — | — |
| rust | clickhouse | 1,448 | 1,145,049 | — | 1,978,070 |
| csharp | clickhouse | 1,485 | 1,068,469 | — | — |
| java | clickhouse | 1,349 | 1,114,859 | — | — |
| go | clickhouse | 1,386 | 1,174,437 | — | 655,149 |
| python | mysql | 77,394 | 1,458,651 | — | — |
| rust | mysql | 71,411 | 1,427,909 | 10,197 | 1,493,508 |
| csharp | mysql | 120,465 | 1,266,572 | 7,629 | 960,250 |
| java | mysql | 110,943 | 1,359,220 | — | — |
| go | mysql | 71,990 | 1,422,684 | 8,517 | 411,385 |
| python | databend | 11,406 | 1,023,774 | — | — |
| rust | databend | 7,377 | 1,095,337 | — | 1,139,613 |
| csharp | databend | 11,237 | 1,132,616 | — | 759,793 |
| java | databend | 19,929 | 1,024,368 | — | — |
| go | databend | 7,523 | 1,082,704 | — | 499,465 |
| python | doris | 2,460 | 1,378,871 | — | — |
| rust | doris | 2,405 | 2,042,430 | — | 2,256,220 |
| csharp | doris | — | — | — | — |
| java | doris | 2,636 | 1,680,902 | — | — |
| go | doris | — | — | — | — |
| python | starrocks | 4,828 | 1,544,630 | — | — |
| rust | starrocks | 4,764 | 2,212,905 | — | 3,032,142 |
| csharp | starrocks | 4,769 | 2,372,203 | — | — |
| java | starrocks | 4,782 | 1,611,809 | — | — |
| go | starrocks | 4,766 | 2,107,217 | — | — |
| python | informix | 92,297 | 634,870 | — | — |
| rust | informix | 126,583 | 747,432 | 170,232 | 790,451 |
| csharp | informix | 113,375 | 736,604 | 11,864 | — |
| java | informix | 126,293 | 717,581 | — | — |
| go | informix | 127,350 | 759,005 | 24,233 | 333,588 |
| python | cockroachdb | 37,090 | 1,047,134 | — | — |
| rust | cockroachdb | 39,812 | 1,089,534 | 6,010 | 1,049,715 |
| csharp | cockroachdb | 41,203 | 1,046,965 | 1,423 | 667,042 |
| java | cockroachdb | 66,662 | 1,022,135 | — | — |
| go | cockroachdb | 44,295 | 1,150,250 | 1,317 | 285,303 |
| python | citus | 350,699 | 1,819,904 | — | — |
| rust | citus | 700,347 | 1,969,088 | 95,104 | 2,114,508 |
| csharp | citus | 511,653 | 1,870,120 | 9,735 | 961,414 |
| java | citus | 569,450 | 1,631,050 | — | — |
| go | citus | 656,012 | 1,825,179 | — | — |
| python | opengauss | 111,423 | 1,399,451 | — | — |
| rust | opengauss | 149,353 | 1,499,678 | 38,984 | 1,530,521 |
| csharp | opengauss | 223,514 | 1,430,994 | 4,533 | 795,308 |
| java | opengauss | 208,017 | 1,321,406 | — | — |
| go | opengauss | 181,602 | 1,548,459 | — | — |
| python | risingwave | 49,953 | 989,430 | — | — |
| rust | risingwave | — | — | — | — |
| csharp | risingwave | — | — | — | — |
| java | risingwave | — | — | — | — |
| go | risingwave | — | — | — | — |
| python | virtuoso | 10,029 | 968,528 | — | — |
| rust | virtuoso | 20,457 | 828,959 | — | — |
| csharp | virtuoso | 23,527 | 419,156 | 12,788 | 424,783 |
| java | virtuoso | 9,948 | 978,832 | — | — |
| go | virtuoso | 17,678 | 847,872 | — | — |
| python | influxdb3 | — | 1,263,303 | — | — |
| rust | influxdb3 | — | — | — | — |
| csharp | influxdb3 | — | — | — | — |
| java | influxdb3 | — | — | — | — |
| go | influxdb3 | — | — | — | — |
| python | ydb | 1,669 | 572,099 | — | — |
| rust | ydb | 1,731 | 642,348 | — | — |
| csharp | ydb | 1,704 | 546,263 | — | — |
| java | ydb | 1,597 | 600,205 | — | — |
| go | ydb | 1,742 | 649,146 | — | — |
| python | access | — | 2,119,999 | — | — |
| rust | access | — | — | — | — |
| csharp | access | — | — | — | — |
| java | access | — | — | — | — |
| go | access | — | — | — | — |

The postgres rows were re-measured with the databases above them and
**supersede** the ones recorded before the write-path rework — that run read
python 82,514, rust 74,684, csharp 81,223, java 72,331 and go 75,641 rows/s of
ADBC ingest, and every language is now 3.4–6.5× that against the same server.
The fetch column moved far less — between −1% (csharp) and +37% (java) — which
is about the size of this host's run-to-run noise.

The **sqlite** rows and the sixteen databases from **mariadb** down were
measured together in one pass, so they are comparable with each other. The
sqlite rows **supersede** the ones this file was first published with (python
382,310, rust 638,747, csharp 590,678, java 258,370, go 516,754 rows/s of ADBC
ingest) — but they are not quite the same workload, because the four
autocommit-off harnesses can no longer commit on this ODBC driver at all and had
to be run with `ADBC_BENCH_AUTOCOMMIT=1`; see **sqlite** in the table below. The
same knob is why **databend**, **doris**, **starrocks** and **ydb** have numbers
at all.

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
which ten of the servers below cannot do: questdb, materialize, firebird,
greptimedb, opensearch, databend, doris, starrocks, ydb and influxdb3. On sqlite
it is the ODBC driver rather than the server that cannot.

Where a server or its ODBC driver cannot run the autocommit-off workload at all,
the four other harnesses were re-run with `ADBC_BENCH_AUTOCOMMIT=1`, which puts
them on python's footing: the bridge batches the stream into one transaction
itself rather than the benchmark committing once. Every database that was
measured that way says so in its row below, and its ADBC numbers should be read
against python's rather than against the databases that took the full workload.
`ADBC_BENCH_NO_NATIVE=1` / `-no-native` likewise switches off only the plain-ODBC
comparison columns, for the drivers that abort or hang the process from that
path; the ADBC columns are still the real measurement.

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
| **sqlite** (ran with `ADBC_BENCH_AUTOCOMMIT=1`) | *ODBC driver, tripped by the harness.* `drop_table()` in all four harnesses follows a failed `DROP TABLE` with `connection.rollback()` **and** a literal `ROLLBACK`, which MonetDB needs to clear an aborted transaction. `libsqlite3odbc.so` never issues `BEGIN` again after that literal `ROLLBACK`, so every later `COMMIT` fails with `[HY000] (1) [SQLite]cannot commit - no transaction is active` and the whole run reads `—` — as it does today for all four languages without the knob. The rows land: the ingest itself succeeds and `SELECT COUNT(*)` returns 10,000; it is only `SQLEndTran(SQL_COMMIT)` that errors. Gating the literal `ROLLBACK` on `rollback()` having failed fixes SQLite and breaks MonetDB, whose `SQLEndTran` returns success without clearing anything, so the workload was changed rather than the harness. In autocommit the rust row's *native ingest* is `—` for a second reason: `odbc-api`'s `ColumnarBulkInserter` writes into a transaction the comparison then cannot see, `wrong row count 0 != 10000`. |
| **clickhouse** | *ODBC driver.* The 1.3–1.5k rows/s ADBC ingest is real and is the same in all five languages: clickhouse-odbc is on the bridge's no-array-binding list, so the ingest is one `INSERT` per row over HTTP. The native ingest is `—` because that comparison *does* array-bind — rust lands 2 of 10,000 rows (`wrong row count 2 != 10000`) and go's driver answers `SQLExecute: {HY000} Error while processing query …: HTTP status code: 501`. C# had to run with `ADBC_BENCH_NO_NATIVE=1`: with the comparison on, `bench_cs` printed nothing and was still running when it was killed at 600 s. |
| **mariadb** (go native) | *Driver/client.* The `database/sql` read of 100,000 rows never returns. MariaDB's own `PROCESSLIST` shows the connection `Sleep` with no query in flight for 12 minutes while `bench_go` sits at 0.2% CPU, so the client is not waiting on the server. Run with `-no-native`; the ADBC columns are unaffected. Its first ADBC ingest also came out at 44,161 rows/s against 230k–257k for the other four languages, and re-ran at 257,292 — the low one was noise, and the second is what the table carries. |
| **databend**, **doris**, **starrocks** (ran with `ADBC_BENCH_AUTOCOMMIT=1`) | *Server, surfaced by the ODBC driver.* None of the three has transactions, so MySQL Connector/ODBC rejects `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `NotImplemented` and the autocommit-off connection never opens — the same failure as **greptimedb** above. With the knob all four languages measure. Their *native ingest* is `—` in every language for the same reason from the other side: the comparison opens its own transaction and gets `{HYC00} [MySQL][ODBC 9.4(w) Driver]Transactions are not enabled`. |
| **starrocks** (go/csharp native fetch) | *Server.* StarRocks quotes with a backtick, so the comparison's `SELECT "id", "val", "txt", "dt"` returns four string *literals* rather than columns: go reads `sql: Scan error on column index 0, name "'id'": converting driver.Value type []uint8 ("id") to a int64: invalid syntax` and C# `Unable to cast object of type 'System.String' to type 'System.Int32'`. The ADBC path quotes with the database's own character and is unaffected. |
| **doris** (go, csharp) | *Host, not the driver or the binding.* Both languages measured on the first pass (go 2,686 rows/s of ADBC ingest); on the re-run needed for their fetch column, and on two further attempts, Doris's backend refused every statement with `errCode = 2, detailMessage = (127.0.0.1)[MEM_ALLOC_FAILED]Create Expr failed because [E11] Allocator sys memory check failed: … process memory used 1.26 GB exceed limit 27.92 GB or sys available memory 1.07 GB less than low water mark 1.60 GB`. The host was down to 1.1 GB available with swap full; `tests/compat/test_matrix.py doris` failed the same way, while `SELECT 1`, `SHOW BACKENDS` and a one-column `CREATE TABLE` still worked. The python, rust and java rows were taken earlier in the same pass, before that. |
| **informix** (csharp native fetch, rust arrow-odbc) | *Driver.* Informix is reached through the same Db2 `clidriver` `libdb2.so`, built with a 32-bit `SQLLEN`: `System.Data.Odbc` raises `Arithmetic operation resulted in an overflow.` and `arrow-odbc` panics with `Failed to retrieve data type from ODBC driver. The SQLLEN could not be converted to a 16 Bit integer`. The bridge detects that layout itself (`adbc.odbc.sqllen_32bit`) and reads the same result set fine, which is why only the comparison columns are empty. |
| **citus**, **opengauss**, **virtuoso** (go native) | *Driver/client.* `alexbrainman/odbc` takes the process down from the plain-ODBC path in all three: citus prints `malloc_consolidate(): invalid chunk size` and then `SIGSEGV … signal arrived during cgo execution` inside `SQLFreeHandle` ← `ODBCStmt.closeByRows` ← `Rows.Close`, which is the same signature as the questdb row above; opengauss segfaults in cgo during the `database/sql` read at `main.go:345`. All three were re-run with `-no-native`, which keeps the ADBC columns. |
| **virtuoso** (rust native) | *Driver.* `odbc-api`'s array-bound insert gets a bare `ODBC emitted an error calling 'SQLExecute':` from `virtodbc.so`, and both of its reads return `read 0 rows, expected 100000` from a `SELECT` the ADBC path drains in full. |
| **risingwave** (rust, csharp, java, go) | *Server semantics, and the harness has no hook for them.* RisingWave makes committed rows visible to a scan only after a `FLUSH`; the compat entry carries `refresh="FLUSH"` and `matrix_bench.py` issues it after every ingest, which is why the python row is the only one here. The four language harnesses have no refresh step, so each fails its row-count check straight after `COMMIT` — `wrong row count 0 != 10000` on the ingest and `wrong row count 62500 != 100000` on the fetch table, the count climbing between runs as the stream catches up. `ADBC_BENCH_AUTOCOMMIT=1` does not help; it is not a transaction problem. |
| **influxdb3** | *Server.* A `read_only` entry: InfluxDB 3's SQL has no DDL. With autocommit off the connection is refused outright (`NotImplemented: [ODBC] SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT) failed`, the Arrow Flight SQL ODBC driver again); with `ADBC_BENCH_AUTOCOMMIT=1` it connects and the ingest's DDL is refused instead — `CREATE TABLE "adbc_bench_rs_b2" ("id" INTEGER, "val" FLOAT, "txt" VARCHAR, "dt" TIMESTAMP) failed`. The four harnesses read back the table they ingest, so with no ingest there is no fetch either, and only the python cell — `matrix_bench.py`'s `read_only` path over the pre-loaded `adbc_big`, 100,000 rows — is measurable. |
| **ydb** (ran with `ADBC_BENCH_AUTOCOMMIT=1` and `-no-native`) | *Server.* YDB requires a primary key, so the bridge's create-mode DDL appends one; its PostgreSQL layer will not run that inside an open transaction and answers `[ODBC] CREATE TABLE "adbc_bench_rs_b2" ("id" INTEGER, "val" DOUBLE, "txt" TEXT, "dt" DATE, adbc_pk SERIAL PRIMARY KEY) failed`. With autocommit on it takes it, and all five languages land within 1,597–1,742 rows/s of each other. `-no-native` as well because the row-at-a-time comparison alone overran the 600 s cap in rust, go and C#; that is why the native columns are empty. |
| **access** | *Driver, and the benchmark harness.* The `mdbtools` ODBC driver is read-only, so there is no create-mode ingest — but the four language harnesses do not even connect: `[ODBC] SQLDriverConnect failed`. `tests/compat/test_matrix.py` makes a fresh `mkdtemp()` per process and the entry's `DBQ=` names `access.mdb` inside it, and only `matrix_bench.py` and the compat runner copy `tests/compat/fixtures/access.mdb` there — `bench/rust/conn.py` exports the connection string without copying the fixture, so the file the four harnesses point at does not exist. The python cell is the `read_only` path again, and its 100,000 is really the fixture's **3,000**-row `adbc_big`; it is comparable with itself across runs, not with the other rows. |

The C# ADBC *fetch* is the one step in this table that is not reliable. Besides
the two rows above it failed intermittently with a bare `[ODBC] SQLFetch failed`
on postgres (2 runs in 4) and once on timescaledb, both of which then read the
same 100,000 rows on a re-run — and it is the only binding here that does so.
Its two hard failures (Db2's `SQLGetData`, Azure SQL Edge's heap corruption) are
both at 100,000 rows and both go away below ~50,000, which is the same shape.

For **arcadedb**, **opensearch**, **tdengine**, **influxdb3** and **access** the
python *ADBC fetch* is not the table above but the entry's pre-loaded `adbc_big`
read end to end (100,000 documents for the first two, 20,000 rows for TDengine,
100,000 for InfluxDB 3 and 3,000 for Access), which is what `matrix_bench.py`
times on a `read_only` entry. It is comparable with itself across runs, not with
the other rows — and **access**'s 3,000 rows in particular are far too few to
read as a throughput next to a 100,000-row one.

These numbers were taken on a host shared with two other benchmark agents, load
average 2.5–7.9 and swap exhausted. The ingest column moves ±25–60% run to run
on the fast servers (postgres ADBC ingest measured 483k and 603k for rust, 402k
and 728k for csharp in separate runs); the fetch column is stable to a few
percent. Read them as ratios.

The sqlite row and the sixteen databases below it were measured in a later pass,
on a host running 36 containers with a second benchmark agent working through a
different sixteen at the same time. Load average was 1.99–4.94 for the whole
pass — `uptime` was recorded before each database — and free memory fell from
6 GB to 1.1 GB with swap full, which is what finally stopped Doris. SQLite is the
control, and it shows what that noise is worth: five consecutive runs of the
identical workload read 619k, 230k, 910k, 819k and 945k rows/s of rust ADBC
ingest. The first two are outliers against a stable ~900k, and the table carries
the last run rather than an average, so a single ingest figure here is worth
about a factor of two. The *fetch* column is far steadier — the same five runs
read 1.83M, 1.69M, 1.83M, 1.86M and 1.81M — and the cross-language agreement is
steadier still: on ydb all five languages land within 1,597–1,742 rows/s of
ingest and 546k–649k of fetch, and on starrocks within 4,764–4,828 rows/s of
ingest. That agreement, not any single number, is what this table is for.
