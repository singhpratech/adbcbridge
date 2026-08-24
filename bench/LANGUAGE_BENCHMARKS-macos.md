<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language — macOS

The workload of [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) run on **macOS 26.5,
Apple M4 Max (16 cores, 64 GB), arm64**, unixODBC 2.3.12 built from source, adbcbridge at
`24dab36`. Same harnesses, same columns: ADBC ingest and fetch (rows/s) through
`libadbc_driver_odbc.dylib`, then that language's own ODBC client. Servers ran in Docker
Desktop; the ones marked emulated are amd64 images under Rosetta-class emulation. The host
was never idle (1-minute load 2.4–10.5, recorded per entry in `BENCHMARKS-macos.md`), so read
rows for cross-language agreement, not absolute rate. `-no-native` rows are Go's, whose
`alexbrainman/odbc` binding faults on every server here except SQLite, Access and Informix
(the same failure the Linux file records). The campaign is in progress; this file grows by
batch.

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
| go | monetdb | — | — | — | — |
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

## Why a cell is empty, and what was different on macOS

| entry | note |
|---|---|
| **duckdb** (rust) | *Binding.* `bench_rs` aborts with `fatal runtime error: Rust cannot catch foreign exceptions` — DuckDB's ODBC driver throws a C++ exception across the ODBC boundary on the plain path; Go's native binding takes a SIGBUS on the same driver; Python, Java and C# are fine. Go is `-no-native`. |
| **monetdb** (go) | All four need `ADBC_BENCH_AUTOCOMMIT=1` (with autocommit off the `CREATE TABLE` failed in every language — MonetDB's `SQLEndTran` is a no-op, as on Linux); Java and C# re-taken that way. Go: `bench_go` hung for five minutes and was killed, then failed outright on the retry — no number. |
| **db2** (go fetch) | Re-taking; batch 2. |
| **mysql**, **mariadb** | Pass after the maodbc ≥ 3.2 quirk (`187dfac`): the connector's parameter-array path misreported MySQL's row count and segfaulted on a NULL DATE against MariaDB. Fetch is ~47k rows/s on both against ~800k on the PostgreSQL-wire servers — the Homebrew libmaodbc 3.2.9 read path, not the bridge: pyodbc reads the same tables at 42–46k. Ingest 100k–204k rows/s across the five languages. |
| **spanner** (rust, go, java, csharp) | Every harness failed at its `CREATE TABLE … GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY`: the run was made with autocommit off, and Spanner refuses DDL inside a transaction (`25000 DDL statements are not allowed in mixed batches or transactions`) — the Linux rows were taken with `ADBC_BENCH_AUTOCOMMIT=1`, as ydb's. Re-run pending. |
| **arcadedb** (rust, go, java, csharp) | All four read 0 rows from `adbc_big` while python reads 100,000 — under investigation. |
| **ydb** (all) | psqlodbc 18 cannot connect (`SHOW DateStyle` refused by YDB's PG layer); a driver-version incompatibility, not a bridge one. |
| **questdb**, **materialize** | `ADBC_BENCH_AUTOCOMMIT=1`, as on Linux (autocommit off: `SQLExecute failed` on the ingest). |
| **cockroachdb**, **yugabyte**, **citus** | Tier 3, psqlodbc 18 built from source; CockroachDB and YugabyteDB arm64 native, Citus amd64 emulated. All five languages agree within a few percent on fetch (0.70M–1.04M rows/s); Go is `-no-native`. |
| **clickhouse** | 300 rows ingested, 2,000 fetched, as on Linux (one HTTP request per row). |
| **oracle** | `NLS_LANG=.AL32UTF8` has to be in the environment before `libsqora` loads; the compat harness's in-process setting is too late on macOS. |
| **mssql**, **postgres** | Python only so far; the four harnesses come with a later batch. |
| **access**, **tdengine** | read-only entries: fetch of the fixture only. |
