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
- **One server can cost the read an order of magnitude, and it shows in every
  language at once.** Oracle's fetch column is 66k–122k rows/s against 1.1M–2.4M
  everywhere else, because the string column is a `CLOB` there and this ODBC
  driver has to be read a row at a time (see below). The five bindings stay
  within 1.9× of each other while doing it — the ratio between them barely
  moves, which is the point of running the same workload from all five.
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
| rust | duckdb | 336,638 | — | — | — |
| csharp | duckdb | 316,437 | — | — | — |
| java | duckdb | 201,602 | — | — | — |
| go | duckdb | 309,410 | — | — | — |
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
which several of these servers cannot do.

| Database | What fails, and whose fault it is |
|---|---|
| **questdb** | *Server.* With autocommit off QuestDB accepts the ingest — `adbc_ingest` returns 10,000 — but after `COMMIT` the table holds 0 rows, so every benchmark fails its row-count check (`wrong row count 0 != 10000`). The same ingest with autocommit on stores all 10,000. Go additionally dies with SIGSEGV inside psqlodbc's `SQLFreeHandle` while closing the prepared `INSERT` of the `database/sql` comparison, and C#'s comparison hits `ERROR: duplicate statement [name=_PLAN0x…]` — psqlodbc's server-side plan names colliding against QuestDB. |
| **materialize** | *Server.* Materialize runs one statement per transaction: the `CREATE TABLE` of a create-mode ingest fails with `[25000] this transaction can only execute a single statement`, and everything after it with `[25P02] current transaction is aborted`. |
| **firebird** | *Server.* Firebird does not make a table created inside an open transaction visible to a later statement in that same transaction, so the ingest fails at its first `INSERT` with `[42S02] (-204) Dynamic SQL Error … Table unknown`. `matrix_bench.py` documents this and runs Firebird with autocommit on. |
| **greptimedb** | *Server, surfaced by the ODBC driver.* GreptimeDB has no transactions (`SQL_TC_NONE`), so MySQL Connector/ODBC rejects `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `NotImplemented` and the connection never opens. Every step of all four benchmarks fails at connect. |
| **opensearch** | *Driver.* The OpenSearch SQL ODBC driver is read-only and refuses `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)` too, so the four autocommit-off benchmarks cannot connect. There is no `CREATE TABLE`/`INSERT` in the SQL plugin either, so no ingest exists to time. |
| **arcadedb**, **tdengine** | *Benchmark harness, and the entry.* Both entries are `read_only` — ArcadeDB has no `CREATE TABLE` (its DDL is `CREATE DOCUMENT TYPE` plus one `CREATE PROPERTY` per column) and every TDengine table must start with a `TIMESTAMP` primary key, which no generated DDL emits — so there is no create-mode ingest for any language. On top of that `bench/rust/conn.py` exports the entry's `setup` through the environment, and for these two `setup` *is* the literal bulk load (100,000 rows / 20,000 rows), so the benchmark binaries cannot even be exec'd: `Argument list too long` (E2BIG). |
| **duckdb** (fetch, and every native column) | *The entry, plus the driver's model.* The compat entry connects with `Database=:memory:`, so **every ODBC connection is its own empty DuckDB**. The ingest step creates its table, fills it and checks the count inside one connection, so its number is real; the fetch step opens a fresh connection, where that table has never existed. All five bindings report the read as `[ODBC] SQLExecDirect failed`, and the driver's own text under it is `[42000] ODBC_DuckDB->PrepareStmt / Catalog Error: Table with name adbc_bench_rs does not exist!`. The native columns are `—` for exactly the same reason and not because the comparison was skipped: `System.Data.Odbc`, `database/sql` and `odbc-api` each carry the same `[42000] ODBC_DuckDB->PrepareStmt` out of their `SQLPrepare`/`SQLExecDirect`. |
| **cratedb** (rust, csharp, java, go) | *Server, meeting the generated DDL.* These four harnesses always build the batch with a `date32` column, and bulk-ingest DDL takes its type names from psqlodbc's `SQLGetTypeInfo` — PostgreSQL's. CrateDB has no DATE storage type, so `CREATE TABLE "adbc_bench_rs" ("id" int4, "val" float8, "txt" text, "dt" date)` is refused with ``[XX000] ERROR: Type `date` does not support storage``, and every step after it fails with the table missing. The python row exists because `matrix_bench.py` applies the compat entry's `ingest_types` remap (`date32 -> timestamp[us]`) before it builds the batch. |
| **spanner** (rust, csharp, java, go) | *Server, meeting the generated DDL.* The same shape one column over: Spanner has no 32-bit integer type and psqlodbc names `int4` for one, so `CREATE TABLE "adbc_bench_rs" ("id" int4, "val" float8, "txt" text, "dt" date, "adbc_ingest_key" bigint GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY)` fails with `[P0001] ERROR: Type <int4> is not supported; use bigint or int8 instead.` Only `matrix_bench.py` applies the entry's `ingest_types` (`int32 -> int64`), so only the python row has numbers. (The primary key in that DDL is the driver's own doing — Spanner requires one — and is not what fails.) |
| **ignite** | *Server.* Every Ignite table must have a primary key and a create-mode ingest generates none, so `CREATE TABLE adbc_bench_go (id INTEGER, val DOUBLE, txt VARCHAR, dt DATE)` is refused with `[42000] No PRIMARY KEY defined for CREATE TABLE.` — which is what the compat entry records as `ingest_create=False`. There is no ingest to time in any language, and with no table to read the four harnesses have no fetch either; the python fetch is the entry's pre-loaded `adbc_big`. |
| **mongodbbi** | *Server, surfaced by the ODBC driver.* mongosqld has no transactions, so MySQL Connector/ODBC refuses `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `[HYC00] (4000) [MySQL][ODBC 9.4(w) Driver]Transactions are not enabled.` and the connection never opens — every step of the four autocommit-off benchmarks fails at connect, exactly as on greptimedb. The entry is `read_only` besides: mongosqld is a query engine with no DDL and no DML, so there is no ingest to time from any language. |
| **flightsql**, **dremio** | *Driver.* Both are driven by the Arrow Flight SQL ODBC driver, which answers `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)` with `[HYC00] (100) [Apache Arrow][Flight SQL] (100) Optional feature not implemented.`, so the four autocommit-off benchmarks cannot open a connection at all. Both entries are `read_only` for a second reason from the same driver — it has no `SQLBindParameter` — so no ingest could reach either server anyway. The python fetch is the entry's pre-loaded `adbc_big`. |
| **monetdb** (all four, without `ADBC_BENCH_AUTOCOMMIT=1`) | *Driver.* MonetDBODBClib's `SQLEndTran` is a no-op. With autocommit off the ingest returns a rate (rust measured 189,937 rows/s) and its row-count check passes on the connection that wrote the rows, but the next connection finds nothing: `ERROR [42S02] [MonetDB][ODBC Driver 11.55.7]INSERT INTO: no such table 'adbc_bench_cs'`, `... SELECT: no such table 'adbc_bench_cs'`. The four monetdb rows above were therefore taken with `ADBC_BENCH_AUTOCOMMIT=1`, which is the autocommit-on setting `matrix_bench.py` — and so the python row — uses everywhere. |
| **monetdb** (rust native) | *Client.* `odbc-api` cannot open this DSN at all: `ODBC emitted an error calling 'SQLDriverConnect': State: IM005, Native error: 0, Message: [unixODBC][Driver Manager]Driver's SQLAllocHandle on SQL_HANDLE_DBC failed`. The bridge, pyodbc, `System.Data.Odbc` and `database/sql` all reach the same MonetDBODBClib through the same unixODBC. |
| **mssql**, **percona**, **columnstore**, **yugabyte**, **monetdb** (go native) | *Binding.* `bench_go` dies before it prints anything, so each of these was re-run with `-no-native`, which keeps both ADBC columns and leaves the `database/sql` ones empty. Two shapes, neither in the driver's path. On mssql, percona and monetdb it is arrow-go's own finalizer: `SIGSEGV … signal arrived during cgo execution` on the finalizer goroutine, in `cdata.initReader.func2 -> _Cfunc_ArrowArrayStreamRelease`, while the main goroutine sits in the `database/sql` step (`odbcFetch` for the first two, the closing cleanup connect for monetdb) — an `ArrowArrayStream` from an earlier ADBC read being finalised long after the step that produced it. On yugabyte it is psqlodbc: SIGSEGV under `alexbrainman/odbc`'s `(*Stmt).Close -> (*ODBCStmt).releaseHandle` while closing the prepared `INSERT`, the same crash the questdb row above records. On columnstore it is maodbc: `double free or corruption (!prev)`, SIGABRT, in `rows.Close` at the end of the `database/sql` read. |
| **oracle** (csharp native fetch) | *Client.* `System.Data.Odbc` raises `Unable to cast object of type 'System.Decimal' to type 'System.Int32'` — Oracle stores the `id` column as `NUMBER`, which comes back as a `decimal`, and the comparison reads it as `int`. The same shape as the vertica row below. Go hit the arrow-go finalizer crash on its first oracle run too (`free(): invalid pointer` in `ArrowArrayStreamRelease`); the repeat of the same command finished, and the go oracle row is that run, native columns included. |
| **azuresqledge** (csharp) | *Undetermined, reproducible.* `bench_cs` aborts with glibc's `corrupted double-linked list (not small)` before printing anything, on every one of six full-workload runs. It needs both halves of the workload: `--rows 10000 --fetch-rows 1000` and `--rows 100 --fetch-rows 100000` both finish, `--rows 10000 --fetch-rows 100000` crashes even at `--reps 1`. Rust, Java and Go run the same workload against the same msodbcsql18 and the same server without trouble. |
| **db2** (csharp fetch) | *Binding.* `[ODBC] SQLGetData failed` on all 4 runs of the 100,000-row read, while python, rust, go and java read the same 100,000 rows through the same driver and server. Size-dependent: 5,000 and 50,000 rows read fine from C#, 100,000 fails even at `--reps 1`. The `System.Data.Odbc` comparison fails separately with `Arithmetic operation resulted in an overflow` — the clidriver's 32-bit `SQLLEN` reaching a client that assumes 64-bit. |
| **db2** (go/csharp native fetch) | *Driver/client.* `alexbrainman/odbc` reports `SQLGetDiagRec failed: ret=-2`; both are the 32-bit `SQLLEN` again. The `odbc-api` numbers in the rust row are for the same reason not to be read as real throughput. |
| **matrixone** (go native) | *Server.* MatrixOne rejects the quoted-identifier `INSERT`/`SELECT` that `database/sql` prepares with `SQL parser error`. |
| **vertica** (csharp native fetch) | *Client.* `System.Data.Odbc` raises `Unable to cast object of type 'System.Int64' to type 'System.Int32'` — Vertica has one 64-bit integer type and the comparison reads it as `int`. |

**Oracle's fetch column used to be empty, and what filling it costs.** It was
`—` in all five languages because the read *crashed the process*: any row-array
fetch of a character column segfaulted inside Oracle's own `libsqora.so.23.1`
(`bcoReturnColData` ← `bcoCacheFetchNext` ← `SQLFetch`), from every binding and
from plain `adbc_driver_manager`. The driver no longer moves
`SQL_ATTR_ROW_ARRAY_SIZE` on SQORA: the reader settles the rowset before the
first fetch and never changes it again — no probe-then-restore for bind-width
adaptation, no collapse to one row to repair a truncated value. Nothing on this
driver can repair one anyway (`SQLGetData` above array size 1 and
`SQLSetPos(SQL_POSITION)` at any size both answer HY109, and
`SQLFetchScroll(SQL_FETCH_ABSOLUTE)` answers HY106), so a column whose declared
width is a type maximum rather than a real bound stays unbound and is read with
`SQLGetData` — one row at a time.

This benchmark's `txt` is an Arrow string, and generated ingest DDL spells that
`CLOB` on Oracle (`user_tab_columns` reads `id NUMBER, val FLOAT, txt CLOB, dt
DATE`); a `CLOB` is `SQL_LONGVARCHAR` of column size 2,147,483,647, so this read
is exactly the shape that gives its block cursor up. That is the whole of the
cost: the five ADBC fetch rates are 66k–122k rows/s where the same four columns
read 1.1M–2.4M rows/s from servers whose strings are ordinary `VARCHAR`.
`odbc-api` reads the *same* result set at 136,334 rows/s and `arrow-odbc` at
145,299 (see [`RUST_BENCHMARKS.md`](RUST_BENCHMARKS.md)), within the same band —
so what is being paid for is the row-at-a-time protocol, not the Arrow layer on
top of it. A result set with no LOB column keeps the full block cursor and is
untouched.

The C# ADBC *fetch* is the one step in this table that is not reliable. Besides
its two rows in the table above it failed intermittently with a bare `[ODBC] SQLFetch failed`
on postgres (2 runs in 4) and once on timescaledb, both of which then read the
same 100,000 rows on a re-run — and it is the only binding here that does so.
Its two hard failures (Db2's `SQLGetData`, Azure SQL Edge's heap corruption) are
both at 100,000 rows and both go away below ~50,000, which is the same shape.

For **arcadedb**, **opensearch**, **tdengine**, **mongodbbi**, **flightsql**,
**ignite** and **dremio** the python *ADBC fetch* is not the table above but the
entry's pre-loaded `adbc_big` read end to end — 100,000 rows for all of them
except TDengine, which holds 20,000 — which is what `matrix_bench.py` times on a
`read_only` entry (and on `ignite`, which is write-refusing rather than
read-only). Those figures are comparable with themselves across runs, not with
the other rows: the table is `(a, b)`, not the four-column one every other row
reads.

These numbers were taken on a host shared with two other benchmark agents, load
average 2.5–7.9 and swap exhausted. The ingest column moves ±25–60% run to run
on the fast servers (postgres ADBC ingest measured 483k and 603k for rust, 402k
and 728k for csharp in separate runs); the fetch column is stable to a few
percent. Read them as ratios.

The sixteen databases added or re-measured after that — every row from
**oracle** down — were taken on the same host with 36 containers up, at a
one-minute load average between 1.8 and 5.2 (`uptime` before and after each
database; it was 18.1 when the session started and had drained by the first
run). The same caveat holds, and **columnstore** is the worst of it: four
consecutive rust runs of the identical command read 50,835, 12,124, 14,687 and
54,005 rows/s of ADBC ingest, a 4.5× spread, while its `odbc-api` comparison
stayed inside 421k–472k after one 907k outlier. The recorded columnstore row is
the last of those four, which is the one that agrees with the other four
languages. Every other database's ingest column repeated inside the usual
±25–60%, and no fetch number moved more than a few percent between runs.
