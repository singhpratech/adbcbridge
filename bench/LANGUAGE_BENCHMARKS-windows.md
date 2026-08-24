<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language — Windows

The workload of [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) run on **Windows 11
Pro 24H2, Intel Core i7-8550U (4 cores / 8 threads), 7.7 GB RAM, x64**, the OS's own driver
manager (odbc32.dll), adbcbridge at `a019213`, x64 Release — a build with **no prefetch
pipeline and no ingest fan-out** (both compiled out on `_WIN32`). Same harnesses, same
columns: ADBC ingest and fetch (rows/s) through `libadbc_driver_odbc.dll`, then that
language's own ODBC client (Java's is JDBC — sqlite-jdbc — not ODBC). ROWS=10000,
FETCH_ROWS=100000, **REPS=1, single samples** on a thermally limited laptop with the
server on the same box; `BENCHMARKS-windows.md` records 26% run-to-run variance here, so
read rows for cross-language agreement, not absolute rate. Toolchains: Python 3.12.10,
Go with WinLibs GCC 16.1.0 for cgo, Rust stable MSVC, .NET, Maven + JDK. The campaign is in
progress; this file grows as servers come up.

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 209,082 | 456,214 | 146,264 | 254,287 |
| rust | sqlite | 196,713 | 414,294 | 243,985 | 485,325 |
| go | sqlite | 266,413 | 426,972 | 119,186 | 268,775 |
| java | sqlite | 80,041 | 216,175 | 25,177 | 362,294 |
| csharp | sqlite | 177,166 | 223,620 | 64,308 | 170,062 |

Rust's arrow-odbc fetch: 404,126 rows/s. As on Linux and macOS, Java's JDBC column is a
different driver stack (sqlite-jdbc's embedded library, no ODBC), so it is the one
"native" column that is not a like-for-like comparison.
