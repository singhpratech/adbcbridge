<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — Windows

**Status: not yet measured — and, until 2026-08-24, not even built.** The `windows-latest`
CI job had failed on every run since it was added; the first person to build on Windows
found four MSVC-only defects (the Windows SDK's `sqltypes.h` needs `windows.h` first;
`strndup` is not in the MSVC CRT; a same-type cast on `ADBC_ERROR_INIT` that only GCC and
Clang tolerate; and `odbc_bind.c` using pthreads without the `_WIN32` guard the other
files carry). All four are fixed on main. Two things the Windows build does *not* have,
which every number in this file must be read against: the prefetch pipeline
(`ADBC_ODBC_HAVE_PREFETCH`, compiled out on `_WIN32`) and the parallel-ingest worker pool
(`adbc.odbc.ingest_connections` is clamped to 1). So `native_threshold.py` on Windows
measures a materially different code path from the Linux rows, and a partitioned read on
a 4-core laptop is not a comparison at all. A Win32 port of both (SRWLOCK +
CONDITION_VARIABLE + `_beginthreadex`) is the first Windows roadmap item.

The tables below are empty on purpose. Fill them from a real machine and replace this
paragraph with the host description.

## Host — first human run, 2026-08-24, main @ 199f40e

| | |
|---|---|
| OS | Windows 11 Pro, build 26200 (24H2/25H2 branch), x64 |
| CPU / RAM | Intel Core i7-8550U @ 1.80 GHz, 4 cores / 8 threads (mobile U-series); 7.7 GB RAM, ~1.2 GB free at bench time |
| Driver manager | Windows ODBC (odbc32.dll), ANSI code page 1252 |
| Build | CMake 4.4.2, MSVC 19.44.35228 (VS Build Tools 17.14, toolset 14.44), Windows SDK 10.0.26100; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`; DLL links, **7/7 ctest** (`adbc_odbc_c_smoke` is POSIX-only by design), **zero warnings** at /W3; `cmake --install` puts `bin\libadbc_driver_odbc.dll`, `lib\adbc_driver_odbc.lib` and `etc\adbc\drivers\odbc.toml` (keyed `windows_amd64`) in the prefix |
| Python | `C:\...\Python312\python.exe` 3.12.10 x64 (no `py` launcher on this machine), adbc-driver-manager 1.12.0, pyarrow 25.0.1, pyodbc 5.3.0, pytest 9.1.1 |
| ODBC drivers (64-bit) | SQL Server; ODBC Driver 17 for SQL Server; ODBC Driver 18 for SQL Server; SQLite3 ODBC Driver (sqliteodbc_w64, SQLite 3.43.2). Access, Excel and Text drivers on this machine are **32-bit only** and cannot be loaded by a 64-bit adbcbridge — the 64-bit Access Database Engine 2016 redistributable is needed first |
| Build features | **no prefetch pipeline, no ingest fan-out** (both pthreads, compiled out on `_WIN32`; `adbc.odbc.ingest_connections` clamped to 1) |
| Load | one server at a time; nothing else running |

## Setup (PowerShell)

```powershell
git clone https://github.com/singhpratech/adbcbridge; cd adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix $PWD\dist
py -3.12 -m pip install "adbc-driver-manager>=1.7" pyarrow pyodbc pytest
$env:ADBC_ODBC_DRIVER   = "$PWD\build\Release\libadbc_driver_odbc.dll"
$env:SQLITE_ODBC_DRIVER = "SQLite3 ODBC Driver"        # the name the installer registered
py -3.12 tests\test_sqlite.py                          # end to end, no server
py -3.12 tests\compat\test_matrix.py sqlite            # the compat workload
```

PostgreSQL on the same 8 GB machine: the EDB installer or Docker Desktop with
`--memory=2g`; `$env:POSTGRES_ODBC_DRIVER = "PostgreSQL Unicode(x64)"` and
`$env:POSTGRES_CONN` overriding the port if it is not 15432.

## Results

### Compat workload

| database | result | vendor string |
|---|---|---|
| sqlite | PASS | `SQLite (via ODBC) 3.43.2` |
| postgres | not run — no server and no psqlodbc on this machine | |
| mssql | not run yet (driver present) | |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | 175,704 (array binding 173,043) | 153,763 | 457,935 | 256,221 |
| postgres | not run | | | |

Raw line: `sqlite  SQLite (via ODBC) 3.43.2  fetch=457,935/s (pyodbc 256,221/s, native —/s)  ingest=175,704/s array=173,043/s pyodbc=153,763/s`
— fetch 1.79× pyodbc, ingest 1.14×, on a 4-core mobile CPU with 1.2 GB free and a build with neither prefetch nor fan-out; not comparable with the Linux rows.

### `tests/test_sqlite.py` — FAILED at `199f40e`, and it was a real bug

`assert d["s"] == ["héllo", None, ""]` got `'hÃ©llo'`. Not console mangling: statement text
went to ODBC through the narrow entry points (`SQLExecDirect`, `SQLPrepare`,
`SQLDescribeCol`, `SQLColAttribute`, `SQLGetDiagRec`, the catalog calls), which the Windows
driver manager transcodes from the ANSI code page — so a UTF-8 literal was stored
double-encoded, `WHERE s = 'héllo'` matched **0** rows against a correctly stored value,
a column named `prix_€` came back as byte 0x80 (invalid UTF-8 in the Arrow schema), and
anything outside cp1252 was best-fit mapped and lost. Bound parameters and fetched
columns (`SQL_C_WCHAR`) were always correct, which is what hid it; unixODBC passes narrow
text through as UTF-8, which is why Linux and macOS never saw it. Fixed the same day
(`src/odbc_text.c`: the W entry points on Windows); `tests/test_windows_text.py` is the
diagnostic that found it and now verifies it, and the compat workload gained a
statement-literal step so a PASS means something here.

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | | | | |
| ingest | | | | |

Windows notes to watch for: the driver manager is not unixODBC, so anything
keyed on unixODBC's error text (`Can't open lib`) is Linux-only; `SQLLEN` is
64-bit on x64 Windows as on Linux; paths in connection strings need no escaping
but a `Driver=` value with spaces needs braces: `Driver={SQLite3 ODBC Driver}`.
