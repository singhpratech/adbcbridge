<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language — Windows

The workload of [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) run on **Windows 11
Pro 24H2, Intel Core i7-8550U (4 cores / 8 threads), 7.7 GB RAM, x64**, the OS's own driver
manager (odbc32.dll), adbcbridge at `a019213` (tier 3 at `b5d2791`, `ffecd7a` and `9a652ae`), x64 Release — a build with **no prefetch
pipeline and no ingest fan-out** (both compiled out on `_WIN32`). Same harnesses, same
columns: ADBC ingest and fetch (rows/s) through `libadbc_driver_odbc.dll`, then that
language's own ODBC client (Java's is JDBC — sqlite-jdbc — not ODBC). ROWS=10000,
FETCH_ROWS=100000, **REPS=1, single samples** on a thermally limited laptop with the
server on the same box; `BENCHMARKS-windows.md` records 26% run-to-run variance here, so
read rows for cross-language agreement, not absolute rate. Toolchains: Python 3.12.10,
Go with WinLibs GCC 16.1.0 for cgo, Rust stable MSVC, .NET, Maven + JDK. That first
machine's campaign is closed and kept below as history; the second machine's pass over all 46
databases comes first. Python's ingest is `matrix_bench.py`'s array-binding column, as the Linux file records it.

<!-- bigwin-lang-begin -->
## Second machine — five languages × 46 databases, 2026-08-25

The same workload on the **14-core / 32 GB machine of `BENCHMARKS-windows.md`'s second
section** (Windows 11 Home 23H2, i9-13900HK, every server a Docker Desktop/WSL2 container
under a 20 GB VM cap), at `a4d6ce5` plus the four Windows fixes in `src/` that campaign found.
Toolchains, all per-user installs: Go 1.27.0 with WinLibs GCC 16.2.0 (UCRT) for cgo, Rust
1.98.0 stable-msvc, .NET SDK 8.0.424, Temurin JDK 21.0.12 + Maven 3.9.16, Python 3.12.10.
ROWS=10000, FETCH_ROWS=100000, **REPS=1, single samples**, the four compiled harnesses of a
database running concurrently against it (they use tables of their own) and two databases at a
time — so read rows for cross-language agreement, not absolute rate. Python's row is
`matrix_bench.py`'s (ADBC ingest = its array-binding column, native = pyodbc), as the Linux
file records it. 219 of 230 (language, database) rows measured (the eleven without a number are listed under "Why a cell is empty"); 216 ADBC-fetch cells with a number.

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 489,790 | 965,045 | 354,867 | 497,073 |
| rust | sqlite | 361,180 | 770,080 | 639,157 | 765,893 |
| go | sqlite | 377,210 | 1,044,196 | 188,656 | 524,880 |
| java | sqlite | 250,198 | 854,180 | 494,188 | 1,266,705 |
| csharp | sqlite | 398,599 | 762,782 | 214,437 | 572,409 |
| python | duckdb | 109,474 | 635,958 | 1,120 | 382,443 |
| rust | duckdb | — | — | — | — |
| go | duckdb | 92,571 | 1,095,302 | — | — |
| java | duckdb | 100,964 | 780,837 | — | — |
| csharp | duckdb | 113,756 | 1,099,419 | 10,164 | 602,099 |
| python | postgres | 304,057 | 371,764 | 1,541 | 192,102 |
| rust | postgres | 324,008 | 473,271 | 46,244 | 630,409 |
| go | postgres | 246,222 | 897,713 | — | — |
| java | postgres | 363,750 | 612,143 | 87,370 | 1,481,438 |
| csharp | postgres | 278,600 | 542,202 | 1,102 | 318,180 |
| python | mariadb | 40,097 | 588,421 | 1,389 | 357,274 |
| rust | mariadb | 68,318 | 775,845 | 1,879 | 864,779 |
| go | mariadb | 16,949 | 937,920 | — | — |
| java | mariadb | 79,070 | 623,299 | — | — |
| csharp | mariadb | 78,341 | 573,686 | 1,144 | 317,515 |
| python | columnstore | 4,879 | 497,803 | 1,523 | 376,152 |
| rust | columnstore | 13,252 | 667,751 | 2,385 | 524,426 |
| go | columnstore | 23,821 | 522,099 | — | — |
| java | columnstore | 12,306 | 499,647 | — | — |
| csharp | columnstore | 19,395 | 674,955 | 1,311 | 485,016 |
| python | oracle | 19,271 | 30,874 | 292 | 29,842 |
| rust | oracle | 15,430 | 30,121 | 298 | 41,018 |
| go | oracle | 12,604 | 29,839 | — | — |
| java | oracle | 14,453 | 30,160 | — | — |
| csharp | oracle | 11,775 | 36,442 | 256 | — |
| python | clickhouse | 1,202 | 423,413 | — | 319,357 |
| rust | clickhouse | 1,407 | 524,969 | — | — |
| go | clickhouse | 1,401 | 493,012 | — | — |
| java | clickhouse | 1,390 | 222,150 | — | — |
| csharp | clickhouse | 1,411 | 428,950 | — | — |
| python | mssql | 47,838 | 867,720 | 90,589 | 528,646 |
| rust | mssql | 78,869 | 1,056,277 | 79,904 | 1,313,793 |
| go | mssql | 97,021 | 1,265,864 | — | — |
| java | mssql | 88,411 | 1,058,785 | — | — |
| csharp | mssql | 86,439 | 837,322 | 2,055 | 909,385 |
| python | azuresqledge | 26,624 | 920,637 | 38,281 | 590,090 |
| rust | azuresqledge | 53,867 | 714,759 | 21,512 | 700,370 |
| go | azuresqledge | 32,899 | 585,778 | — | — |
| java | azuresqledge | 57,498 | 770,049 | — | — |
| csharp | azuresqledge | 44,375 | 939,421 | 1,597 | 527,420 |
| python | mysql | 30,743 | 605,429 | 1,383 | 399,614 |
| rust | mysql | 44,817 | 706,430 | 2,351 | 833,779 |
| go | mysql | 13,920 | 909,225 | — | — |
| java | mysql | 67,478 | 808,187 | — | — |
| csharp | mysql | 58,228 | 763,030 | 2,296 | 562,876 |
| python | tidb | 49,267 | 579,121 | 752 | 361,928 |
| rust | tidb | 40,554 | 661,809 | 1,631 | 792,678 |
| go | tidb | 62,074 | 884,201 | — | — |
| java | tidb | 64,755 | 860,398 | — | — |
| csharp | tidb | 60,474 | 535,642 | 1,535 | 472,894 |
| python | dolt | 36,764 | 526,988 | 869 | 126,788 |
| rust | dolt | 44,543 | 549,612 | 1,239 | 729,789 |
| go | dolt | 55,136 | 855,532 | — | — |
| java | dolt | 54,432 | 713,179 | — | — |
| csharp | dolt | 53,841 | 793,869 | 1,206 | 594,695 |
| python | databend | 4,677 | 560,952 | — | 437,956 |
| rust | databend | 6,020 | 500,834 | — | 613,766 |
| go | databend | 6,001 | 595,642 | — | — |
| java | databend | 6,343 | 649,031 | — | — |
| csharp | databend | 6,106 | 713,027 | — | 476,826 |
| python | percona | 38,920 | 585,548 | 1,163 | 269,611 |
| rust | percona | 36,764 | 852,092 | 1,970 | 749,839 |
| go | percona | 14,935 | 902,789 | — | — |
| java | percona | 17,307 | 888,526 | — | — |
| csharp | percona | 43,043 | 722,007 | 1,923 | 514,159 |
| python | matrixone | 42,025 | 752,149 | 576 | 475,380 |
| rust | matrixone | 44,325 | 816,434 | 1,116 | 1,097,005 |
| go | matrixone | 45,344 | 908,975 | — | — |
| java | matrixone | 56,341 | 974,774 | — | — |
| csharp | matrixone | 45,409 | 983,827 | 1,077 | 650,005 |
| python | doris | 1,898 | 829,499 | — | 277,358 |
| rust | doris | 1,917 | 1,254,371 | — | 1,206,257 |
| go | doris | 1,914 | 1,327,788 | — | — |
| java | doris | 1,850 | 1,286,235 | — | — |
| csharp | doris | 1,928 | 1,069,711 | — | — |
| python | oceanbase | 76,696 | 800,665 | 2,284 | 471,138 |
| rust | oceanbase | 49,139 | 612,372 | 1,526 | 687,669 |
| go | oceanbase | 41,179 | 460,154 | — | — |
| java | oceanbase | 64,327 | 706,743 | — | — |
| csharp | oceanbase | 34,898 | 630,090 | 1,503 | 381,644 |
| python | greptimedb | 90,967 | 321,240 | — | 191,745 |
| rust | greptimedb | 75,963 | 1,990,002 | — | 1,979,622 |
| go | greptimedb | 78,202 | 2,147,688 | — | — |
| java | greptimedb | 106,418 | 1,837,580 | — | — |
| csharp | greptimedb | 89,505 | 2,159,762 | — | — |
| python | starrocks | 3,790 | 714,882 | — | 516,061 |
| rust | starrocks | 2,721 | 1,261,947 | — | 1,207,888 |
| go | starrocks | 2,613 | 1,098,261 | — | — |
| java | starrocks | 2,772 | 1,080,015 | — | — |
| csharp | starrocks | 2,686 | 1,281,117 | — | — |
| python | mongodbbi | — | 151,363 | — | — |
| rust | mongodbbi | — | 136,683 | — | 139,463 |
| go | mongodbbi | — | 138,503 | — | — |
| java | mongodbbi | — | 170,580 | — | — |
| csharp | mongodbbi | — | 139,447 | — | — |
| python | db2 | 81,651 | 398,715 | 2,780 | 431,224 |
| rust | db2 | 5,811 | 476,481 | 154,873 | 2,050,075 |
| go | db2 | 23,594 | 441,731 | — | — |
| java | db2 | 3,724 | 320,348 | — | — |
| csharp | db2 | 6,762 | 569,518 | 2,328 | — |
| python | informix | 91,977 | 591,439 | 2,308 | 284,124 |
| rust | informix | 65,971 | 480,346 | 74,842 | 543,822 |
| go | informix | 46,777 | 502,276 | — | — |
| java | informix | 41,293 | 498,633 | — | — |
| csharp | informix | 42,941 | 582,093 | 2,302 | — |
| python | monetdb | 79,423 | 361,206 | 306 | 282,512 |
| rust | monetdb | 118,529 | 834,415 | — | 869,382 |
| go | monetdb | 121,349 | 167,827 | — | — |
| java | monetdb | 106,946 | 763,147 | — | — |
| csharp | monetdb | 120,426 | 601,657 | 1,923 | 400,283 |
| python | vertica | 49,274 | 189,248 | 46,407 | 173,115 |
| rust | vertica | 45,671 | 111,262 | 57,306 | 170,624 |
| go | vertica | 42,831 | 123,435 | — | — |
| java | vertica | 43,999 | 178,746 | — | — |
| csharp | vertica | 37,100 | 239,229 | 2,535 | — |
| python | cockroachdb | 31,555 | 271,045 | 759 | 233,829 |
| rust | cockroachdb | 19,272 | 499,561 | 2,283 | 538,328 |
| go | cockroachdb | 45,950 | 593,239 | — | — |
| java | cockroachdb | 28,938 | 526,250 | — | — |
| csharp | cockroachdb | 29,652 | 439,031 | 610 | 429,946 |
| python | yugabyte | 15,649 | 264,353 | 681 | 221,607 |
| rust | yugabyte | 22,786 | 507,975 | 2,882 | 640,652 |
| go | yugabyte | 15,889 | 770,492 | — | — |
| java | yugabyte | 23,296 | 660,887 | — | — |
| csharp | yugabyte | — | 629,356 | — | 240,317 |
| python | timescaledb | 268,389 | 360,484 | 1,558 | 224,157 |
| rust | timescaledb | 356,373 | 575,062 | 11,464 | 491,686 |
| go | timescaledb | 288,097 | 986,237 | — | — |
| java | timescaledb | 235,565 | 639,884 | — | — |
| csharp | timescaledb | 288,719 | 757,898 | 1,387 | 500,596 |
| python | citus | 259,308 | 259,309 | 1,536 | 285,972 |
| rust | citus | 340,186 | 862,000 | 52,718 | 735,010 |
| go | citus | 436,740 | 987,338 | — | — |
| java | citus | 340,424 | 849,574 | — | — |
| csharp | citus | 411,860 | 417,658 | 1,275 | 212,557 |
| python | cloudberry | 6,252 | 390,739 | 28 | 308,765 |
| rust | cloudberry | 4,767 | 533,646 | 639 | 528,048 |
| go | cloudberry | 4,900 | 462,343 | — | — |
| java | cloudberry | 2,355 | 311,162 | — | — |
| csharp | cloudberry | 4,905 | 416,838 | 377 | 236,676 |
| python | materialize | 18,502 | 103,790 | 651 | 96,458 |
| rust | materialize | 12,604 | 245,164 | — | 201,087 |
| go | materialize | 10,778 | 160,183 | — | — |
| java | materialize | 12,584 | 121,592 | — | — |
| csharp | materialize | 12,293 | 221,441 | — | 229,015 |
| python | opengauss | 76,996 | 257,499 | 1,521 | 219,314 |
| rust | opengauss | 47,059 | 680,514 | 6,835 | 471,796 |
| go | opengauss | 57,975 | 340,678 | — | — |
| java | opengauss | 83,877 | 523,439 | — | — |
| csharp | opengauss | 70,788 | 712,748 | 1,054 | 438,873 |
| python | cratedb | 14,114 | 252,400 | 24 | 236,161 |
| rust | cratedb | 16,075 | 401,018 | 100 | 373,773 |
| go | cratedb | 10,692 | 421,092 | — | — |
| java | cratedb | 26,683 | 245,742 | — | — |
| csharp | cratedb | 14,072 | 306,095 | 74 | 308,569 |
| python | questdb | 62,752 | 374,822 | 2,784 | 259,358 |
| rust | questdb | 51,697 | 305,029 | — | 296,894 |
| go | questdb | 38,415 | 640,658 | — | — |
| java | questdb | 37,073 | 335,605 | — | — |
| csharp | questdb | 39,109 | 429,291 | — | 202,942 |
| python | risingwave | 20,913 | 242,606 | 387 | 245,511 |
| rust | risingwave | 10,259 | 425,952 | 1,598 | 438,843 |
| go | risingwave | 12,501 | 589,773 | — | — |
| java | risingwave | 18,180 | 562,676 | — | — |
| csharp | risingwave | 13,744 | 413,323 | 645 | 229,696 |
| python | spanner | — | — | — | — |
| rust | spanner | — | — | — | — |
| go | spanner | — | — | — | — |
| java | spanner | — | — | — | — |
| csharp | spanner | — | — | — | — |
| python | firebird | 23,638 | 80,444 | 1,326 | 68,197 |
| rust | firebird | 26,599 | 77,615 | — | 78,590 |
| go | firebird | 22,922 | 75,783 | — | — |
| java | firebird | 26,528 | 73,531 | — | — |
| csharp | firebird | 26,206 | 75,933 | 1,333 | 71,436 |
| python | virtuoso | 2,811 | 196,184 | 2,801 | 174,845 |
| rust | virtuoso | 2,076 | 128,847 | 104,725 | — |
| go | virtuoso | 1,448 | — | — | — |
| java | virtuoso | 2,186 | — | — | — |
| csharp | virtuoso | 2,083 | — | 2,012 | — |
| python | flightsql | — | 5,211,319 | — | — |
| rust | flightsql | — | 4,894,236 | — | 126,564 |
| go | flightsql | — | 4,403,133 | — | — |
| java | flightsql | — | 3,244,246 | — | — |
| csharp | flightsql | — | 3,949,073 | — | — |
| python | arcadedb | — | 104,786 | — | — |
| rust | arcadedb | — | 201,914 | — | 268,454 |
| go | arcadedb | — | 238,977 | — | — |
| java | arcadedb | — | 234,593 | — | — |
| csharp | arcadedb | — | 273,689 | — | — |
| python | influxdb3 | — | 6,137,567 | — | — |
| rust | influxdb3 | — | 4,219,979 | — | 152,818 |
| go | influxdb3 | — | 6,218,596 | — | — |
| java | influxdb3 | — | 1,952,927 | — | — |
| csharp | influxdb3 | — | 3,295,577 | — | — |
| python | ignite | — | 775,660 | — | — |
| rust | ignite | — | — | — | — |
| go | ignite | — | 581,721 | — | — |
| java | ignite | — | 497,746 | — | — |
| csharp | ignite | — | 487,266 | — | — |
| python | opensearch | — | 84,094 | — | — |
| rust | opensearch | — | 53,788 | — | 86,108 |
| go | opensearch | — | 53,908 | — | — |
| java | opensearch | — | 71,444 | — | — |
| csharp | opensearch | — | 50,963 | — | — |
| python | ydb | 1,903 | 340,051 | 69 | 255,062 |
| rust | ydb | — | — | — | — |
| go | ydb | 1,837 | 438,534 | — | — |
| java | ydb | — | — | — | — |
| csharp | ydb | — | — | — | — |
| python | dremio | — | 1,002,050 | — | — |
| rust | dremio | — | 677,539 | — | 115,785 |
| go | dremio | — | 1,509,664 | — | — |
| java | dremio | — | 915,198 | — | — |
| csharp | dremio | — | 671,502 | — | — |
| python | tdengine | — | 298,618 | — | — |
| rust | tdengine | — | — | — | — |
| go | tdengine | — | 274,806 | — | — |
| java | tdengine | — | 227,943 | — | — |
| csharp | tdengine | — | 229,276 | — | — |
| python | access | — | 3,352,705 | — | — |
| rust | access | — | 2,138,732 | — | 1,805,923 |
| go | access | — | 1,888,693 | — | — |
| java | access | — | 629,842 | — | — |
| csharp | access | — | 3,086,737 | — | — |

Rust's arrow-odbc fetch, not in the table: sqlite 655,837, access 1,914,242, postgres 530,215, mysql 634,853, mssql 1,239,726, mariadb 801,968, timescaledb 751,403, citus 716,793, cockroachdb 506,515, yugabyte 381,556, questdb 310,101, percona 689,640, tidb 708,254, dolt 669,032, db2 456,590, cratedb 389,950, dremio 798,995, flightsql 2,632,320, influxdb3 2,370,483, firebird 68,846, monetdb 811,880, azuresqledge 615,510, opengauss 420,001, materialize 167,850, cloudberry 504,398, risingwave 453,536, databend 652,110, greptimedb 1,474,400, matrixone 805,610, starrocks 1,048,928, opensearch 91,237, oracle 38,720, columnstore 537,510, mongodbbi 145,241, arcadedb 254,325, oceanbase 555,545, doris 1,008,345, vertica 153,449.

`—` is a step that did not finish or does not exist for that pair: Go's native columns are
off everywhere but sqlite (`alexbrainman/odbc` faults inside the driver on Windows — DuckDB
took the process down in `SQLExecute` here, the first machine saw it in `SQLGetDiagRec` — and
a crash loses the ADBC numbers too, so the harness ran with `ADBC_BENCH_NO_NATIVE`); Java's
native column exists only where the pom carries a JDBC driver (sqlite, postgres); a read-only
entry (ArcadeDB, the Flight SQL trio, Ignite, TDengine, MongoDB BI, Access, OpenSearch) has no
ingest by construction; ClickHouse's native columns were switched off (one HTTP request per
row, >20 minutes on the first machine); Virtuoso's fetch is the `repair` case of the first
machine (the driver's `SQLSetPos` fails, the value stays unbound). Four servers were measured
one harness at a time rather than four at once — MonetDB (a concurrent `CREATE TABLE` fails
its optimistic-concurrency check; it also needs `ADBC_BENCH_AUTOCOMMIT=1`, as the harness
sources say), Firebird (DDL under four concurrent transactions), ArcadeDB (the 1 GB container
is OOM-killed under four readers) and YDB. Three servers needed a hand after a container
restart, recorded in `tests/compat/README.md`: ColumnStore (`provision` again), MongoDB BI
(`mongosqld` is not part of the image and has to be started after every recreate), Vertica
(the database is not started with the container; `vcluster start_db`). Rows with no result
at all say why in `adbc-results\lang\<lang>-<db>.log` on the machine; the ones worth
recording:

- `csharp` / `spanner`: not run, see go / spanner
- `csharp` / `ydb`: hung, see rust / ydb
- `go` / `spanner`: not run: the emulator wedges under the 10,000-row workload (`docs/COMPATIBILITY.md`), as on the first machine
- `java` / `spanner`: not run, see go / spanner
- `java` / `ydb`: hung, see rust / ydb
- `rust` / `duckdb`: `abort` — the DuckDB ODBC driver throws a C++ exception across the FFI boundary, which Rust cannot unwind (the first machine's finding, unchanged)
- `rust` / `ignite`: `SQLExecDirect failed` on the read of the quoted `"adbc_big"`: Ignite folds unquoted names to upper case and the Rust harness quotes the read-only table, so the name does not match — harness spelling, not the bridge (the other three harnesses read it)
- `rust` / `spanner`: not run, see go / spanner
- `rust` / `tdengine`: taos_odbc asserts in `_env_set_odbc_version()` (`not implemented yet`) on the ODBC version odbc-api sets, before any query — driver-side
- `rust` / `ydb`: hung at 0 % CPU after connecting, killed after 100 s (Java and C# the same; the compat run hit the same hang on this server once, cleared by recreating the container; Go and Python went through)
<!-- bigwin-lang-end -->

## First machine (i7-8550U, 7.7 GB) — historical campaign

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 161,088 | 456,214 | 146,264 | 254,287 |
| rust | sqlite | 222,957 | 429,941 | 290,565 | 535,762 |
| go | sqlite | 235,899 | 456,975 | 100,447 | 290,949 |
| java | sqlite | 157,071 | 408,076 | 83,118 | 680,818 |
| csharp | sqlite | 238,109 | 427,553 | 108,024 | 340,805 |
| python | duckdb | 73,952 | 592,742 | 590 | 267,814 |
| rust | duckdb | abort | abort | abort | abort |
| go | duckdb | 64,308 | 554,729 | skipped | skipped |
| java | duckdb | 56,400 | 517,045 | no driver | no driver |
| csharp | duckdb | 61,744 | 498,014 | 5,273 | 382,357 |
| python | mssql | 36,209 | 583,138 | 21,468 | 270,097 |
| rust | mssql | 46,963 | 745,551 | 34,024 | 810,348 |
| go | mssql | 56,362 | 652,627 | skipped | skipped |
| java | mssql | 34,237 | 573,710 | no driver | no driver |
| csharp | mssql | 60,126 | 701,911 | 5,370 | 555,398 |
| python | postgres | 151,542 | 235,955 | 9,506 | 151,745 |
| rust | postgres | 164,864 | 429,818 | 43,026 | 434,106 |
| go | postgres | 214,632 | 440,254 | skipped | skipped |
| java | postgres | 114,210 | 387,586 | 33,942 | 618,736 |
| csharp | postgres | 225,977 | 445,502 | 4,929 | 258,351 |
| python | mysql | 42,144 | 397,443 | 6,145 | 269,100 |
| rust | mysql | 42,398 | 489,120 | 6,040 | 533,710 |
| go | mysql | 37,381 | 467,658 | skipped | skipped |
| java | mysql | 30,400 | 444,488 | no driver | no driver |
| csharp | mysql | 41,557 | 492,964 | 5,485 | 333,804 |
| python | cockroachdb | 16,863 | 155,144 | 533 | 109,120 |
| rust | cockroachdb | 15,266 | 208,366 | 2,055 | 214,104 |
| go | cockroachdb | 13,392 | 157,819 | skipped | skipped |
| java | cockroachdb | 14,507 | 159,345 | no driver | no driver |
| csharp | cockroachdb | 11,826 | 190,613 | 241 | 133,247 |
| python | timescaledb | 119,999 | 216,602 | 903 | 132,486 |
| rust | timescaledb | 103,331 | 260,449 | 5,243 | 244,966 |
| go | timescaledb | 113,592 | 266,786 | skipped | skipped |
| java | timescaledb | 120,051 | 267,317 | no driver | no driver |
| csharp | timescaledb | 136,470 | 283,250 | 442 | 184,760 |
| python | citus | 160,615 | 266,622 | 1,176 | 145,459 |
| rust | citus | 127,243 | 300,539 | 27,066 | 290,798 |
| go | citus | 182,180 | 332,711 | skipped | skipped |
| java | citus | 128,307 | 285,583 | no driver | no driver |
| csharp | citus | 177,228 | 300,448 | 501 | 183,735 |
| python | cratedb | 5,633 | 104,449 | 108 | 94,266 |
| rust | cratedb | 8,105 | 193,169 | 57 | 216,539 |
| go | cratedb | 9,862 | 59,028 | skipped | skipped |
| java | cratedb | 4,635 | 130,133 | no driver | no driver |
| csharp | cratedb | stopped | stopped | stopped | stopped |
| python | questdb | 28,482 | 87,206 | 1,455 | 97,229 |
| rust | questdb | 30,973 | 210,475 | — | 200,852 |
| go | questdb | 40,043 | 223,036 | skipped | skipped |
| java | questdb | 23,096 | 167,649 | no driver | no driver |
| csharp | questdb | 32,658 | 206,389 | — | 143,983 |
| python | tidb | 32,783 | 328,749 | 750 | 207,015 |
| rust | tidb | 31,824 | 285,981 | 811 | 400,567 |
| go | tidb | 24,113 | 419,858 | skipped | skipped |
| java | tidb | 18,197 | 389,099 | no driver | no driver |
| csharp | tidb | 28,022 | 364,744 | 804 | 282,147 |
| python | mariadb | 60,345 | 410,497 | 5,152 | 199,643 |
| rust | mariadb | 50,310 | 387,832 | 4,063 | 346,889 |
| go | mariadb | 49,873 | 322,501 | skipped | skipped |
| java | mariadb | 32,291 | 172,918 | no driver | no driver |
| csharp | mariadb | 44,275 | 332,106 | 1,977 | 259,194 |
| python | dolt | 33,281 | 351,950 | 538 | 205,135 |
| rust | dolt | stopped | stopped | stopped | stopped |
| go | dolt | 29,653 | 389,574 | skipped | skipped |
| java | dolt | hung | hung | no driver | no driver |
| csharp | dolt | stopped | stopped | stopped | stopped |
| python | percona | 12,319 | 177,759 | 339 | 149,370 |
| rust | percona | 25,916 | 421,338 | 1,020 | 414,096 |
| go | percona | 25,786 | 359,564 | skipped | skipped |
| java | percona | 18,779 | 356,111 | no driver | no driver |
| csharp | percona | 26,822 | 414,062 | 1,087 | 285,388 |
| python | arcadedb | — | 84,639 | — | — |
| rust | arcadedb | — | 97,606 | — | 107,369 |
| go | arcadedb | — | 104,034 | skipped | skipped |
| java | arcadedb | — | 4,361 | no driver | no driver |
| csharp | arcadedb | — | 9,743 | — | — |
| python | risingwave | 14,532 | 187,927 | 414 | 121,140 |
| rust | risingwave | 18,783 | 213,715 | 1,034 | 213,193 |
| go | risingwave | 19,571 | 212,143 | skipped | skipped |
| java | risingwave | 16,367 | 202,135 | no driver | no driver |
| csharp | risingwave | 17,831 | 211,085 | 395 | 164,493 |
| python | materialize | 10,409 | 110,188 | 545 | 73,957 |
| rust | materialize | 11,507 | 110,483 | — | 104,812 |
| go | materialize | 10,657 | 121,389 | skipped | skipped |
| java | materialize | 8,020 | 87,100 | no driver | no driver |
| csharp | materialize | 6,161 | 95,484 | — | 66,504 |
| python | yugabyte | 10,579 | 181,786 | 528 | 128,431 |
| rust | yugabyte | 12,257 | 233,200 | 1,531 | 248,092 |
| go | yugabyte | 11,629 | 259,748 | skipped | skipped |
| java | yugabyte | 9,335 | 235,265 | no driver | no driver |
| csharp | yugabyte | 9,369 | 199,046 | 302 | 153,967 |
| python | opengauss | 58,290 | 196,872 | 1,035 | 103,289 |
| rust | opengauss | 55,925 | 250,729 | 4,118 | 244,812 |
| go | opengauss | 40,932 | 183,759 | skipped | skipped |
| java | opengauss | 42,759 | 250,543 | no driver | no driver |
| csharp | opengauss | 66,640 | 196,339 | 497 | 147,673 |
| python | databend | 7,705 | 334,776 | — | — |
| rust | databend | 7,248 | 340,704 | — | 341,996 |
| go | databend | 6,767 | 327,053 | skipped | skipped |
| java | databend | 6,915 | 324,918 | no driver | no driver |
| csharp | databend | 7,422 | 287,209 | — | 237,985 |
| python | greptimedb | 50,294 | 224,755 | — | 101,186 |
| rust | greptimedb | 65,872 | 944,514 | — | 791,351 |
| go | greptimedb | 43,547 | 771,731 | skipped | skipped |
| java | greptimedb | 39,438 | 747,146 | no driver | no driver |
| csharp | greptimedb | 64,233 | 830,539 | — | — |
| python | matrixone | 34,868 | 484,587 | 683 | 235,948 |
| rust | matrixone | 30,525 | 466,079 | — | 483,167 |
| go | matrixone | 31,728 | 430,154 | skipped | skipped |
| java | matrixone | 14,318 | 469,736 | no driver | no driver |
| csharp | matrixone | 30,498 | 454,826 | 620 | 243,890 |
| python | cloudberry | 4,171 | 224,017 | 276 | 150,536 |
| rust | cloudberry | 4,002 | 280,245 | 438 | 288,134 |
| go | cloudberry | 3,304 | 261,277 | skipped | skipped |
| java | cloudberry | 3,679 | 286,978 | no driver | no driver |
| csharp | cloudberry | 3,725 | 272,792 | 208 | 201,917 |
| python | mongodbbi | — | 97,643 | — | — |
| rust | mongodbbi | — | 91,986 | — | 84,870 |
| go | mongodbbi | — | 97,813 | skipped | skipped |
| java | mongodbbi | — | 86,677 | no driver | no driver |
| csharp | mongodbbi | — | 89,969 | — | — |
| python | clickhouse | 516 | 159,763 | — | 107,933 |
| rust | clickhouse | 967 | 245,606 | — | 371,655 |
| go | clickhouse | 930 | 274,853 | skipped | skipped |
| java | clickhouse | 948 | 210,446 | no driver | no driver |
| csharp | clickhouse | stopped | stopped | stopped | stopped |
| python | ydb | 756 | 101,712 | 38 | 61,361 |
| rust | ydb | no connect | no connect | no connect | no connect |
| go | ydb | no connect | no connect | no connect | no connect |
| java | ydb | no connect | no connect | no connect | no connect |
| csharp | ydb | no connect | no connect | no connect | no connect |
| python | starrocks | 3,957 | 402,109 | — | 213,907 |
| rust | starrocks | 4,543 | 720,935 | — | 640,323 |
| go | starrocks | 4,567 | 645,700 | skipped | skipped |
| java | starrocks | 4,636 | 545,951 | no driver | no driver |
| csharp | starrocks | 4,316 | 590,596 | — | — |
| python | azuresqledge | 21,976 | 237,448 | 20,548 | 136,506 |
| rust | azuresqledge | 12,281 | 212,010 | 6,747 | 137,746 |
| go | azuresqledge | 14,240 | 215,153 | skipped | skipped |
| java | azuresqledge | 23,629 | 428,772 | no driver | no driver |
| csharp | azuresqledge | 25,420 | 393,279 | 763 | 321,355 |
| python | columnstore | 13,432 | 294,795 | 1,466 | 190,464 |
| rust | columnstore | — | — | — | — |
| go | columnstore | — | — | — | — |
| java | columnstore | 12,047 | 320,443 | no driver | no driver |
| csharp | columnstore | — | — | — | — |
| python | monetdb | 55,757 | 268,684 | 236 | 167,720 |
| java | monetdb | 23,491 | 256,594 | no driver | no driver |
| python | virtuoso | 1,194 | repair | — | 75,746 |
| java | virtuoso | 1,074 | repair | no driver | no driver |
| python | ignite | HYC00 | 274,871 | — | — |
| java | ignite | HYC00 | 278,090 | no driver | no driver |
| python | flightsql | — | 2,013,077 | — | — |
| java | flightsql | — | 1,095,096 | no driver | no driver |
| python | influxdb3 | — | 1,592,073 | — | — |
| java | influxdb3 | — | 978,364 | no driver | no driver |

The first five databases: 24 of 25 cells; tier 3 (Docker Desktop on WSL2, one container at a time at 1 GB) follows below them — Rust's arrow-odbc fetch on tier 3: cockroachdb 198,095, timescaledb 229,103, citus 291,885, cratedb 210,859, questdb 211,570, tidb 342,479, mariadb 299,628, percona 427,781, arcadedb 96,694, risingwave 208,683, materialize 94,970, yugabyte 238,927, opengauss 218,913, databend 302,666, greptimedb 581,868, matrixone 410,862, cloudberry 280,066, mongodbbi 85,434 (read-only entry). YDB's harness rows are `no connect`: all four harness processes failed to connect through the registry-registered psqlodbc 16 driver while Python connected through the same adbcbridge DLL (the 16 DLL's libpq/OpenSSL dependencies not resolving for those processes is the likely cause; not chased). StarRocks' rust arrow-odbc fetch: 561,589. ClickHouse's Go, Rust and Java rows come from a re-run (REPS=1); C# stays `stopped` — its row-per-HTTP-request native ingest was stopped after 20 minutes. ADBC ingest is ~950 rows/s in every language there: clickhouse-odbc's per-statement shape, the same on every platform. Rust arrow-odbc fetch: clickhouse 320,597, azuresqledge 173,781, starrocks 561,589. The campaign is closed. Cloudberry ingests at ~3.5k rows/s from all five languages alike — the MPP dispatch cost of a coordinator and two segments in one container, not a client property. Spanner has no rows on Windows: the emulator wedges under the bench (`docs/COMPATIBILITY.md`). The databend, greptimedb, matrixone, mongodbbi and starrocks rows are measured on entries whose compat result on Windows is FAIL for one reason shared by all five (astral characters read back as `???` through Connector/ODBC 8.4.0 from a server without character-set variables, `docs/COMPATIBILITY.md`); the benchmark carries no astral text, so the rows stand, with that caveat. GreptimeDB's 771,731 rows/s from Go is the fastest single cell Windows produced. ArcadeDB is a read-only entry: its ingest cells are empty by construction. Java and C# read ArcadeDB at 4,361 and 9,743 rows/s against ~100,000 for Python, Rust and Go — the same 1.2-1.5M-vs-100k spread the Linux file records for that server, not a Windows effect. `stopped` marks a harness whose *native* row-at-a-time ingest ran at tens of rows/s on CrateDB and Dolt and was stopped after 10 minutes (C# on CrateDB after 30) — the harness prints its row only at the end, so the ADBC numbers for that run are lost with it; `hung` is Java on Dolt, whose ADBC-only run produced nothing in 10 minutes and was killed, unexplained. The sqlite rows were re-taken with the rest of the grid, so they differ from
the first-day numbers in `BENCHMARKS-windows.md` by the run-to-run variance recorded there.
Rust's arrow-odbc fetch, not in the table: sqlite 396,258, mssql 896,103, postgres 411,178,
mysql 489,701. Three words stand for three different kinds of empty native cell — `skipped`
(Go, run with `-no-native`), `abort` (Rust on DuckDB, the process died before a line was
written) and `no driver` (Java, the pom carries a PostgreSQL JDBC driver and sqlite-jdbc only)
— because the next table explains them differently.

Two readings the numbers support. Ingest through System.Data.Odbc is about 5,000 rows/s on
every server but SQLite (5,273 / 5,370 / 4,929 / 5,485 against 41k–226k for the ADBC path),
the same figure across three unrelated drivers — a property of that client's row-at-a-time
parameter binding, not a driver result; Rust's odbc-api shows the same shape on MySQL
(6,040 against 42,398, 7.0×) and PostgreSQL (43,026 against 164,864, 3.8×), while fetch is
within 10% of the ADBC path either way (0.8–1.0×, and 0.9× on SQL Server where the driver's
own wide fetch is fast). The ADBC ingest path's multi-row batching is what separates them.

## What the empty cells are

None of these is the bridge's: in every case the ADBC path ran and the comparison path or
the harness did not.

| cell | why |
|---|---|
| **go** native columns, every server but sqlite | `alexbrainman/odbc` crashes the process on Windows with `Exception 0xc0000005` (access violation) inside `api.SQLGetDiagRec` (`zapi_windows.go:151`), reached from `odbc.NewError` ← `(*Rows).Next` ← `odbcFetch` (`bench/go/main.go:394`) — the path only runs when the driver raises a diagnostic, which SQLite never does. It takes the ADBC numbers down with it, so the four rows were taken with `-no-native`: their native columns are absent by construction, not measured and failed. The Linux and macOS files record the same library faulting on most servers there. |
| **rust / duckdb** | `fatal runtime error: Rust cannot catch foreign exceptions, aborting` — the DuckDB ODBC driver throws a C++ exception across the FFI boundary and Rust cannot unwind it. The other four Rust rows are fine. |
| **java** `no driver` cells | The pom carries a PostgreSQL JDBC driver and sqlite-jdbc only, so duckdb, mssql and mysql have no JDBC comparison to run — not a failure, a column that does not exist for them. Recording the bench also found a harness bug: where a JDBC column is unavailable the bench prints an em dash, and a Windows JVM (`file.encoding` Cp1252) wrote it as the single byte `0xE3`, so the file stopped being valid UTF-8. `bench/java/run.sh` now passes `-Dstdout.encoding=UTF-8 -Dfile.encoding=UTF-8`. |
