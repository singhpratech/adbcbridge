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
| duckdb | PASS | `DuckDB (via ODBC) ` (the driver reports no version; duckdb_odbc 1.5.5.0) |
| mssql | PASS | `Microsoft SQL Server (via ODBC) 17.00.1000` — SQL Server 2025 RTM, native install, Windows auth, `Trusted_Connection=yes;TrustServerCertificate=yes` |
| postgres | PASS | `PostgreSQL (via ODBC) 16.15.0` — native install, psqlodbc 18.00.0002 "PostgreSQL Unicode(x64)"; **FAIL at 199f40e–9c07f78** with `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xe9`, see the second bug below |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | 209,082 (array binding 161,088) | 146,264 | 456,214 | 254,287 |
| duckdb | 74,005 (array 73,952) | 590 | 592,742 | 267,814 |
| mssql | 31,269 (array 36,209) | 21,468 | 583,138 | 270,097 |
| postgres | 86,343 (array 151,542) | 9,506 | 235,955 | 151,745 |

All at main @ d364312, x64 Release, **single sample each**, servers as native Windows installs.
**Run-to-run variance on this machine swamps build-to-build comparison**: two postgres
fetches on the same build minutes apart read 187,893 and 235,955 rows/s (26% apart) — a
4-core mobile CPU with ~1.2 GB free, thermally limited, with the database server and Defender
on the same box. DuckDB's fetch read 839,721 rows/s on the pre-fix narrow path and 592,742 on
the wide one, which *looks* like a wide-path cost and cannot be distinguished from that noise;
a real answer needs a quiet machine and repeated runs. Earlier single-sample sqlite line at
199f40e: 175,704 / 153,763 / 457,935 / 256,221.

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

### Second bug, found by the fourth database: character columns were read through the ANSI code page

At 199f40e–9c07f78, against PostgreSQL 16.15 with `server_encoding`/`client_encoding` UTF8
(the server verifiably holding `68c3a96c6c6f` for `héllo`), `SELECT 'héllo'::varchar` raised
`UnicodeDecodeError: byte 0xe9` in pyarrow and `SELECT '日本語'::text` came back as `???`,
silently and irreversibly. The reader bound `SQL_CHAR`/`SQL_VARCHAR`/`SQL_LONGVARCHAR` as
`SQL_C_CHAR` on the assumption that the narrow path carries UTF-8 — true on unixODBC and
iODBC, never on the Windows driver manager, which transcodes narrow data to the ANSI code
page (1252 here). SQLite, SQL Server (`NVARCHAR` → wide path) and DuckDB had passed by luck
of the driver; psqlodbc, which fronts 14 of the 46, honours the conversion. Fixed in
`9c07f78`: on Windows every character column is read as `SQL_C_WCHAR`, the `wchar_as_utf8`
quirk (whose premise is the same assumption) is off there, and catalog string reads go the
same way. Verified at `d364312`: all four probes byte-exact (`68c3a96c6c6f`,
`e697a5e69cace8aa9e`), the four entries above PASS, `tests/test_windows_text.py` 9/9,
`ctest` 7/7, zero warnings. No truncation or doubling seen on these four drivers, whose
`column_size` is in characters; drivers that report bytes are the ones to watch next.

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | | | | |
| ingest | | | | |

Windows notes to watch for: the driver manager is not unixODBC, so anything
keyed on unixODBC's error text (`Can't open lib`) is Linux-only; `SQLLEN` is
64-bit on x64 Windows as on Linux; paths in connection strings need no escaping
but a `Driver=` value with spaces needs braces: `Driver={SQLite3 ODBC Driver}`.
