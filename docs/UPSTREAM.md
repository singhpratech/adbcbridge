<!-- SPDX-License-Identifier: Apache-2.0 -->
# Upstream: what this project found, and gave back

Driving 46 databases through one driver across three operating systems turns up defects
that belong to other projects. This file is the record: what was found, where it was
reported, and what is documented here but not yet filed. Each entry stands on evidence
that anyone can reproduce without adbcBridge in the stack.

## Reported

| Date | Project | Report | What it is | Status |
|---|---|---|---|---|
| 2026-08-25 | unixODBC | [lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239) | The driver manager overwrites its own stack and heap, and the process aborts, on the first `SQL_ERROR` from a driver whose `SQLWCHAR` is 4 bytes (`extract_diag_error_w`, `SQLWCHAR sqlstate[6]`). Reproduces on 2.3.12 and 2.3.14. Filed with a driver-independent reproduction: a 40-line fake driver compiled with `SQL_WCHART_CONVERT` triggers it on Linux, its 2-byte twin does not; plus lldb frames from two real macOS drivers and a fix suggestion. Same crash as the earlier #227 (Informix), which had been closed without a reproduction. | open |
| 2026-08-25 | OpenLink Virtuoso | [openlink/virtuoso-opensource#1469](https://github.com/openlink/virtuoso-opensource/issues/1469) | The macOS driver (Homebrew 7.2.17) is built to iODBC's 4-byte `SQLWCHAR` and nothing says so; through unixODBC every application dies on its first SQL error. Asks for a unixODBC-width build or a documented iODBC-only statement; includes the UTF-8 statement-literal question. | open |
| 2026-08-25 | Dremio / Arrow Flight SQL ODBC | [dremio/warpdrive#16](https://github.com/dremio/warpdrive/issues/16) | The Apple Silicon build 0.9.7 is iODBC-width, undocumented, and aborts under unixODBC on any SQL error (three servers); `LogEnabled=true` makes `SQLAllocHandle(ENV)` fail with `IM004` (an uncaught exception in the logger); the pkg ships `arrow-odbc.ini.orig` but no `arrow-odbc.ini`; the docs contradict themselves on Apple Silicon support. | open |

## Documented here, not yet reported

Each of these is recorded with its first error and the conditions in
[`COMPATIBILITY.md`](COMPATIBILITY.md) and the per-OS benchmark files; reports follow as
time allows, and a reproduction contributed by anyone is welcome.

| Project | Finding | Where recorded |
|---|---|---|
| MariaDB Connector/ODBC ≥ 3.2 (Connector/C 3.4.9) | A parameter array with a NULL `DATE` segfaults the connector; the array path also misreports MySQL's row count. adbcBridge routes those versions row by row (`no_param_arrays`). | `bench/BENCHMARKS-macos.md` batch 2 |
| MariaDB Connector/ODBC 3.2.9 | Its connect-time probe `SELECT 1 FROM DUAL WHERE @@sql_mode LIKE '%ansi_quotes%'` fails on servers without a `DUAL` table (Databend, GreptimeDB); its prepared/binary-literal path fails on Doris and StarRocks. | `COMPATIBILITY.md`, macOS column |
| MariaDB Connector/ODBC 3.2.9 (macOS arm64) | Every MySQL-wire server reads at 39–47k rows/s through it, in five languages and pyodbc alike, against 1.3–4.5M rows/s through MySQL's own connector on the same machine. | `bench/BENCHMARKS-macos.md` batches 3–4 |
| MySQL Connector/ODBC 8.4.0 (Windows) | Needs `NO_SSPS=1` against every non-MySQL server (`No data supplied for parameters in prepared statement`); reads result sets from servers without character-set session variables as 3-byte `utf8`, so astral characters come back as `???` (Databend, GreptimeDB, MatrixOne, MongoDB BI). 9.x, which has neither problem, is not published for Windows. | `bench/BENCHMARKS-windows.md` |
| MySQL Connector/ODBC 26.7.1 (macOS arm64, iODBC build) | Writes UTF-16 code units into iODBC's 4-byte `SQLWCHAR` (surrogate pairs as two units) but does not read bound `SQL_C_WCHAR` parameters in four-byte units consistently — plain ASCII inlined as garbage. adbcBridge binds text narrow for it on a four-byte build. Only obtainable through a JavaScript download page. | `docs/TROUBLESHOOTING.md`, `bench/BENCHMARKS-macos.md` batch 4 |
| DuckDB ODBC (Windows, Rust) | Throws a C++ exception through the FFI boundary; a Rust process cannot unwind it and aborts. | `bench/LANGUAGE_BENCHMARKS-windows.md` |
| `alexbrainman/odbc` (Go, Windows) | Access violation inside `api.SQLGetDiagRec` on the first driver diagnostic, on every server but SQLite. | `bench/LANGUAGE_BENCHMARKS-windows.md` |
| psqlodbc 18.x | Sends `SHOW DateStyle` at connect, which YDB's PostgreSQL layer rejects; 16.x does not. (Arguably YDB's to implement.) | `COMPATIBILITY.md`, macOS and Windows columns |
| Firebird ODBC 3.0.1 / 3.5.0-rc1 | No macOS build; Firebird 5's sample security database ships with no SYSDBA and bootstrapping one needs an administrator. | `COMPATIBILITY.md` |
| Apache Ignite ODBC | The C++ platform layer has only `linux/` and `win/`; the Darwin build stops at `sys/sysinfo.h`. | `COMPATIBILITY.md`, macOS column |
| OpenSearch SQL ODBC | The macOS package is x86_64-only and links `/usr/lib/libiodbc`. | `COMPATIBILITY.md`, macOS column |
| openGauss (enmotech image, arm64) | The MOT engine panics at start inside Docker Desktop's VM (`thread identifiers exhausted`), with or without `enable_numa = false`. | `COMPATIBILITY.md`, macOS column |
| Spanner emulator | A read-write transaction left open by a killed client wedges the emulator (`FATAL: UNAVAILABLE` on every later connection). | `COMPATIBILITY.md`, Windows column |
| Docker Desktop for Windows (WSL 2.7.12) | The data VHDX never returns space to Windows while running; `wsl --shutdown` compacts it. | `bench/BENCHMARKS-windows.md` |
