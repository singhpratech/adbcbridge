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

Two things share the word "Java" here and they are not the same. The `java` rows are
Java *as a client* of this driver: a Java program loads `libadbc_driver_odbc.so` through
`adbc-driver-jni` and talks ODBC through it, exactly as Python or Go do. "JDBC" appears
only in Java's *native comparison* column, as the ordinary path a Java program would use
instead. The JDBC *bridge* on the roadmap -- a separate library that would host a JVM to
reach databases that ship only a JDBC driver -- does not exist yet and is not measured here.

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
  the bulk of the work, the five ADBC ingest rates span 1.74× (278k rows/s from
  Python to 483k from Rust; the other four sit inside 1.22×). Where the server
  sets the pace more tightly the binding disappears: the five ingest rates are
  within 1.01× of each other on StarRocks and 1.09× on YDB.
- **The fetch column is where the five bindings agree.** On the 47 rows below
  where all five languages measured, ADBC fetch spreads no more than 1.25×
  across languages on 28 of them and no more than 1.5× on 38, and that agreement
  holds across rates more than four times apart (546k–649k rows/s on ydb against
  2.28M–2.71M on mariadb). Re-measured, SQLite — where
  the per-row database cost is near zero and the binding should show most —
  spreads 1.37× on fetch and 1.34× on ingest, not the 2.5× this file first
  recorded.
- **The ingest column is looser, and noisier.** It runs from 1.01× across
  languages (starrocks) to 3.13× (CrateDB), and SQLite's own control run moved by
  a factor of four between repetitions of the identical workload. Treat a single
  ingest cell as an order-of-magnitude reading and the agreement across a row as
  the real result.
- **Java pays the most, by a little.** `adbc-driver-jni` has the slowest ADBC
  fetch on 20 of those 47 rows — more than any other binding, Python next at
  10 — and the slowest SQLite ingest. On ingest overall it is not the laggard:
  of the 40 rows with an ingest number in all five languages, python is slowest
  on 13, java on 9 and rust on 8. Every one of these margins is inside this
  host's run-to-run noise on any single row.
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

The seven databases added on 2026-09-03 — SingleStore, SAP HANA Express, Exasol, Altibase, Kinetica, Actian Ingres and IBM Db2 for i — were measured on 2026-09-04 with the bridge rebuilt from `main` at 530720e, the same workload and sizes, three databases at a time on the reference host (1-minute load 0.4–3.3). Db2 for i is the one remote entry: the public IBM i host PUB400.COM at ~110 ms round trip, one connection at a time from the account, so its rows are a WAN measurement and its native comparisons mostly could not open their second connection (see below).

## Results

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 801,668 | 1,595,715 | — | — |
| rust | sqlite | 997,745 | 1,795,703 | 735,267 | 1,996,522 |
| csharp | sqlite | 765,539 | 1,536,939 | 296,819 | 980,392 |
| java | sqlite | 743,890 | 1,309,780 | 265,672 | 1,371,841 |
| go | sqlite | 804,033 | 1,580,606 | 356,741 | 682,859 |
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
| rust | greptimedb | 140,269 | 2,609,913 | — | 2,962,675 |
| csharp | greptimedb | 71,525 | 2,213,432 | — | — |
| java | greptimedb | 145,493 | 2,282,943 | — | — |
| go | greptimedb | 84,443 | 2,501,208 | — | — |
| python | questdb | 131,909 | 1,390,435 | — | — |
| rust | questdb | 136,242 | 1,239,831 | — | 1,304,348 |
| csharp | questdb | 166,459 | 1,323,168 | — | 749,581 |
| java | questdb | 160,660 | 1,187,772 | — | — |
| go | questdb | 179,188 | 1,376,015 | — | — |
| python | materialize | 24,021 | 174,074 | — | — |
| rust | materialize | 31,735 | 271,104 | — | 494,995 |
| csharp | materialize | 31,689 | 255,175 | — | 284,581 |
| java | materialize | 33,159 | 498,412 | — | — |
| go | materialize | 33,404 | 362,933 | — | — |
| python | firebird | 40,468 | 297,837 | — | — |
| rust | firebird | 36,045 | 293,842 | — | 288,629 |
| csharp | firebird | 37,507 | 258,789 | — | — |
| java | firebird | 34,089 | 270,097 | — | — |
| go | firebird | 37,564 | 300,858 | — | — |
| python | arcadedb | — | 397,502 | — | — |
| rust | arcadedb | — | 327,191 | — | 369,610 |
| csharp | arcadedb | — | 355,848 | — | — |
| java | arcadedb | — | 332,503 | — | — |
| go | arcadedb | — | 417,419 | — | — |
| python | opensearch | — | 120,772 | — | — |
| rust | opensearch | — | 126,674 | — | — |
| csharp | opensearch | — | 120,479 | — | — |
| java | opensearch | — | 120,801 | — | — |
| go | opensearch | — | 126,886 | — | — |
| python | tdengine | — | 739,772 | — | — |
| rust | tdengine | — | 933,052 | — | — |
| csharp | tdengine | — | 818,384 | — | — |
| java | tdengine | — | 773,804 | — | — |
| go | tdengine | — | 878,675 | — | — |
| python | duckdb | 305,140 | — | — | — |
| rust | duckdb | 227,426 | 3,051,699 | — | — |
| csharp | duckdb | 231,452 | 2,971,883 | 23,133 | 1,342,668 |
| java | duckdb | 213,181 | 2,643,702 | — | — |
| go | duckdb | 230,264 | 2,283,542 | — | — |
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
| rust | mongodbbi | — | 164,089 | — | 169,543 |
| csharp | mongodbbi | — | 141,424 | — | — |
| java | mongodbbi | — | 148,167 | — | — |
| go | mongodbbi | — | 164,004 | — | — |
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
| rust | cratedb | 12,915 | 764,288 | 741 | 1,019,171 |
| csharp | cratedb | 14,663 | 791,619 | 135 | 660,045 |
| java | cratedb | 14,999 | 751,566 | — | — |
| go | cratedb | 12,865 | 844,199 | — | — |
| python | spanner | 7,870 | 205,396 | — | — |
| rust | spanner | 8,091 | 283,050 | — | 272,802 |
| csharp | spanner | 8,775 | — | — | — |
| java | spanner | 9,120 | 250,018 | — | — |
| go | spanner | 8,611 | 252,463 | — | — |
| python | flightsql | — | 1,247,578 | — | — |
| rust | flightsql | — | 1,283,196 | — | 1,313,905 |
| csharp | flightsql | — | 1,236,047 | — | — |
| java | flightsql | — | 1,186,500 | — | — |
| go | flightsql | — | 1,202,181 | — | — |
| python | ignite | — | 934,431 | — | — |
| rust | ignite | — | 794,157 | — | 685,390 |
| csharp | ignite | — | 893,506 | — | — |
| java | ignite | — | 695,225 | — | — |
| go | ignite | — | 615,646 | — | — |
| python | dremio | — | 1,230,968 | — | — |
| rust | dremio | — | 922,869 | — | 1,295,987 |
| csharp | dremio | — | 1,166,339 | — | — |
| java | dremio | — | 985,335 | — | — |
| go | dremio | — | 928,137 | — | — |
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
| csharp | doris | 2,507 | 1,961,684 | — | — |
| java | doris | 2,636 | 1,680,902 | — | — |
| go | doris | 2,485 | 1,663,943 | — | — |
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
| rust | risingwave | 24,021 | 1,060,921 | 1,198 | 1,074,690 |
| csharp | risingwave | 27,557 | 1,017,418 | — | 674,502 |
| java | risingwave | 21,784 | 945,434 | — | — |
| go | risingwave | 27,017 | 900,750 | — | — |
| python | virtuoso | 10,029 | 968,528 | — | — |
| rust | virtuoso | 20,457 | 828,959 | — | — |
| csharp | virtuoso | 23,527 | 419,156 | 12,788 | 424,783 |
| java | virtuoso | 9,948 | 978,832 | — | — |
| go | virtuoso | 17,678 | 847,872 | — | — |
| python | influxdb3 | — | 1,263,303 | — | — |
| rust | influxdb3 | — | 1,273,136 | — | 1,277,855 |
| csharp | influxdb3 | — | 1,262,921 | — | — |
| java | influxdb3 | — | 1,172,879 | — | — |
| go | influxdb3 | — | 1,279,289 | — | — |
| python | ydb | 1,669 | 572,099 | — | — |
| rust | ydb | 1,731 | 642,348 | — | — |
| csharp | ydb | 1,704 | 546,263 | — | — |
| java | ydb | 1,597 | 600,205 | — | — |
| go | ydb | 1,742 | 649,146 | — | — |
| python | access | — | 2,119,999 | — | — |
| rust | access | — | 3,297,308 | — | — |
| csharp | access | — | 3,124,349 | — | — |
| java | access | — | 803,137 | — | — |
| go | access | — | 2,576,841 | — | — |
| python | singlestore | 201,384 | 1,676,765 | — | — |
| rust | singlestore | 195,038 | 1,300,312 | 16,175 | 2,312,971 |
| csharp | singlestore | 117,605 | 1,435,985 | 18,105 | 1,012,648 |
| java | singlestore | 175,056 | 1,415,632 | — | — |
| go | singlestore | 109,652 | 1,843,792 | 17,075 | 604,631 |
| python | hana | 682,120 | 7,283,051 | — | — |
| rust | hana | 779,004 | 7,233,554 | 1,300,593 | 5,843,678 |
| csharp | hana | 688,705 | 7,159,580 | 10,169 | 826,358 |
| java | hana | 578,624 | 4,669,900 | — | — |
| go | hana | 762,244 | 7,402,942 | — | — |
| python | exasol | 10,323 | 1,507,321 | — | — |
| rust | exasol | 10,375 | 1,466,367 | 103,819 | 4,531,326 |
| csharp | exasol | 10,360 | 1,275,818 | 14,788 | — |
| java | exasol | 10,190 | 1,423,727 | — | — |
| go | exasol | 10,599 | 1,396,677 | 20,911 | 725,624 |
| python | altibase | 959,383 | 2,387,341 | — | — |
| rust | altibase | 965,812 | 2,503,114 | 686,317 | 5,104,674 |
| csharp | altibase | 972,573 | 2,434,049 | 42,058 | 1,157,544 |
| java | altibase | 399,477 | 1,953,582 | — | — |
| go | altibase | — | — | — | — |
| python | kinetica | — | 597,315 | — | — |
| rust | kinetica | — | 542,148 | — | 573,141 |
| csharp | kinetica | — | 589,845 | — | — |
| java | kinetica | — | 550,805 | — | — |
| go | kinetica | — | 579,404 | — | — |
| python | ibmi | 1,392 | 1,929 | — | — |
| rust | ibmi | 1,575 | 1,665 | 6,160 | 5,633 |
| csharp | ibmi | 1,779 | 1,378 | — | — |
| java | ibmi | 1,497 | 1,316 | — | — |
| go | ibmi | 2,005 | 1,692 | — | — |
| python | ingres | 1,853 | — | 1,328 | — |
| rust | ingres | 6,859 | 362,335 | 2,501 | 347,394 |
| csharp | ingres | — | — | — | — |
| java | ingres | — | — | — | — |
| go | ingres | 9,989 | 357,583 | 4,086 | 318,450 |

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
ingest). They are the full autocommit-off workload: the pass that first
re-measured them had to run the four non-python harnesses with
`ADBC_BENCH_AUTOCOMMIT=1` because of a harness bug (see **sqlite** in the table
below), which was then fixed and the four rows re-taken with autocommit off —
rust 997,745, csharp 765,539, java 743,890 and go 804,033 rows/s of ADBC ingest,
within noise of the autocommit-on figures (944,635 / 844,423 / 732,161 /
827,133). `ADBC_BENCH_AUTOCOMMIT=1` is why **databend**, **doris**, **starrocks**
and **ydb** have numbers at all.

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
| **questdb** (native ingest) | *Server rule, handled.* With autocommit off QuestDB accepts the ingest but the table holds 0 rows after `COMMIT`; with autocommit on it stores all 10,000. The four harnesses now run it with `ADBC_BENCH_AUTOCOMMIT=1` and agree at 136k–179k rows/s ingest and 1.19M–1.38M fetch (python: 22,383 / 1,451,460 on the matrix workload). Native ingest: Rust's `odbc-api` writes inside a transaction the autocommit connection never commits (`wrong row count 0`), C#'s prepared `INSERT` hits psqlodbc's `ERROR: duplicate statement [name=_PLAN0x…]` against QuestDB, and Go runs `-no-native` because `database/sql` dies in psqlodbc's `SQLFreeHandle` there. |
| **materialize** (native ingest) | *Server rule, handled.* Materialize runs one statement per transaction, so a create-mode ingest under autocommit off fails at its `CREATE TABLE` (`[25000] this transaction can only execute a single statement`). With `ADBC_BENCH_AUTOCOMMIT=1` all four harnesses have numbers: 31.7k–33.4k rows/s ingest, 255k–498k fetch. Rust's and C#'s native ingest read back 0 rows for the same transaction reason; Go is `-no-native`. |
| **firebird** (native columns) | *Server rule, handled — and a driver-side finding.* Firebird does not make a table created inside an open transaction visible to a later statement in that transaction, so the four harnesses run it with `ADBC_BENCH_AUTOCOMMIT=1`, as `matrix_bench.py` always did. Their first pass read the table back at 5.5k–7.7k rows/s — and `odbc-api` at the same 7.5k — which turned out to be our generated DDL: it spelled the Arrow string as `BLOB SUB_TYPE TEXT` and OdbcFb reads a BLOB one row at a time. The driver now spells it `VARCHAR(8191)`; re-measured, all five languages agree at 34k–40k rows/s ingest and 259k–301k fetch (was 5–8k both ways). Native columns: OdbcFb refuses `System.Data.Odbc`'s prepared statements with an empty diagnostic, `odbc-api`'s ingest never commits, Go is `-no-native`, Java has no JDBC URL. |
| **greptimedb** (native columns) | *Server, surfaced by the ODBC driver; handled.* GreptimeDB has no transactions (`SQL_TC_NONE`), so MySQL Connector/ODBC rejects `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `[HYC00] Transactions are not enabled` and an autocommit-off connection never opens. With `ADBC_BENCH_AUTOCOMMIT=1` all four have numbers — 71k–145k rows/s ingest, 2.21M–2.61M fetch, the fastest reads in this table after MariaDB. The native comparisons need the same autocommit-off connection (Rust `SQLSetConnectAttr` error, C# `Transactions are not enabled`), so they stay empty; Go is `-no-native`. |
| **opensearch** (native columns) | *Driver, read-only; handled.* The OpenSearch SQL ODBC driver is read-only and has no `SQLBindParameter`, so the entry is `read_only`; its Linux build (the project ships Windows and macOS binaries only) was rebuilt from source for this pass with the same five fixes `tests/compat/README.md` documents. All four harnesses now read the 100,000-document `adbc_big` at 120k–127k rows/s, next to python's 120,000. Native columns: `odbc-api` cannot connect at all — the driver's ANSI `SQLDriverConnect` fails with an empty diagnostic queue (`No Diagnostics available`), the quirk adbcbridge itself works around by retrying through `SQLDriverConnectW`; Go and C# read the wrong column shape; Java has no JDBC URL. |
| **arcadedb**, **tdengine** (native columns) | *Read-only entries; both handled.* ArcadeDB has no `CREATE TABLE` (its DDL is `CREATE DOCUMENT TYPE` plus one `CREATE PROPERTY` per column) and every TDengine table must start with a `TIMESTAMP` primary key that no generated DDL emits, so both are `read_only`; the four harnesses fetch the pre-loaded 100,000-row `adbc_big` — ArcadeDB at 327k–417k rows/s (Rust's `odbc-api` 369,610, `arrow-odbc` 395,745), TDengine at 774k–933k, next to python's 331,629 and 403,000. Both entries' `setup` carries the fixture load itself (4.3 MB and 0.7 MB of INSERTs), which no environment can hold, so `bench/rust/conn.py` exports only the setup statements under 2 KB (`USE`, `CREATE DATABASE IF NOT EXISTS`) and the compat run seeds the fixture. TDengine's driver (`taos-odbc`, built from source) `assert`s and aborts the whole process when `odbc-api` sets `SQL_ATTR_ODBC_VERSION` ("not implemented yet"), so Rust ran `-no-native` and the harness now skips its vendor probe under that flag; Go's and C#'s native fetch fail in `SQLPrepare` (`taos_odbc`: statement not supported) or on column shape. |
| **duckdb** (rust/go/java native) | *Entry, fixed.* The compat entry connected to `Database=:memory:`, and every ODBC connection to an in-memory DuckDB is its own empty database: the harness ingested on one connection and its fetch connection found no table (`Catalog Error: Table with name adbc_bench_rs does not exist!`). The entry now uses a file in the run's temp directory (as sqlite does), and all four have both cells: 213k–231k rows/s ingest, 2.28M–3.05M fetch — the fastest reads in the table. Native columns: DuckDB's ODBC driver throws a C++ exception out of `SQLExecute` on the plain-ODBC path, which `std::terminate`s the process, so Rust and Go ran `-no-native`; C#'s `System.Data.Odbc` comparison survived this time (23,133 / 1,342,668); Java has no JDBC URL. python's row is the earlier matrix measurement. |
| **cratedb** (go native) | *Harness gap, closed; one binding crash left.* The four non-python harnesses used to send a `date32` column whose generated DDL named PostgreSQL's `date`, which CrateDB has no storage type for (`XX000 Type \`date\` does not support storage`). They now apply the compat entry's `ingest_types` (`date32=timestamp_us`) and issue its `REFRESH TABLE` before every row count, the way `matrix_bench.py` always did, and all four have numbers. The go row ran with `-no-native`: with the `database/sql` comparison on, `bench_go` dies with a SIGSEGV in cgo before printing anything (register dump on stderr, no Go stack). |
| **spanner** (csharp fetch; all native ingest) | *Harness gap, closed; one server rule; one open cell.* The four harnesses used to send an `int32` column whose generated DDL named psqlodbc's `int4`, which Spanner has not (`P0001 Type <int4> is not supported; use bigint or int8 instead`); they now apply the entry's `ingest_types` (`int32=int64`). Spanner also refuses DDL inside a transaction (`25000 DDL statements are not allowed in mixed batches or transactions`), so like ydb the four ran with `ADBC_BENCH_AUTOCOMMIT=1`. Rust, Java and Go then agree at 8,091–9,120 rows/s ingest and 250k–283k fetch, next to python's 7,870 / 205,396; Rust's `odbc-api` ingest reads back 0 rows because its `ColumnarBulkInserter` writes inside a transaction the autocommit connection never commits (as on sqlite). Go's first run failed with `Already Exists` on `CREATE TABLE` — the emulator had not finished the preceding `DROP TABLE` — and was clean on the retry that is recorded. **C#'s fetch is the open cell**: the first run reported an error the log truncated, the second did not finish inside the 600 s cap; not yet isolated. |
| **ignite** (all ingest; go/csharp/java native fetch) | *Server rule, and a harness gap closed.* Ignite refuses any `CREATE TABLE` without a `PRIMARY KEY` (`[42000] No PRIMARY KEY defined for CREATE TABLE`), and the generated ingest DDL has none, so there is no create-mode ingest in any language; the compat entry says so with `ingest_create=False`. The four harnesses now treat that like a read-only entry and fetch the pre-loaded 100,000-row `adbc_big` (615,646–893,506 rows/s across the four; Rust's `odbc-api` reads it at 685,390 and `arrow-odbc` at 821,254). Go's and C#'s native fetch read the wrong column shape (`expected 2 destination arguments`, `Unable to cast System.Int64 to System.Int32`); Java has no JDBC URL. |
| **mongodbbi** (all four native columns) | *Server, read-only.* mongosqld is a query engine with no `CREATE TABLE`/`INSERT`, so the entry is `read_only`: the four harnesses now take the fixture's 100,000-document `adbc_big` as the fetch table and skip the ingest steps, which is what python always did. Their *native* fetch is empty because each comparison reads the four-column bench shape (`id, val, txt, dt`) and `adbc_big` has other columns: Go `sql: expected 6 destination arguments in Scan, not 4`, C# `Unable to cast object of type 'System.String' to type 'System.Int32'`, Rust's `odbc-api` and `arrow-odbc` do read it (169,543 and 153,254 rows/s against ADBC's 164,089). |
| **flightsql**, **dremio** (native columns) | *Driver, read-only.* The Arrow Flight SQL ODBC driver has no `SQLBindParameter` and answers `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)` with `[HYC00] (100) [Apache Arrow][Flight SQL] (100) Optional feature not implemented.`, so both are `read_only` entries: the harnesses fetch the pre-loaded 100,000-row `adbc_big` on an autocommit connection and skip ingest. All five languages now have a fetch number for both (1.19M–1.28M rows/s on sqlflite, 0.92M–1.17M on Dremio). Go's and C#'s native comparisons read the wrong column shape (`expected 5 destination arguments`, `Unable to cast System.Int64 to System.Int32`); Rust's `odbc-api` reads it, 1,313,905 and 1,295,987 rows/s. |
| **monetdb** (all four, without `ADBC_BENCH_AUTOCOMMIT=1`) | *Driver.* MonetDBODBClib's `SQLEndTran` is a no-op. With autocommit off the ingest returns a rate (rust measured 189,937 rows/s) and its row-count check passes on the connection that wrote the rows, but the next connection finds nothing: `ERROR [42S02] [MonetDB][ODBC Driver 11.55.7]INSERT INTO: no such table 'adbc_bench_cs'`, `... SELECT: no such table 'adbc_bench_cs'`. The four monetdb rows above were therefore taken with `ADBC_BENCH_AUTOCOMMIT=1`, which is the autocommit-on setting `matrix_bench.py` — and so the python row — uses everywhere. |
| **monetdb** (rust native) | *Client.* `odbc-api` cannot open this DSN at all: `ODBC emitted an error calling 'SQLDriverConnect': State: IM005, Native error: 0, Message: [unixODBC][Driver Manager]Driver's SQLAllocHandle on SQL_HANDLE_DBC failed`. The bridge, pyodbc, `System.Data.Odbc` and `database/sql` all reach the same MonetDBODBClib through the same unixODBC. |
| **mssql**, **percona**, **columnstore**, **yugabyte**, **monetdb** (go native) | *Binding.* `bench_go` dies before it prints anything, so each of these was re-run with `-no-native`, which keeps both ADBC columns and leaves the `database/sql` ones empty. Two shapes, neither in the driver's path. On mssql, percona and monetdb it is arrow-go's own finalizer: `SIGSEGV … signal arrived during cgo execution` on the finalizer goroutine, in `cdata.initReader.func2 -> _Cfunc_ArrowArrayStreamRelease`, while the main goroutine sits in the `database/sql` step (`odbcFetch` for the first two, the closing cleanup connect for monetdb) — an `ArrowArrayStream` from an earlier ADBC read being finalised long after the step that produced it. On yugabyte it is psqlodbc: SIGSEGV under `alexbrainman/odbc`'s `(*Stmt).Close -> (*ODBCStmt).releaseHandle` while closing the prepared `INSERT`, the same crash the questdb row above records. On columnstore it is maodbc: `double free or corruption (!prev)`, SIGABRT, in `rows.Close` at the end of the `database/sql` read. |
| **oracle** (csharp native fetch) | *Client.* `System.Data.Odbc` raises `Unable to cast object of type 'System.Decimal' to type 'System.Int32'` — Oracle stores the `id` column as `NUMBER`, which comes back as a `decimal`, and the comparison reads it as `int`. The same shape as the vertica row below. Go hit the arrow-go finalizer crash on its first oracle run too (`free(): invalid pointer` in `ArrowArrayStreamRelease`); the repeat of the same command finished, and the go oracle row is that run, native columns included. |
| **azuresqledge** (csharp) | *Undetermined, reproducible.* `bench_cs` aborts with glibc's `corrupted double-linked list (not small)` before printing anything, on every one of six full-workload runs. It needs both halves of the workload: `--rows 10000 --fetch-rows 1000` and `--rows 100 --fetch-rows 100000` both finish, `--rows 10000 --fetch-rows 100000` crashes even at `--reps 1`. Rust, Java and Go run the same workload against the same msodbcsql18 and the same server without trouble. |
| **db2** (csharp fetch) | *Binding.* `[ODBC] SQLGetData failed` on all 4 runs of the 100,000-row read, while python, rust, go and java read the same 100,000 rows through the same driver and server. Size-dependent: 5,000 and 50,000 rows read fine from C#, 100,000 fails even at `--reps 1`. The `System.Data.Odbc` comparison fails separately with `Arithmetic operation resulted in an overflow` — the clidriver's 32-bit `SQLLEN` reaching a client that assumes 64-bit. |
| **db2** (go/csharp native fetch) | *Driver/client.* `alexbrainman/odbc` reports `SQLGetDiagRec failed: ret=-2`; both are the 32-bit `SQLLEN` again. The `odbc-api` numbers in the rust row are for the same reason not to be read as real throughput. |
| **matrixone** (go native) | *Server.* MatrixOne rejects the quoted-identifier `INSERT`/`SELECT` that `database/sql` prepares with `SQL parser error`. |
| **vertica** (csharp native fetch) | *Client.* `System.Data.Odbc` raises `Unable to cast object of type 'System.Int64' to type 'System.Int32'` — Vertica has one 64-bit integer type and the comparison reads it as `int`. |
| **sqlite** | *Harness bug, fixed.* `drop_table()` in all four harnesses used to follow a failed `DROP TABLE` with `connection.rollback()` **and** a literal `ROLLBACK`, added for MonetDB. `libsqlite3odbc.so` never issues `BEGIN` again after that literal `ROLLBACK`, so every later `COMMIT` failed with `[HY000] (1) [SQLite]cannot commit - no transaction is active` and the whole run read `—` — the rows had landed (`SELECT COUNT(*)` returned 10,000); only `SQLEndTran(SQL_COMMIT)` errored. The literal `ROLLBACK` bought nothing: MonetDB's `SQLEndTran` is a no-op, so it is measured with `ADBC_BENCH_AUTOCOMMIT=1` and never reaches that branch. It is gone from all four harnesses, and the sqlite rows above are the full autocommit-off workload again, native columns included. |
| **clickhouse** | *ODBC driver.* The 1.3–1.5k rows/s ADBC ingest is real and is the same in all five languages: clickhouse-odbc is on the bridge's no-array-binding list, so the ingest is one `INSERT` per row over HTTP. The native ingest is `—` because that comparison *does* array-bind — rust lands 2 of 10,000 rows (`wrong row count 2 != 10000`) and go's driver answers `SQLExecute: {HY000} Error while processing query …: HTTP status code: 501`. C# had to run with `ADBC_BENCH_NO_NATIVE=1`: with the comparison on, `bench_cs` printed nothing and was still running when it was killed at 600 s. |
| **mariadb** (go native) | *Driver/client.* The `database/sql` read of 100,000 rows never returns. MariaDB's own `PROCESSLIST` shows the connection `Sleep` with no query in flight for 12 minutes while `bench_go` sits at 0.2% CPU, so the client is not waiting on the server. Run with `-no-native`; the ADBC columns are unaffected. Its first ADBC ingest also came out at 44,161 rows/s against 230k–257k for the other four languages, and re-ran at 257,292 — the low one was noise, and the second is what the table carries. |
| **databend**, **doris**, **starrocks** (ran with `ADBC_BENCH_AUTOCOMMIT=1`) | *Server, surfaced by the ODBC driver.* None of the three has transactions, so MySQL Connector/ODBC rejects `SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, OFF)` with `NotImplemented` and the autocommit-off connection never opens — the same failure as **greptimedb** above. With the knob all four languages measure. Their *native ingest* is `—` in every language for the same reason from the other side: the comparison opens its own transaction and gets `{HYC00} [MySQL][ODBC 9.4(w) Driver]Transactions are not enabled`. |
| **starrocks** (go/csharp native fetch) | *Server.* StarRocks quotes with a backtick, so the comparison's `SELECT "id", "val", "txt", "dt"` returns four string *literals* rather than columns: go reads `sql: Scan error on column index 0, name "'id'": converting driver.Value type []uint8 ("id") to a int64: invalid syntax` and C# `Unable to cast object of type 'System.String' to type 'System.Int32'`. The ADBC path quotes with the database's own character and is unaffected. |
| **doris** (native columns) | *Host, then handled.* Go's and C#'s cells were empty on the first passes because Doris's backend refused every statement with `[MEM_ALLOC_FAILED] … sys available memory 1.07 GB less than low water mark 1.60 GB` while 36 containers shared the host; after its frontend also stuck in `wait catalog to be ready` on a restart, the container was recreated and both measured on a quieter host: 2,485 and 2,507 rows/s ingest, 1.66M and 1.96M fetch, next to python/rust/java's 2,405–2,636 and 1.38M–2.04M. The ingest rate is StarRocks/Doris design — every `INSERT` is a backend load transaction — not the driver. Native columns: no transactions (`ADBC_BENCH_AUTOCOMMIT=1`), so the autocommit-off comparisons never connect; Go is `-no-native`. |
| **informix** (csharp native fetch, rust arrow-odbc) | *Driver.* Informix is reached through the same Db2 `clidriver` `libdb2.so`, built with a 32-bit `SQLLEN`: `System.Data.Odbc` raises `Arithmetic operation resulted in an overflow.` and `arrow-odbc` panics with `Failed to retrieve data type from ODBC driver. The SQLLEN could not be converted to a 16 Bit integer`. The bridge detects that layout itself (`adbc.odbc.sqllen_32bit`) and reads the same result set fine, which is why only the comparison columns are empty. |
| **citus**, **opengauss**, **virtuoso** (go native) | *Driver/client.* `alexbrainman/odbc` takes the process down from the plain-ODBC path in all three: citus prints `malloc_consolidate(): invalid chunk size` and then `SIGSEGV … signal arrived during cgo execution` inside `SQLFreeHandle` ← `ODBCStmt.closeByRows` ← `Rows.Close`, which is the same signature as the questdb row above; opengauss segfaults in cgo during the `database/sql` read at `main.go:345`. All three were re-run with `-no-native`, which keeps the ADBC columns. |
| **virtuoso** (rust native) | *Driver.* `odbc-api`'s array-bound insert gets a bare `ODBC emitted an error calling 'SQLExecute':` from `virtodbc.so`, and both of its reads return `read 0 rows, expected 100000` from a `SELECT` the ADBC path drains in full. |
| **risingwave** (go native, csharp native ingest) | *Harness gap, closed.* RisingWave makes committed rows visible to a scan only after `FLUSH`; the compat entry carries `refresh="FLUSH"`, and the four harnesses now run it before every row count as `matrix_bench.py` does, so all four have numbers (21,784–27,557 rows/s ingest, 0.90M–1.06M fetch, all within noise of python's 49,953 / 989,430 given the 4.9–5.7 load they ran at). The go row is `-no-native` — the same cgo SIGSEGV as on cratedb — and C#'s native ingest fails with `ERROR [XX000] ERROR: Failed to prepare the statement` from `System.Data.Odbc`'s prepared `INSERT`. |
| **influxdb3** (native columns) | *Server, read-only.* InfluxDB 3's SQL has no DDL, so the entry is `read_only` and the harnesses read the pre-loaded 100,000-point `adbc_big` instead of ingesting; all five languages land within 1.17M–1.28M rows/s. The four native fetch comparisons read the wrong column shape (Go `expected 3 destination arguments in Scan, not 4`, C# `Unable to cast System.Int64 to System.Int32`); Rust's `odbc-api` reads it at 1,277,855 rows/s, `arrow-odbc` at 1,142,367. |
| **ydb** (ran with `ADBC_BENCH_AUTOCOMMIT=1` and `-no-native`) | *Server.* YDB requires a primary key, so the bridge's create-mode DDL appends one; its PostgreSQL layer will not run that inside an open transaction and answers `[ODBC] CREATE TABLE "adbc_bench_rs_b2" ("id" INTEGER, "val" DOUBLE, "txt" TEXT, "dt" DATE, adbc_pk SERIAL PRIMARY KEY) failed`. With autocommit on it takes it, and all five languages land within 1,597–1,742 rows/s of each other. `-no-native` as well because the row-at-a-time comparison alone overran the 600 s cap in rust, go and C#; that is why the native columns are empty. |
| **access** (native columns) | *Driver, read-only; and a number that is not a throughput.* The `mdbtools` driver is read-only, so the fetch reads the fixture's `adbc_big` — which holds **3,000** rows. The four harnesses used to fail to connect because `bench/rust/conn.py` named a fixture it never copied; it copies it now. The fetch figures (0.8M–3.3M rows/s) are 3,000 rows in about a millisecond, timer resolution rather than a rate; read them as "works", not as a speed. Native fetch: mdbtools has no `SQLPrepare` (`IM001 Driver does not support this function` for Go) and Rust's `odbc-api` trips on `SQLSetStmtAttr` (`HY092 Invalid attribute/option identifier`). |
| **singlestore**, **hana**, **exasol**, **altibase**, **kinetica**, **ibmi**, **ingres** (java native) | *Harness, by design.* `no JDBC URL for <db>; set <DB>_JDBC` — the pom carries only the SQLite and PostgreSQL JDBC drivers, as for every other database in the table. |
| **hana** (go native) | *Binding.* The `database/sql` read died before printing anything: `SIGSEGV: segmentation violation` / `signal arrived during cgo execution` on the finalizer goroutine in `arrow-go/v18 cdata.initReader.func2 -> _Cfunc_ArrowArrayStreamRelease` while goroutine 1 was in `alexbrainman/odbc (*Rows).Next -> SQLFetch` — the same arrow-go finalizer signature recorded above for mssql, percona and monetdb. Re-run `-no-native`: clean; the ADBC columns are that run. |
| **exasol** (csharp native fetch) | *Client.* `Unable to cast object of type 'System.Decimal' to type 'System.Int32'.` — Exasol has no narrow integer type (`INT` is `DECIMAL(18,0)`, described `SQL_DECIMAL`), so `System.Data.Odbc` hands `id` over as a decimal and the comparison reads it as `int`; the same shape as the oracle and vertica rows. ADBC columns and the native ingest are from the same run. |
| **altibase** (go, all four cells) | *Binding.* Three `run.sh` invocations — the measuring pass, its `ADBC_BENCH_NO_NATIVE=1` retry and a verification re-run — died before printing anything with the identical `SIGSEGV: segmentation violation` / `signal arrived during cgo execution`, fault address `0x20002f` every time, on the finalizer goroutine in `arrow-go/v18 cdata.initReader.func2 -> _Cfunc_ArrowArrayStreamRelease` while goroutine 1 was still building the Arrow batch for step 1 (`main.makeRecord`), before any ADBC ingest or native step ran: an `ArrowArrayStream` from the harness's earlier ADBC reads (vendor probe, drop-table row count) finalised on GC timing. `-no-native` cannot help because the native path never starts. The other four languages all have both ADBC cells (399k–973k rows/s ingest, 1.95M–2.50M fetch). |
| **kinetica** (ingest, all five; csharp and go native fetch; python native) | *Entry, by design; harness shape.* Kinetica is a `read_only` compat entry (the driver executes parameter *N* with the value bound at *N+1*, see `COMPATIBILITY.md`), so every language fetches the pre-loaded 100,000-row `adbc_big` and has nothing to ingest; `matrix_bench.py` skips pyodbc for read-only entries. The C# and Go comparisons read the four-column bench shape and `adbc_big` is `(a INTEGER, b VARCHAR)`: `Unable to cast object of type 'System.String' to type 'System.Double'.` and `sql: expected 2 destination arguments in Scan, not 4` — the same pattern as the other read-only entries. Rust's `odbc-api` read the two columns as they are (573,141 rows/s). |
| **ibmi** (csharp and go native; python native fetch) | *Account and link, not the bridge.* PUB400.COM allows one connection at a time from the account and sits ~110 ms away; with the comparison on, `bench_cs` and `bench_go` each printed `== ibmi` and then nothing for 600 s while the ADBC connection was open, and were killed — no error, no diagnostic. Both re-run `-no-native`; the ADBC columns are that run. Rust's `odbc-api` did get its own connection (6,160 / 5,633 rows/s). Python's pyodbc fetch of 100,000 rows hit `matrix_bench.py`'s 600 s per-step timeout over the WAN (its ingest, 6,735 rows/s with `fast_executemany`, finished). |
| **ingres** (python ADBC fetch; csharp, all four; java ADBC) | *Server log limit; driver; driver.* Python's fetch step failed loading the 100,000-row table on `matrix_bench.py`'s autocommit connection: `SQLExecute failed` with the diagnostic `[40001] (4706) … Your transaction has been externally aborted` and `E_DM9059_TRAN_FORCE_ABORT … in database adbc is being force aborted` in the server's `errlog.log` — Ingres' logging system force-aborting a transaction that outgrows the log's force-abort limit (20,000 and 50,000 rows load fine); Rust and Go loaded the same 100,000 rows through the same driver with autocommit off and one commit. Its ADBC ingest of 10,000 rows measured normally (1,853 rows/s). C#: `[ODBC] SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT) failed`, then a SIGSEGV with no row printed, under gdb inside the driver's own `IIODsqtb_SQLTables_InternalCall` ← `SQLTables` called from the bridge's ingest under `libcoreclr.so`; identical with `ADBC_BENCH_AUTOCOMMIT=1`, `ADBC_BENCH_NO_NATIVE=1` and a 100-row workload, so neither size nor the comparison. Java: `# SIGSEGV (0xb) … (sent by kill)`, problematic frame `libcompat.1.so EXsignal` — the Ingres client's own EX exception facility installs process-wide signal handlers when its GCF layer loads and takes over the SIGSEGV the JVM uses for implicit null checks, so the JVM dies 0.37 s in, inside the harness's `makeRoot`, before any ADBC step; same with `ADBC_BENCH_AUTOCOMMIT=1`. Both driver findings are recorded in `docs/UPSTREAM.md`. |

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
**ignite**, **dremio**, **influxdb3** and **access** the python *ADBC fetch* is not
the table above but the entry's pre-loaded `adbc_big` read end to end — 100,000
rows for all of them except TDengine (20,000) and Access (3,000) — which is what
`matrix_bench.py` times on a `read_only` entry (and on `ignite`, which is
write-refusing rather than read-only). Those figures are comparable with
themselves across runs, not with the other rows: the table is `(a, b)`, not the
four-column one every other row reads, and **access**'s 3,000 rows in particular
are far too few to read as a throughput next to a 100,000-row one.

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

The sqlite row and the fifteen databases from **mariadb** to **access** were
measured in a third pass,
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

A fourth pass the same afternoon (build `ad44c27`, load average 3.3–5.7 with eight
servers up) filled the cells the harnesses themselves had been leaving empty:
**cratedb**, **risingwave**, **access**, **influxdb3**, **flightsql**, **dremio** and
**mongodbbi** in rust, csharp, java and go, after the harnesses learned the compat
entry's `ingest_types`, its `refresh` statement and its read-only fixture table. Its
sqlite control read 445,935 / 609,114 / 564,968 / 568,728 rows/s of ADBC ingest for
rust / go / java / csharp — 40–55% under the quiet-host rows above, which is the
load, and one more reason to read this table for cross-language agreement rather
than absolute rate.
