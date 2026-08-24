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

## Host

| | |
|---|---|
| OS | _e.g. Windows 11 23H2 x64_ |
| CPU / RAM | _e.g. 8 GB_ |
| Driver manager | Windows ODBC (odbc32.dll) |
| Build | `cmake -S . -B build && cmake --build build --config Release` (MSVC 2022) |
| Python | _3.12.x_, `adbc-driver-manager`, `pyarrow`, `pyodbc` |
| ODBC drivers | SQLite ODBC (sqliteodbc x64) _version_; psqlodbc _version_; ODBC Driver 18 for SQL Server |
| Load | _idle; one server at a time on an 8 GB machine_ |

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
| sqlite | | |
| postgres | | |
| mssql | | |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | | | | |
| postgres | | | | |

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | | | | |
| ingest | | | | |

Windows notes to watch for: the driver manager is not unixODBC, so anything
keyed on unixODBC's error text (`Can't open lib`) is Linux-only; `SQLLEN` is
64-bit on x64 Windows as on Linux; paths in connection strings need no escaping
but a `Driver=` value with spaces needs braces: `Driver={SQLite3 ODBC Driver}`.
