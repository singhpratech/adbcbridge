<!-- SPDX-License-Identifier: Apache-2.0 -->
# The same benchmark, from every language — Windows

The workload of [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) run on **Windows 11
Pro 24H2, Intel Core i7-8550U (4 cores / 8 threads), 7.7 GB RAM, x64**, the OS's own driver
manager (odbc32.dll), adbcbridge at `a019213` (tier 3 at `b5d2791`), x64 Release — a build with **no prefetch
pipeline and no ingest fan-out** (both compiled out on `_WIN32`). Same harnesses, same
columns: ADBC ingest and fetch (rows/s) through `libadbc_driver_odbc.dll`, then that
language's own ODBC client (Java's is JDBC — sqlite-jdbc — not ODBC). ROWS=10000,
FETCH_ROWS=100000, **REPS=1, single samples** on a thermally limited laptop with the
server on the same box; `BENCHMARKS-windows.md` records 26% run-to-run variance here, so
read rows for cross-language agreement, not absolute rate. Toolchains: Python 3.12.10,
Go with WinLibs GCC 16.1.0 for cgo, Rust stable MSVC, .NET, Maven + JDK. The campaign is in
progress; this file grows as servers come up. Python's ingest is `matrix_bench.py`'s array-binding column, as the Linux file records it.

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
| java | cratedb | — | — | — | — |
| csharp | cratedb | — | — | — | — |

The first five databases: 24 of 25 cells; tier 3 (Docker Desktop on WSL2, one container at a time at 1 GB) follows below them — CrateDB's C# and Java rows are still running (its row-at-a-time native ingest is ~60 rows/s, so each exceeds a 10-minute window). Rust's arrow-odbc fetch on tier 3: cockroachdb 198,095, timescaledb 229,103, citus 291,885, cratedb 210,859. The sqlite rows were re-taken with the rest of the grid, so they differ from
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
