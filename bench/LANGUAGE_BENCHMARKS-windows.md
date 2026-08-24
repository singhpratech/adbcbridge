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
progress; this file grows as servers come up. Python's ingest is `matrix_bench.py`'s array-binding column, as the Linux file records it.

| Language | Database | ADBC ingest | ADBC fetch | Native ingest | Native fetch |
|---|---|---:|---:|---:|---:|
| python | sqlite | 161,088 | 456,214 | 146,264 | 254,287 |
| rust | sqlite | 222,957 | 429,941 | 243,985 | 485,325 |
| go | sqlite | 235,899 | 456,975 | 119,186 | 268,775 |
| java | sqlite | 157,071 | 408,076 | 25,177 | 362,294 |
| csharp | sqlite | 238,109 | 427,553 | 64,308 | 170,062 |
| python | duckdb | 73,952 | 592,742 | 590 | 267,814 |
| rust | duckdb | — | — | — | — |
| go | duckdb | 64,308 | 554,729 | — | — |
| java | duckdb | 56,400 | 517,045 | — | — |
| csharp | duckdb | 61,744 | 498,014 | — | — |
| python | mssql | 36,209 | 583,138 | 21,468 | 270,097 |
| rust | mssql | 46,963 | 745,551 | — | — |
| go | mssql | 56,362 | 652,627 | — | — |
| java | mssql | 34,237 | 573,710 | — | — |
| csharp | mssql | 60,126 | 701,911 | — | — |
| python | postgres | 151,542 | 235,955 | 9,506 | 151,745 |
| rust | postgres | 164,864 | 429,818 | — | — |
| go | postgres | 214,632 | 440,254 | — | — |
| java | postgres | 114,210 | 387,586 | — | — |
| csharp | postgres | 225,977 | 445,502 | — | — |
| python | mysql | 42,144 | 397,443 | 6,145 | 269,100 |
| rust | mysql | 42,398 | 489,120 | — | — |
| go | mysql | 37,381 | 467,658 | — | — |
| java | mysql | 30,400 | 444,488 | — | — |
| csharp | mysql | 41,557 | 492,964 | — | — |

24 of 25 cells. The sqlite rows were re-taken with the rest of the grid, so they differ from
the first-day numbers in `BENCHMARKS-windows.md` by the run-to-run variance recorded there.
The native columns beyond SQLite and Python are still to be filled from the raw harness
lines; where the peer run reported them qualitatively, Rust's odbc-api ingest was 7.0× slower
than the ADBC path on mysql and 3.8× on postgres, with fetch within 10% either way.

## What the empty cells are

None of these is the bridge's: in every case the ADBC path ran and the comparison path or
the harness did not.

| cell | why |
|---|---|
| **go** native columns, every server but sqlite | `alexbrainman/odbc` crashes the process on Windows with `Exception 0xc0000005` (access violation) inside `api.SQLGetDiagRec` (`zapi_windows.go:151`), reached from `odbc.NewError` ← `(*Rows).Next` ← `odbcFetch` (`bench/go/main.go:394`) — the path only runs when the driver raises a diagnostic, which SQLite never does. It takes the ADBC numbers down with it, so the four rows were taken with `-no-native`: their native columns are absent by construction, not measured and failed. The Linux and macOS files record the same library faulting on most servers there. |
| **rust / duckdb** | `fatal runtime error: Rust cannot catch foreign exceptions, aborting` — the DuckDB ODBC driver throws a C++ exception across the FFI boundary and Rust cannot unwind it. The other four Rust rows are fine. |
| **java** JDBC columns | Not run on this box beyond SQLite. Recording the bench also found a harness bug: where a JDBC column is unavailable the bench prints an em dash, and a Windows JVM (`file.encoding` Cp1252) wrote it as the single byte `0xE3`, so the file stopped being valid UTF-8. `bench/java/run.sh` now passes `-Dstdout.encoding=UTF-8 -Dfile.encoding=UTF-8`. |
