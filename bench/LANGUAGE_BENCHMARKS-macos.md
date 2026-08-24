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
| python | tdengine | — | 552,492 | — | — |
| rust | tdengine | — | 507,308 | — | — |

## Why a cell is empty, and what was different on macOS

| entry | note |
|---|---|
| **duckdb** (rust) | *Binding.* `bench_rs` aborts with `fatal runtime error: Rust cannot catch foreign exceptions` — DuckDB's ODBC driver throws a C++ exception across the ODBC boundary on the plain path; Go's native binding takes a SIGBUS on the same driver; Python, Java and C# are fine. Go is `-no-native`. |
| **monetdb** (go, java, csharp) | Re-taking with `ADBC_BENCH_AUTOCOMMIT=1`; with autocommit off the `CREATE TABLE` failed in all four languages (MonetDB's `SQLEndTran` is a no-op, as on Linux). Batch 2. |
| **db2** (go fetch) | Re-taking; batch 2. |
| **clickhouse** | 300 rows ingested, 2,000 fetched, as on Linux (one HTTP request per row). |
| **oracle** | `NLS_LANG=.AL32UTF8` has to be in the environment before `libsqora` loads; the compat harness's in-process setting is too late on macOS. |
| **mssql**, **postgres** | Python only so far; the four harnesses come with a later batch. |
| **access**, **tdengine** | read-only entries: fetch of the fixture only. |
