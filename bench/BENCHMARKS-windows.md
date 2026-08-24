<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — Windows

**Status: measured, one machine — nine databases so far (five native installs, then a
Docker Desktop tier running one container at a time) and the five-language grid.** Until 2026-08-24 the Windows build had never succeeded on any commit, and
CI was reporting that to nobody. The first person to build on Windows found ten defects across the repository — driver,
tests and benchmark harnesses — and all are fixed on main: four MSVC-only build breaks (the Windows SDK's `sqltypes.h` needs
`windows.h` first; `strndup` is not in the MSVC CRT; a same-type cast on `ADBC_ERROR_INIT`
only GCC and Clang tolerate; `odbc_bind.c` using pthreads without the `_WIN32` guard), **two
silent text corruptions in the driver** (statement text through the narrow ODBC entry points;
character columns read through the ANSI code page), one x86 Release inlining failure in a
test, and three harness defects (the Java classpath, the `python3` default, and the JVM
writing the results file in Cp1252 — the third silent corruption). Three of the ten were one
root cause in three disguises: a platform whose default narrow encoding is not UTF-8, met in
the ODBC data path, in ODBC statement text, and in the JVM's stdout.

Two things the Windows build does *not* have, which every number in this file must be read
against: the prefetch pipeline (`ADBC_ODBC_HAVE_PREFETCH`, compiled out on `_WIN32`) and the
parallel-ingest worker pool (`adbc.odbc.ingest_connections` is clamped to 1). So a Windows
row measures a materially different code path from the Linux rows, and a partitioned read on
a 4-core laptop is not a comparison at all. A Win32 port of both (SRWLOCK +
CONDITION_VARIABLE + `_beginthreadex`) is the first Windows roadmap item.

## Verified at the shipped state — main @ b5d2791

Clean build directory, x64 Release, MSVC 19.44.35228, CMake 4.4.2, Windows SDK 10.0.26100;
Windows 11 Pro build 26200, i7-8550U (4C/8T), 7.7 GB; Python 3.12.10 x64, adbc-driver-manager
1.12.0, pyarrow 25.0.1, pyodbc 5.3.0; drivers SQLite3 ODBC Driver, PostgreSQL Unicode(x64)
psqlodbc 18.00.0002, MySQL ODBC 8.4 Unicode Driver; servers native, not containers.

```
cmake --build          exit 0, zero warnings
ctest -C Release       100% tests passed out of 7
tests/test_windows_text.py   11 passed, 0 failed
tests/test_sqlite.py         exit 0 (MULTIROW INGEST OK)
compat sqlite    PASS  (SQLite (via ODBC) 3.43.2)
compat postgres  PASS  (PostgreSQL (via ODBC) 16.15.0)
compat mysql     PASS  (MySQL (via ODBC) 8.4.9)
```

## The Windows column, in two phases

**Phase 1 — native installs, five pass:** SQLite, DuckDB, SQL Server 2025, PostgreSQL 16,
MySQL 8.4. Blocked there: MariaDB (its Connector/ODBC MSI is a browser-only download),
Firebird (its security database needs an administrator to bootstrap), Access (32-bit ACE
drivers only, recorded above).

**Phase 2 — Docker Desktop on WSL2.** First declined (administrator rights and a reboot),
then done: `wsl --install` registered WSL but did not enable Virtual Machine Platform; a
second elevated `wsl --install --no-distribution` staged it, then a reboot. Docker Desktop
29.7.2 from winget, engine ready 5 s after launch; its VM disk cost ~6 GB before the first
image. The VM is capped in `.wslconfig` at 2560 MB / 2 CPUs / no swap so an over-size
container fails visibly instead of thrashing the host; containers run one at a time at
`--memory=1g`, each image deleted before the next pull (10–50 s to ready for the psqlodbc
tier). The 36 container entries are being worked through in tiers; anything that does not
fit is recorded as `server not runnable here: RAM` with the evidence, not skipped.

### Tier 3, batch 1 — psqlodbc "PostgreSQL Unicode(x64)" 18.00.0002, x64 Release at b5d2791

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| cockroachdb | PASS (`PostgreSQL (via ODBC) 18.0.0`), CockroachDB v26.3.0, `--max-go-memory=512MiB`, 235 MB | 155,144 | 109,120 | 16,816 (16,863) | 533 |
| timescaledb | PASS (`PostgreSQL (via ODBC) 16.15.0`), latest-pg16, hypertable steps | 216,602 | 132,486 | 87,048 (119,999) | 903 |
| citus | PASS (`PostgreSQL (via ODBC) 18.4.0`), PostgreSQL 18.4 + Citus 14.1.0, distributed-table steps | 266,622 | 145,459 | 132,876 (160,615) | 1,176 |
| cratedb | PASS (`PostgreSQL (via ODBC) 14.0.0`), CrateDB 6.4.3, 495 MB | 104,449 | 94,266 | 4,136 (5,633) | 108 |

Single samples, no prefetch, no fan-out. The five-language rows are in
[`LANGUAGE_BENCHMARKS-windows.md`](LANGUAGE_BENCHMARKS-windows.md); Rust's odbc-api
ingest is where the multi-row batching shows most on this tier — 7.4× on CockroachDB,
19.7× on TimescaleDB, 141.6× on CrateDB (57 rows/s row by row) — with fetch within 10%.

**A Windows-only environment fact, found by CrateDB:** Windows has no system time-zone
database, so pyarrow's timestamp-with-timezone conversion raises `ArrowInvalid: The
zoneinfo module or pytz package must be installed` until the `tzdata` PyPI package is
installed. It bit CrateDB first only because that was the first Windows entry whose
workload produces a tz-aware timestamp; any entry with a `timestamptz` column fails the
same way on a fresh box. The setup line above now includes `tzdata`.

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
py -3.12 -m pip install "adbc-driver-manager>=1.7" pyarrow pyodbc pytest tzdata   # tzdata: Windows has no system tz database
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
| mysql | PASS | `MySQL (via ODBC) 8.4.9` — winget's `Oracle.MySQL` binaries, `mysqld --initialize-insecure` and a standalone `mysqld` on a spare port (no service, no admin); MySQL Connector/ODBC 8.4.0. Byte-exact probe (validated against PostgreSQL first): `VARCHAR(50)` reports `column_size` 50, `TEXT` 65535, `héllo` and `日本語` round-trip exactly on read and on a bound-parameter write read back with pyodbc. Needs no `LD_PRELOAD`-style workaround, unlike the connector under pyarrow on Linux |
| postgres | PASS | `PostgreSQL (via ODBC) 16.15.0` — native install, psqlodbc 18.00.0002 "PostgreSQL Unicode(x64)"; **FAIL at 199f40e–9c07f78** with `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xe9`, see the second bug below |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | 209,082 (array binding 161,088) | 146,264 | 456,214 | 254,287 |
| duckdb | 74,005 (array 73,952) | 590 | 592,742 | 267,814 |
| mssql | 31,269 (array 36,209) | 21,468 | 583,138 | 270,097 |
| postgres | 86,343 (array 151,542) | 9,506 | 235,955 | 151,745 |
| mysql | 38,535 (array 42,144) | 6,145 | 397,443 | 269,100 |

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

### Five languages, five databases (`bench/*/run.sh`, ROWS=10000 FETCH_ROWS=100000 REPS=1)

**24 of 25 cells** on Windows: [`LANGUAGE_BENCHMARKS-windows.md`](LANGUAGE_BENCHMARKS-windows.md).
adbcbridge ingest / fetch rows/s, single samples on the 4-core laptop:

| database | python | go | rust | csharp | java |
|---|---:|---:|---:|---:|---:|
| sqlite | 161,088 / 456,214 | 235,899 / 456,975 | 222,957 / 429,941 | 238,109 / 427,553 | 157,071 / 408,076 |
| duckdb | 73,952 / 592,742 | 64,308 / 554,729 | abort (driver's C++ exception) | 61,744 / 498,014 | 56,400 / 517,045 |
| mssql | 36,209 / 583,138 | 56,362 / 652,627 | 46,963 / 745,551 | 60,126 / 701,911 | 34,237 / 573,710 |
| postgres | 151,542 / 235,955 | 214,632 / 440,254 | 164,864 / 429,818 | 225,977 / 445,502 | 114,210 / 387,586 |
| mysql | 42,144 / 397,443 | 37,381 / 467,658 | 42,398 / 489,120 | 41,557 / 492,964 | 30,400 / 444,488 |

Five languages land within a band on every server (fetch on SQL Server 573k–746k, on
MySQL 397k–493k), which is the point of the grid: the binary is the same, the language
binding is not the bottleneck. C# and Rust ran unmodified (Rust's first build: 4 min 22 s).
The Java runner's classpath fix (`cygpath -w`, `;`) is confirmed on all five databases.

The empty cells and three findings from the grid, none of them the bridge's — the
language file spells each out: Go's `alexbrainman/odbc` access-violates inside
`SQLGetDiagRec` on Windows on every server but SQLite, so four Go rows are `-no-native`;
the DuckDB ODBC driver throws a C++ exception through Rust's FFI and the process aborts;
and a Windows JVM wrote the bench's em-dash placeholder as one Cp1252 byte, corrupting the
file's UTF-8 — `bench/java/run.sh` now pins `-Dstdout.encoding=UTF-8 -Dfile.encoding=UTF-8`.

Three harness defects the first Windows run found, all fixed on main:

1. `bench/java/run.sh` joined the classpath with `:` and a Git-Bash `/c/...` prefix, which
   a Windows JVM cannot read — `ClassNotFoundException: org.adbcbridge.bench.Bench`, which
   looks like a build failure. Maven's own `target/classpath.txt` was already right (`C:\`
   paths, `;`); the runner now spells the prefix the same way. `JAVA_HOME` alone is not
   enough: `java` must be on `PATH` or `JAVA` set.
2. Go: ADBC's `drivermgr` is cgo-only and MSVC cannot serve cgo. Without a GCC Go sets
   `CGO_ENABLED=0` and the build fails with `undefined: drivermgr.Driver`, which reads as
   a version mismatch. mingw-w64 (WinLibs GCC 16.1.0) and `CGO_ENABLED=1` fix it; the
   `dllexport attribute ignored` warnings from `adbc.h` under GCC are harmless noise.
3. Every runner defaulted to `python3`, which Windows does not install; they fall back to
   `python` now.

### Microsoft Access, Excel and Text drivers: 32-bit Office, 64-bit process

The ACE drivers on this machine (Access, Excel, Text) are 32-bit — they arrive with a
32-bit Office install — and a 64-bit process cannot load them. Microsoft's 64-bit Access
Database Engine redistributable refuses to install alongside 32-bit Office, so the only
route to those three from x64 adbcbridge is to replace Office, and the alternative — a
32-bit adbcbridge build with a 32-bit Python — is out of scope: the project targets 64-bit
systems. Recorded as *driver unavailable on x64* for `access`, and the two Windows-only
entries that were on the list (the Excel driver, the Text driver for CSV) are unreachable
for the same reason. Anyone with 32-bit Office and a 64-bit client hits exactly this, which
is a common configuration; it is a platform constraint, not a coverage gap.

The Win32 *build* is still verified: `ctest` 7/7 in Debug, 6/7 in Release (the `/Ob2`
inlining case recorded above), so the x86 evidence is about the code, with no database row
behind it.

### Getting servers and drivers onto Windows

Not "run the installer": what actually worked on this box, 2026-08-24.

- **Every ODBC driver MSI needs an interactive UAC accept** (silent install fails with
  `Error 1925`), one round trip per driver — the real rate limiter here, not the 8 GB.
- **MySQL**: winget's `Oracle.MySQL` is binaries only (no service, datadir or config);
  `mysqld --initialize-insecure` and a standalone `mysqld` on a spare port need no admin.
  **Connector/ODBC 9.x is not downloadable** — every documented 9.x URL on dev.mysql.com and
  cdn.mysql.com returns 404; **8.4.0 from cdn.mysql.com** is what exists.
- **MariaDB**: the server is the good citizen (`winget install MariaDB.Server --custom
  "PORT=13307 PASSWORD=adbc"` gives a configured, running service), the **Connector/ODBC
  MSI is not scriptably downloadable** (not in winget, not on archive.mariadb.org or the
  mirrors — a JavaScript download page only), so a person has to fetch it.
- **Firebird**: server 5.0.4 zip and ODBC driver 3.5.0-rc1 win-x64 MSI are both on GitHub;
  the server runs standalone from the zip on a spare port without admin, but its sample
  security database ships with no SYSDBA, and creating one through the embedded engine as a
  non-administrator fails (`no permission for INSERT access to TABLE PLG$SRP_VIEW`). Recorded
  as *server not runnable here*; the driver installed fine.

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | | | | |
| ingest | | | | |

Windows notes to watch for: the driver manager is not unixODBC, so anything
keyed on unixODBC's error text (`Can't open lib`) is Linux-only; `SQLLEN` is
64-bit on x64 Windows as on Linux; paths in connection strings need no escaping
but a `Driver=` value with spaces needs braces: `Driver={SQLite3 ODBC Driver}`.
