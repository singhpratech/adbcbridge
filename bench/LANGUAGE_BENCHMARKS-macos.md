| csharp | arcadedb | — | 304,373 | — | — || java | arcadedb | — | 274,520 | — | — || go | arcadedb | — | 281,320 | — | — || rust | arcadedb | — | 276,685 | — | 321,745 || csharp | spanner | 8,761 | 126,408 | — | — || java | spanner | 7,222 | 111,714 | — | — || go | spanner | 8,301 | 125,034 | — | — || rust | spanner | 7,061 | 106,509 | — | 126,469 |<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language — macOS

The workload of [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) run on **macOS 26.5,
Apple M4 Max (16 cores, 64 GB), arm64**, unixODBC 2.3.12 built from source, adbcbridge at
`24dab36`. Same harnesses, same columns: ADBC ingest and fetch (rows/s) through
`libadbc_driver_odbc.dylib`, then that language's own ODBC client. Servers ran in Docker
Desktop; the ones marked emulated are amd64 images under Rosetta-class emulation. The host
was never idle (1-minute load 2.4–10.5, recorded per entry in `BENCHMARKS-macos.md`), so read
rows for cross-language agreement, not absolute rate. `-no-native` rows are Go's, whose
`alexbrainman/odbc` binding faults on every server here except SQLite, Access and Informix
(the same failure the Linux file records). The campaign is complete: 183 cells, every empty one explained below.

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 1,840,039 | 2,335,766 | 785,186 | 1,224,857 |
| go | sqlite | 1,486,298 | 2,633,092 | 606,853 | 1,343,205 |
| java | sqlite | 1,215,608 | 1,780,732 | 1,060,778 | 1,059,368 |
| csharp | sqlite | 1,835,873 | 2,246,585 | — | — |
| python | duckdb | 390,288 | 4,238,635 | 12,280 | 1,299,832 |
| rust | duckdb | — | — | — | — |
| go | duckdb | 333,998 | 4,339,399 | — | — |
| java | duckdb | 314,444 | 3,514,038 | — | — |
| csharp | duckdb | 326,250 | 4,086,436 | — | — |
| python | access | — | 2,205,341 | — | — |
| rust | access | — | 1,348,189 | — | — |
| go | access | — | 1,565,285 | — | — |
| java | access | — | 1,006,698 | — | — |
| csharp | access | — | 2,107,038 | — | — |
| python | postgres | 1,031,335 | 1,259,979 | 42,134 | 707,592 |
| python | azuresqledge | 42,577 | 590,525 | 61,371 | 555,073 |
| rust | azuresqledge | 84,498 | 605,698 | 63,406 | 591,403 |
| go | azuresqledge | 77,646 | 574,301 | — | — |
| java | azuresqledge | 75,259 | 562,274 | — | — |
| csharp | azuresqledge | 77,514 | 573,662 | — | — |
| python | clickhouse | 998 | 727,548 | 16 | 487,468 |
| rust | clickhouse | 1,183 | 476,063 | — | 640,897 |
| go | clickhouse | 1,184 | 648,473 | — | — |
| java | clickhouse | 1,198 | 408,097 | — | — |
| csharp | clickhouse | 1,255 | 457,467 | — | — |
| python | oracle | 28,407 | 78,908 | 1,178 | 55,935 |
| rust | oracle | 27,742 | 47,160 | 1,546 | 52,734 |
| go | oracle | 29,199 | 54,165 | — | — |
| java | oracle | 26,920 | 56,328 | — | — |
| csharp | oracle | 29,575 | 56,389 | — | — |
| python | monetdb | 114,534 | 598,932 | 1,250 | 560,305 |
| rust | monetdb | 125,550 | 601,336 | — | — |
| python | cockroachdb | 75,825 | 799,169 | 2,426 | 497,329 |
| rust | cockroachdb | 62,178 | 828,741 | 11,338 | 813,691 |
| go | cockroachdb | 69,421 | 851,722 | — | — |
| java | cockroachdb | 67,820 | 721,075 | — | — |
| csharp | cockroachdb | 69,484 | 696,866 | — | — |
| python | yugabyte | 36,527 | 858,915 | 1,801 | 496,826 |
| rust | yugabyte | 42,390 | 842,771 | 4,554 | 925,154 |
| go | yugabyte | 48,769 | 909,453 | — | — |
| java | yugabyte | 41,579 | 841,728 | — | — |
| csharp | yugabyte | 49,650 | 896,040 | — | — |
| python | citus | 193,490 | 1,008,472 | 4,202 | 575,339 |
| rust | citus | 433,566 | 994,454 | 60,568 | 994,956 |
| go | citus | 434,011 | 1,017,249 | — | — |
| java | citus | 476,946 | 951,262 | — | — |
| csharp | citus | 530,884 | 1,036,637 | — | — |
| go | monetdb | 98,851 | 185,749 | — | — |
| java | monetdb | 118,473 | 573,845 | — | — |
| csharp | monetdb | 119,689 | 556,486 | — | — |
| python | db2 | 82,281 | 582,691 | 4,869 | 589,701 |
| rust | db2 | 143,859 | 1,283,731 | 315,338 | 3,731,807 |
| go | db2 | 195,931 | — | — | — |
| java | db2 | 196,104 | 1,095,947 | — | — |
| csharp | db2 | 195,760 | 1,250,038 | — | — |
| python | informix | 3,702 | 562,870 | 4,334 | 385,082 |
| rust | informix | 82,181 | 615,606 | 107,244 | 616,686 |
| go | informix | 92,141 | 633,453 | — | — |
| java | informix | 91,478 | 583,216 | — | — |
| csharp | informix | 90,616 | 597,779 | — | — |
| python | mysql | 119,319 | 48,715 | 4,626 | 45,680 |
| rust | mysql | 112,442 | 47,886 | 149,945 | 47,471 |
| go | mysql | 100,440 | 47,300 | — | — |
| java | mysql | 121,184 | 47,061 | — | — |
| csharp | mysql | 107,917 | 47,347 | — | — |
| python | mariadb | 157,034 | 46,926 | 4,219 | 42,450 |
| rust | mariadb | 197,986 | 43,609 | — | 44,397 |
| go | mariadb | 203,689 | 43,716 | — | — |
| java | mariadb | 184,510 | 43,897 | — | — |
| csharp | mariadb | 194,324 | 44,396 | — | — |
| python | timescaledb | 533,002 | 1,074,095 | 4,370 | 581,495 |
| rust | timescaledb | 588,746 | 1,044,356 | 37,544 | 1,079,604 |
| go | timescaledb | 543,315 | 1,109,759 | — | — |
| java | timescaledb | 516,847 | 984,524 | — | — |
| csharp | timescaledb | 569,934 | 1,073,022 | — | — |
| python | cratedb | 24,740 | 454,878 | 516 | 352,906 |
| rust | cratedb | 33,174 | 452,780 | 1,712 | 465,897 |
| go | cratedb | 48,856 | 458,819 | — | — |
| java | cratedb | 61,019 | 438,830 | — | — |
| csharp | cratedb | 56,802 | 461,876 | — | — |
| python | questdb | 112,365 | 545,566 | 4,802 | 373,440 |
| rust | questdb | 120,725 | 567,874 | — | 537,837 |
| go | questdb | 135,283 | 568,675 | — | — |
| java | questdb | 163,159 | 529,526 | — | — |
| csharp | questdb | 166,160 | 564,683 | — | — |
| python | cloudberry | 12,327 | 1,029,974 | 1,013 | 578,469 |
| rust | cloudberry | 12,201 | 972,723 | 1,493 | 970,874 |
| go | cloudberry | 12,225 | 1,067,451 | — | — |
| java | cloudberry | 12,334 | 970,618 | — | — |
| csharp | cloudberry | 12,502 | 994,201 | — | — |
| python | risingwave | 23,843 | 735,801 | 1,051 | 472,795 |
| rust | risingwave | 33,029 | 730,066 | 4,135 | 708,388 |
| go | risingwave | 32,645 | 722,933 | — | — |
| java | risingwave | 42,147 | 706,363 | — | — |
| csharp | risingwave | 42,469 | 721,252 | — | — |
| python | materialize | 32,833 | 164,748 | 2,423 | 101,211 |
| rust | materialize | 31,039 | 324,547 | — | 295,782 |
| go | materialize | 28,614 | 265,437 | — | — |
| java | materialize | 31,575 | 149,707 | — | — |
| csharp | materialize | 31,466 | 327,977 | — | — |
| python | spanner | 6,115 | 81,367 | 265 | 112,280 |
| rust | spanner | — | — | — | — |
| go | spanner | — | — | — | — |
| java | spanner | — | — | — | — |
| csharp | spanner | — | — | — | — |
| python | arcadedb | — | 289,023 | — | — |
| rust | arcadedb | — | — | — | — |
| go | arcadedb | — | — | — | — |
| java | arcadedb | — | — | — | — |
| csharp | arcadedb | — | — | — | — |
| python | tdengine | — | 552,492 | — | — |
| rust | tdengine | — | 507,308 | — | — |
| python | tidb | 105,077 | 46,796 | 3,296 | 44,287 |
| rust | tidb | 86,057 | 45,675 | 134,968 | 44,963 |
| go | tidb | 78,782 | 44,108 | — | — |
| java | tidb | 91,862 | 44,860 | — | — |
| csharp | tidb | 106,326 | 44,541 | — | — |
| python | percona | 133,650 | 45,408 | 4,783 | 42,507 |
| rust | percona | 103,614 | 44,744 | 151,119 | 44,769 |
| go | percona | 170,642 | 44,604 | — | — |
| java | percona | 158,060 | 44,581 | — | — |
| csharp | percona | 171,011 | 44,781 | — | — |
| python | dolt | 105,090 | 44,927 | 3,081 | 43,156 |
| rust | dolt | 85,776 | 44,710 | — | 44,871 |
| go | dolt | 83,953 | 44,607 | — | — |
| java | dolt | 99,336 | 44,255 | — | — |
| csharp | dolt | 104,674 | 44,620 | — | — |
| python | matrixone | 188,201 | 44,995 | 3,116 | 42,976 |
| rust | matrixone | 154,691 | 44,574 | 136,472 | 44,822 |
| go | matrixone | 207,898 | 44,881 | — | — |
| java | matrixone | 199,402 | 44,694 | — | — |
| csharp | matrixone | 201,279 | 44,471 | — | — |
| python | columnstore | 89,203 | 44,197 | 4,188 | 42,263 |
| rust | columnstore | 91,859 | 43,803 | — | 43,374 |
| go | columnstore | 109,248 | 42,589 | — | — |
| java | columnstore | 100,108 | 42,944 | — | — |
| csharp | columnstore | 88,115 | 44,511 | — | — |
| python | mongodbbi | — | 39,271 | — | — |
| rust | mongodbbi | — | 39,215 | — | 39,154 |
| go | mongodbbi | — | 39,172 | — | — |
| java | mongodbbi | — | 38,920 | — | — |
| csharp | mongodbbi | — | 38,967 | — | — |
| python | oceanbase | 102,495 | 45,282 | 3,435 | 43,170 |
| rust | oceanbase | 101,469 | 45,606 | 251,850 | 45,023 |
| go | oceanbase | 89,760 | 44,793 | — | — |
| java | oceanbase | 104,198 | 44,902 | — | — |
| csharp | oceanbase | 102,390 | 44,799 | — | — |
| python | ydb | 1,781 | 541,823 | 94 | 389,313 |
| rust | ydb | 1,744 | 618,847 | — | 624,210 |
| go | ydb | 1,667 | 644,349 | — | — |
| java | ydb | 1,703 | 625,175 | — | — |
| csharp | ydb | 1,664 | 578,305 | — | — |
| python | databend | 13,581 | 2,034,078 | — | — |
| rust | databend | 12,813 | 2,205,864 | — | — |
| go | databend | 13,510 | 2,197,599 | — | — |
| java | databend | 13,387 | 2,039,526 | — | — |
| csharp | databend | 13,844 | 2,214,070 | — | — |
| python | greptimedb | 23,501 | 1,336,557 | — | — |
| rust | greptimedb | 24,641 | 4,416,018 | — | — |
| go | greptimedb | 24,618 | 4,499,530 | — | — |
| java | greptimedb | 24,229 | 4,205,686 | — | — |
| csharp | greptimedb | 25,772 | 4,411,933 | — | — |
| python | doris | 1,226 | 261,990 | — | — |
| rust | doris | 1,254 | 339,647 | — | — |
| go | doris | 1,240 | 320,147 | — | — |
| java | doris | 1,234 | 383,751 | — | — |
| csharp | doris | 1,274 | 415,671 | — | — |
| python | starrocks | 1,026 | 301,326 | — | — |
| rust | starrocks | 976 | 380,261 | — | — |
| go | starrocks | 999 | 371,267 | — | — |
| java | starrocks | 994 | 418,027 | — | — |
| csharp | starrocks | 996 | 445,097 | — | — |
| python | flightsql | — | 8,260,509 | — | — |
| rust | flightsql | — | 8,029,603 | — | 8,630,517 |
| go | flightsql | — | 8,069,343 | — | — |
| java | flightsql | — | 5,637,191 | — | — |
| csharp | flightsql | — | 8,291,874 | — | — |
| python | influxdb3 | — | 8,741,131 | — | — |
| rust | influxdb3 | — | 7,565,179 | — | 9,647,892 |
| go | influxdb3 | — | 8,063,053 | — | — |
| java | influxdb3 | — | 6,168,936 | — | — |
| csharp | influxdb3 | — | 7,560,865 | — | — |
| python | dremio | — | 1,341,476 | — | — |
| rust | dremio | — | 1,341,555 | — | 2,219,183 |
| go | dremio | — | 1,507,204 | — | — |
| java | dremio | — | 1,456,169 | — | — |
| csharp | dremio | — | 1,634,243 | — | — |
| python | virtuoso | 3,065 | 252,282 | — | — |
| rust | virtuoso | abort | abort | abort | abort |
| go | virtuoso | 2,985 | 244,246 | — | — |
| java | virtuoso | 2,935 | 239,594 | — | — |
| csharp | virtuoso | 2,874 | 257,790 | — | — |

## Why a cell is empty, and what was different on macOS

| entry | note |
|---|---|
| **duckdb** (rust) | *Binding.* `bench_rs` aborts with `fatal runtime error: Rust cannot catch foreign exceptions` — DuckDB's ODBC driver throws a C++ exception across the ODBC boundary on the plain path; Go's native binding takes a SIGBUS on the same driver; Python, Java and C# are fine. Go is `-no-native`. |
| **monetdb** (go) | Closed in batch 3 with `ADBC_BENCH_AUTOCOMMIT=1` and `-no-native`; the earlier hang did not recur. All four need `ADBC_BENCH_AUTOCOMMIT=1` (with autocommit off the `CREATE TABLE` failed in every language — MonetDB's `SQLEndTran` is a no-op, as on Linux); Java and C# re-taken that way. Go: `bench_go` hung for five minutes and was killed, then failed outright on the retry — no number. |
| **db2** (go fetch) | Ingest 195,931 stands; fetch stays empty. On a fresh Db2 container that python connects to, `bench_go`'s second `SQLDriverConnect` fails every time (`I/O: [ODBC] SQLDriverConnect failed`) while rust, java and csharp reconnect fine — a Go + IBM clidriver fact, recorded as one. |
| **mysql**, **mariadb** | Pass after the maodbc ≥ 3.2 quirk (`187dfac`): the connector's parameter-array path misreported MySQL's row count and segfaulted on a NULL DATE against MariaDB. Fetch is ~47k rows/s on both against ~800k on the PostgreSQL-wire servers — the Homebrew libmaodbc 3.2.9 read path, not the bridge: pyodbc reads the same tables at 42–46k. Ingest 100k–204k rows/s across the five languages. |
| **spanner** (rust, go, java, csharp) | First run failed at `CREATE TABLE … GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY`: autocommit was off and Spanner refuses DDL inside a transaction. Re-run with `ADBC_BENCH_AUTOCOMMIT=1` (ROWS=300, FETCH_ROWS=2,000 as on Linux): the four rows above. Rust's arrow-odbc fetch 111,214. |
| **arcadedb** (rust, go, java, csharp) | First run read 0 rows: `conn.py` handed the harnesses the fixture's `DROP TYPE`/`CREATE TYPE adbc_big` without its 100,000 INSERTs (which are too long for an environment), so every connection recreated the table empty. Fixed in `b883d62` (statements naming the read-only table stay with the fixture load); the rows above were taken at `f9c27dc` with `ARCADEDB_SETUP` unset by hand, `COUNT(*)` = 100,000 confirmed first. Rust's arrow-odbc fetch 313,678; a second python run read 316,532. |
| **ydb** (all) | psqlodbc 18 cannot connect (`SHOW DateStyle` refused by YDB's PG layer); a driver-version incompatibility, not a bridge one. Built psqlodbc 16.00.0005 from `REL-16_00_0005` for this entry only: PASS, rows above with `ADBC_BENCH_AUTOCOMMIT=1`. Ingest 1,664–1,781 rows/s in all five languages, as on Linux (1,597–1,742): the server's per-statement cost, not the client's. |
| **questdb**, **materialize** | `ADBC_BENCH_AUTOCOMMIT=1`, as on Linux (autocommit off: `SQLExecute failed` on the ingest). |
| **cockroachdb**, **yugabyte**, **citus** | Tier 3, psqlodbc 18 built from source; CockroachDB and YugabyteDB arm64 native, Citus amd64 emulated. All five languages agree within a few percent on fetch (0.70M–1.04M rows/s); Go is `-no-native`. |
| **clickhouse** | 300 rows ingested, 2,000 fetched, as on Linux (one HTTP request per row). |
| **oracle** | `NLS_LANG=.AL32UTF8` has to be in the environment before `libsqora` loads; the compat harness's in-process setting is too late on macOS. |
| **mssql**, **postgres** | Python only so far; the four harnesses come with a later batch. |
| **access**, **tdengine**, **mongodbbi** | read-only entries: fetch of the fixture only. |
| **flightsql**, **influxdb3**, **dremio**, **virtuoso** (batch 5) | Through a bridge built against iODBC — these drivers' macOS builds are iODBC-width, and under unixODBC the driver manager aborts the process on the first SQL error ([lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239)). The three Flight SQL entries are read-only, so ingest is empty by construction; their Rust odbc-api / arrow-odbc cells survived because a read-only workload never raises a diagnostic. Virtuoso's Rust row is `abort`: `bench_rs` runs its odbc-api comparison through unixODBC, the entry writes (a `DROP`/`CREATE` error on the way), and the driver-manager overflow takes the whole process — its ADBC cells go with it. 8 M rows/s on sqlflite and InfluxDB 3 is the fastest read anywhere in these files: Arrow batches straight off a Flight stream. |
| **tidb**, **percona**, **dolt**, **matrixone**, **columnstore**, **oceanbase**, **mongodbbi** (tier 4) | Every MySQL-wire entry in tier 4 went through MariaDB Connector/ODBC 3.2.9 (arm64; MySQL's own connector for macOS arm64 exists — 26.7.1 — but is built for iODBC, see batch 4), and **every one of them fetches at 39–47k rows/s in all five languages and in pyodbc alike** — against 1.0–2.0M rows/s for the same servers on Linux through MySQL Connector/ODBC. Six servers, six clients, one number: the ceiling is the connector's fetch path on this platform, not the servers and not the bridge (ingest through the same connector runs 86–208k rows/s). |
| **databend**, **greptimedb**, **doris**, **starrocks** | Two connectors, two results, both kept. Through MariaDB Connector/ODBC 3.2.9 all four FAIL inside the connector (the connect-time `DUAL` probe for the first two, the prepared/binary-literal path for the last two). Through MySQL's own Connector/ODBC 26.7.1 — Oracle's macOS arm64 binary, which is built for iODBC and so needs a bridge built against iODBC (`bench/BENCHMARKS-macos.md`, batch 4) — all four PASS in all five languages, the rows above. No pyodbc / odbc-api / arrow-odbc columns for them: those clients link unixODBC and cannot load an iODBC driver. And the MySQL-wire fetch ceiling below is the connector's: the same servers read at 1.3–4.5M rows/s through MySQL's connector. |
