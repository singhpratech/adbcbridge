<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — macOS

Measured 2026-08-24 on a real machine (first non-CI macOS run), commit `e4eb3dd`.
SQLite, PostgreSQL and SQL Server all measured. **The PostgreSQL numbers are indicative only,
not the reference comparison**: psqlodbc was built from source, the server is PostgreSQL 15 (not
16), and the host was not idle — see the Load row. The Linux run in `BENCHMARKS.md` stays the
number to quote — but read the PostgreSQL table below before quoting anything: on this machine
the native driver is faster than the bridge at every setting tried.

## Host

| | |
|---|---|
| OS | macOS 26.5.2 (build 25F84), Apple Silicon (arm64) |
| CPU / RAM | Apple M4 Max, 16 cores / 64 GB |
| Driver manager | unixODBC 2.3.12, built from source (`SQLLEN` size 8). Homebrew's prefix on this box belongs to another account, so `brew install unixodbc sqliteodbc` could not write; the same versions were configured with `--prefix` into a scratch dir and passed to CMake via `-DCMAKE_PREFIX_PATH` |
| Build | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<unixODBC prefix>` && `cmake --build build -j` — AppleClang 21.0.0 (Xcode CLT 2100.1.1.101), CMake 4.4.2; no compiler warnings |
| Python | 3.12.12 (uv-managed venv), `adbc-driver-manager` 1.12.0, `pyarrow` 25.0.1, `pyodbc` 5.3.0 (built from source against the same unixODBC) |
| ODBC drivers | sqliteodbc 0.99991 (from source, arm64, linked to the system `/usr/lib/libsqlite3.dylib` = SQLite 3.51.0); psqlodbc 18.00.0002 (from the `REL-18_00_0002` GitHub tag, bootstrapped with m4 1.4.19 / autoconf 2.72 / automake 1.17 / libtool 2.5.4 built from source, linked to Homebrew `postgresql@15`'s libpq 5.15); msodbcsql 18.6.2.1 arm64 (Microsoft's `msodbcsql18-18.6.2.1-arm64.tar.gz`, `install_name_tool`-relinked from `/opt/homebrew/lib/libodbcinst.2.dylib` to the scratch unixODBC and ad-hoc re-signed) |
| Servers | PostgreSQL 15.15 (Homebrew `postgresql@15` binaries, scratch `initdb` cluster on 127.0.0.1:15432, `shared_buffers=1GB`, fsync on, Unix sockets off — the scratch path exceeded macOS's 103-byte socket limit); SQL Server 2022 16.00.4265 (`mcr.microsoft.com/mssql/server:2022-latest`, amd64 image under Docker Desktop 29.1.3 emulation, VM 16 CPU / 7.65 GiB, port 14331) |
| Load | not idle: SQLite `matrix_bench` at load 4.48 / 4.22 / 2.93 → 4.28 / 4.19 / 2.92 (~40 GB free+inactive); PostgreSQL `matrix_bench` + `native_threshold` at 2.49 / 4.72 / 4.10 → 2.57 / 4.67 / 4.09 (29.4 → 28.3 GB free+inactive), SQL Server container stopped first |

## Setup

```sh
# (brew install cmake unixodbc sqliteodbc on a normal box; here both were built from source into $PREFIX)
git clone https://github.com/singhpratech/adbcbridge && cd adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build build -j
cmake --install build --prefix "$PWD/dist"
ctest --test-dir build --output-on-failure
uv venv --python 3.12 .venv && . .venv/bin/activate
pip install "adbc-driver-manager>=1.7" pyarrow pyodbc pytest
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.dylib
export SQLITE_ODBC_DRIVER=$PREFIX/lib/libsqlite3odbc.dylib
python tests/test_sqlite.py
python tests/compat/test_matrix.py sqlite
python bench/matrix_bench.py --rows 10000 --fetch-rows 100000 sqlite
python tests/test_plug_and_play.py
```

## Results

### Compat workload

| database | result | vendor string |
|---|---|---|
| sqlite | PASS | `SQLite (via ODBC) 3.51.0` |
| postgres | PASS | `PostgreSQL (via ODBC) 15.15.0` |
| mssql | PASS | `Microsoft SQL Server (via ODBC) 16.00.4265` |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | 1,840,039 (array binding 1,970,233) | 785,186 | 2,335,766 | 1,224,857 |
| postgres | 1,031,335 (array binding 1,081,442) | 42,134 | 1,259,979 | 707,592 |

Raw lines from the harness:
`sqlite    SQLite (via ODBC) 3.51.0       fetch=2,335,766/s (pyodbc 1,224,857/s, native —/s)  ingest=1,840,039/s array=1,970,233/s pyodbc=785,186/s`
`postgres  PostgreSQL (via ODBC) 15.15.0  fetch=1,259,979/s (pyodbc 707,592/s, native 6,372,133/s)  ingest=1,031,335/s array=1,081,442/s pyodbc=42,134/s`
(sqlite fetch 1.9× pyodbc, native delegation not measured — `adbc_driver_sqlite` not installed;
postgres fetch 1.8× pyodbc, ingest 24× pyodbc, native delegation 6.37 M rows/s.)

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | 0.314 s (0.311 / 0.312 / 0.320) | 0.189 s (0.198 / 0.183 / 0.185) | **0.60×** — FAIL vs 1.2× threshold | 2.49 4.72 4.10 / 2.57 4.67 4.09 |
| ingest | 0.540 s (0.596 / 0.520 / 0.504) | 0.268 s (0.262 / 0.262 / 0.280) | **0.50×** — FAIL vs 1.2× threshold | 29.4 GB → 28.3 GB free+inactive |

Run with `--pg postgresql://adbc:adbc@127.0.0.1:15432/adbc --ingest-connections 8`; 8 partitions
returned for 8 requested, `adbc.odbc.ingest_connections` accepted (8). Both axes **correct**
(fetch checksum against the reference, ingest checksummed in SQL on the server) and both
**slower than the native driver** here. Indicative only — from-source psqlodbc, PG 15, host not
idle, PostgreSQL and the client on the same 16-core machine — do not quote against the Linux
reference. On this box `adbc_driver_postgresql` reads 1 M rows in 0.19 s (6.4 M rows/s) so the
"beat native by splitting across connections" claim did not hold at 8 partitions.

## Test runs

### `ctest --test-dir build --output-on-failure` — 8/8 passed (1.27 s)

`adbc_odbc_c_smoke`, `test_utf16`, `test_types`, `test_sqllen32`, `test_objects`,
`test_errors`, `test_multirow`, `test_partition`.
Not built on macOS by design (`if(... AND NOT APPLE)`): `adbc_odbc_tls_dep`, `adbc_odbc_tls_user`
— there is no ctest entry for them anywhere, so nothing shows as "skipped"; the Python side
(`tests/test_driver_load_errors.py`) reports `SKIP: the static-TLS fixture is not built`.

### `tests/test_sqlite.py` — OK

All phases printed: ingest (3 rows, full type round-trip incl. binary/date/timestamp/bool),
create-twice → ProgrammingError, GetObjects (catalogs/tables/columns/PK+FK constraints, filter),
`PHASE2 OK`, `TYPES OK` (time32[s], decimal→string, emoji/UTF-16 round-trip, ±inf doubles),
`BIND TYPES OK`, `METADATA OK`, `BOUND PARAMS OK`, `RELEASE WITH OPEN TXN OK`, `MULTIROW INGEST OK`.

### `tests/compat/test_matrix.py sqlite` — `sqlite    PASS  (SQLite (via ODBC) 3.51.0)`

### `tests/test_plug_and_play.py` — 1 of 2 flows passed at `e4eb3dd`; the second was Linux-specific as written

* `install --prefix + ADBC_DRIVER_PATH: CONNECTED ADBC ODBC Driver` — PASS.
* `install.sh + user config dir` — FAIL on macOS at `e4eb3dd`, **test-harness issue, not a driver
  issue**: `test_install_sh_flow` set `MANIFEST_DIR=$tmp/xdg/adbc/drivers` and then expected the
  driver manager to find it via `XDG_CONFIG_HOME=$tmp/xdg`. On macOS the driver manager ignores XDG
  and searches only `~/Library/Application Support/ADBC/Drivers` (its error listed
  `does not exist: /Users/<me>/Library/Application Support/ADBC/Drivers`). Fixed after this run:
  on Darwin the test now installs under `$tmp/home/Library/Application Support/ADBC/Drivers` and
  runs the child with `HOME=$tmp/home`.
* The real flow, run by hand, **works**: `./install.sh` (with `CMAKE_PREFIX_PATH` exported) wrote
  `~/.local/lib/libadbc_driver_odbc.dylib` and `~/Library/Application Support/ADBC/Drivers/odbc.toml`
  (`macos_arm64 = '/Users/<me>/.local/lib/libadbc_driver_odbc.dylib'`), and with nothing set in the
  environment `dbapi.connect(driver="odbc", ...)` printed `CONNECTED ADBC ODBC Driver`.
  `install.sh` needs `cmake` on PATH and, on a non-Homebrew unixODBC, `CMAKE_PREFIX_PATH` in the
  environment — it has no flag for that.

#### Loop closed at `96a9867` (same machine, rebuilt clean, 0 warnings)

* `tests/test_plug_and_play.py` → `PLUG AND PLAY OK`, both flows, negative control and idempotence
  included; nothing left behind in the real `~/.local/lib` or `~/Library/Application Support`.
* `python -m pytest tests/test_delegate.py -q` (SQLite + a fresh PG 15 cluster + psqlodbc) →
  **27 passed, 3 skipped** (was 8 / 22); the skips are `adbc_driver_sqlite` not installed ×2 and
  no `MARIADB_ODBC_DRIVER`.
* x86_64-only dylib through `dlopen()` → the explanation is now 818 bytes and ends exactly after the
  first reason (`... (have 'x86_64', need 'arm64e' or 'arm64e.v1' or 'arm64' or 'arm64')). SQLSTATE: 01000`);
  no Cryptexes tail, no truncation.

## Bonus: other Python tests

| test | result |
|---|---|
| `test_driver_load_errors.py` | 3 ok (`missing_driver_library_is_still_reported_as_missing`, `unreadable_driver_library_says_permission_denied`, `static_tls_exhaustion_is_explained`→SKIP fixture not built, as designed) |
| `test_prefetch.py` | 24 passed, 3 skipped (need PG_URI) |
| `test_delegate.py` | 8 PASS, 22 SKIP at `e4eb3dd` — **harness bug, fixed after this run**: the file hard-coded `build/libadbc_fake_native_driver.so`; the target is `.dylib` here, so every delegation test skipped. It now uses the platform's suffix |
| `test_partitions.py` (with PG) | 85 passed |
| `test_long_columns.py` (with PG) | 13 passed |
| `test_parallel_ingest.py` (with PG) | 18 passed |
| `test_prefetch.py` (with PG) | 27 passed, 0 skipped |
| `test_pg_array_ingest.py` (`python -m pytest`, with PG) | 18 passed |

## macOS-specific checks

1. **Linkage** — `otool -L build/libadbc_driver_odbc.dylib`:
   `@rpath/libadbc_driver_odbc.dylib`, `<prefix>/lib/libodbc.2.dylib` (unixODBC 2.3.12, compat 3.0.0),
   `/usr/lib/libSystem.B.dylib`. **Not** iODBC. (macOS 26.5 no longer ships `/usr/lib/libiodbc*` at all.)
   `file`: `Mach-O 64-bit dynamically linked shared library arm64`.
2. **install_name** — `LC_ID_DYLIB name @rpath/libadbc_driver_odbc.dylib`; no `LC_RPATH` entries.
   Harmless because both the manifest and `ADBC_ODBC_DRIVER` use absolute paths; the installed
   copy in `dist/lib` loaded through `ADBC_DRIVER_PATH=$PWD/dist/etc/adbc/drivers`:
   `OK ADBC ODBC Driver 0.1.0 SQLite (via ODBC) 3.51.0` and `SELECT 42` returned `int32 [[42]]`.
3. **Manifest** — `dist/etc/adbc/drivers/odbc.toml` keyed `macos_arm64` with the absolute
   `dist/lib/...dylib` path; `install.sh` writes the same shape into the user config dir.
4. **Gatekeeper / quarantine** — nothing needed: every dylib was compiled locally (no
   `com.apple.quarantine` attribute). Not exercised for a downloaded driver zip.
5. **"Can't open lib" augmentation** (`src/odbc_reader.c`, `OdbcExplainLoadFailure`) works on
   macOS + unixODBC, all three branches:
   * missing path →
     `[unixODBC][Driver Manager]Can't open lib '/nonexistent/libsqlite3odbc.dylib' : file not found`
     `[adbcbridge] /nonexistent/libsqlite3odbc.dylib: No such file or directory`
   * x86_64-only dylib (built with `cc -arch x86_64 -shared`) →
     `[adbcbridge] the file is there and readable -- the driver manager says "file not found" for any load failure.  dlopen(): dlopen(<path>, 0x0005): tried: '<path>' (mach-o file, but is an incompatible architecture (have 'x86_64', need 'arm64e' or 'arm64e.v1' or 'arm64' or 'arm64')), '/System/Volumes/Preboot/Cryptexes/OS<path>' (no such file), '<scratch>. SQLSTATE: 01000`
   * non-Mach-O file (README.md) → `... dlopen(): dlopen(<path>, 0x0005): tried: '<path>' (slice is not valid mach-o file), ...`
   **Finding, fixed after this run:** the augmented message was truncated at 1024 bytes
   (`kErrorBufferSize`, `src/utils.c`). dyld's `dlerror()` enumerates every path it tried (incl. the
   Cryptexes mirror), so on macOS the tail — and with a longer path, the actual reason — fell off;
   the arch reason survived here only because it comes first. The explanation now keeps dyld's
   first entry (the file named, with its reason) and drops the rest of the list.
6. **arm64** — all drivers arm64 (`file` on libodbc.2.dylib, libsqlite3odbc.dylib, the bridge).
   A Rosetta/x86_64 driver produces the "incompatible architecture" text above, i.e. it is
   *not* mistaken for a missing file on macOS as long as the augmentation is present.
7. **Static-TLS test** — Linux-only by design; not built, nothing ran.

## Other notes

* `bench/native_threshold.py` defaulted `PG_URI` to port **15482** while `tests/compat/test_matrix.py`
  uses **15432** for postgres; fixed after this run (both 15432).
* msodbcsql 18 arm64 from Microsoft's tarball links `/opt/homebrew/lib/libodbcinst.2.dylib` by
  absolute path; without Homebrew's unixODBC it needs `install_name_tool -change` + `codesign -f -s -`.
  No quarantine attribute on the tarball contents (only `com.apple.provenance`), so no `xattr -d` needed.
* SQL Server 2022 (amd64) runs fine under Docker Desktop's emulation on Apple Silicon: ready in
  ~10 s, compat workload passes.
* `bench/matrix_bench.py` rewrote `bench/MATRIX_BENCHMARKS.md` in place; with only `sqlite`
  passed it replaced the 13-database Linux table with a single sqlite row (reverted on the Mac;
  fixed after this run — a subset run now merges into the existing table).
* `sqliteodbc-0.99991`'s bundled `config.guess`/`config.sub` do not know `arm64-apple-darwin25`
  (`configure: error: ./config.sub -apple-darwin25.5.0 failed`); copying the ones from
  unixODBC 2.3.12 fixes it. Worth a line in docs if from-source is ever documented.
* The pyodbc macOS wheel is linked against `/opt/homebrew/opt/unixodbc/lib/libodbc.2.dylib`
  (absolute path); without Homebrew's unixODBC it must be built from source with
  `odbc_config` on PATH.

## Second pass: the two drivers Linux had to build from source (main @ 18e1a8d)

* **TDengine — PASS** with the vendor's own arm64 client (`TDengine-client-3.3.6.13-macOS-arm64.pkg`)
  and `taos-connector-odbc` branch 3.3.6 built against it: `tdengine PASS (tdengine (via ODBC)
  03.03.0613 … build:Linux-arm64 …)`; `matrix_bench` fetch 552,492 rows/s; Rust ADBC fetch 507,308
  rows/s on the 20,000-row fixture, while `odbc-api` and `arrow-odbc` both fail in `SQLBindCol`
  (`Column conversion to 'SQL_C_TYPE_TIMESTAMP' not implemented yet`) — the bridge is the only
  Rust path that reads TDengine at all. Load 3.3–6.5, not idle. Root-free workarounds the run
  needed: the pkg's dylibs carry a bogus `install_name` (`install_name_tool -id` + ad-hoc
  `codesign`); `libtaos` `dlopen()`s `libtaosnative.dylib` by bare name and SIP strips
  `DYLD_LIBRARY_PATH` from anything spawned by `/bin/bash`, so `bench/rust/run.sh` had to run
  with the client-lib directory as cwd; `TAOS_LOG_DIR`/`TAOS_CONFIG_DIR` in the environment;
  bison ≥ 3 and pkg-config for the build; and one **local** patch to `taos-odbc`
  (`src/core/env.c`, `_env_set_odbc_version`): for `SQL_OV_ODBC3_80` — what `odbc-api` asks for —
  it builds the 01S02 "substituted" record and then aborts the process with `OA_NIY(0)`; with
  that call removed it returns `SQL_SUCCESS_WITH_INFO` as written. On Linux the harness avoids
  the abort by not touching `odbc-api` for this entry (`ADBC_BENCH_NO_NATIVE`). The Rust bench
  also needs `DB=adbc` in `TDENGINE_CONN`.
* **OpenSearch — recorded, not run.** The project's only macOS artifact
  (`OpenSearch-SQL-ODBC-Driver-64-bit-1.5.0.0-Darwin.pkg`) is a single-arch **x86_64** dylib
  linked against the system iODBC; in an arm64 process `dlopen()` answers `incompatible
  architecture (have 'x86_64', need 'arm64e' or 'arm64e.v1' or 'arm64' or 'arm64')`. It loads
  under Rosetta (`arch -x86_64 python3`), which an Intel-only stack could use; this project does
  not target Intel Macs, so the entry stays Linux-only (where it is built from source).

## Full-matrix campaign (main @ 688229f) — tier 1, no server

| entry | result | matrix_bench (10,000 / 100,000 rows) |
|---|---|---|
| sqlite | PASS (SQLite 3.51.0) | as above |
| duckdb | PASS (`DuckDB (via ODBC)`, version string empty) — `duckdb_odbc-osx-universal.zip` 1.5.5.0, arm64+x86_64 fat, no external deps, no quarantine attribute | fetch 4,238,635/s (pyodbc 1,299,832), ingest 390,288/s (array 385,861; pyodbc 12,280) — on a **file-backed** database: with the entry's `Database=:memory:` every connection is its own empty DuckDB and the fetch connection finds no table |
| access | PASS (MDBTOOLS 1.0.1), both `libmdbodbcW` and `libmdbodbc` — mdbtools 1.0.1 built from the release tarball with `--with-unixodbc` (bison ≥ 3 and flex; Apple's bison 2.3 is rejected) | fetch 2,205,341/s on the 3,000-row fixture (timer resolution, not a rate); read-only |

**mdbtools 1.0.1 needed one local patch, and it is an upstream bug**: built without real glib it
uses `src/libmdb/fakeglib.c`, whose `g_strsplit()` advances `haystack` in its counting loop and
never resets it, so the split loop starts at end-of-string and `components[0]` is `""`.
`ExtractDBQ()` then hands the driver an empty path and every `DBQ=` connection fails with
"Unable to locate database" (a DSN with `Database=` takes another code path and works).
Reproduced in a ten-line harness; present on upstream `dev` at the time of writing; the fix is
to count on a local copy of the pointer. Ubuntu's `odbc-mdbtools` links real glib, which is
why Linux never sees it. Load 5–6 throughout.

## Full-matrix campaign — batch 1: tiers 1–2 (main @ 688229f)

| entry | result |
|---|---|
| azuresqledge | PASS (`Microsoft SQL Server (via ODBC) 15.00.2000`) — arm64 image, native; msodbcsql 18.6.2.1 arm64 |
| mysql | **FAIL** `AssertionError: (2, 2)` — `adbc_ingest` of the 4-row table reports `rows_affected` 2 on create and on append. Driver: MariaDB Connector/ODBC 3.2.9 + Connector/C 3.4.9 (Homebrew, relinked), `PLUGIN_DIR` set for `caching_sha2_password`; MySQL's own Connector/ODBC for macOS has no non-interactive download. Server `mysql:8` = 8.4.11 arm64. Whether two or four rows landed is the open question |
| mariadb | **FAIL** segmentation fault in `executemany`: `libmariadb.3.dylib store_param+128`, `EXC_BAD_ACCESS addr 0x1d` — a NULL `DATE` inside a parameter array on the bulk path. 100% reproducible with `INSERT (d DATE)` rows `[(date,), (None,)]`; a non-NULL date, a NULL `TIMESTAMP`, pyodbc row-at-a-time, and the same driver against MySQL 8 (no bulk protocol) are all fine. Server `mariadb:11` = 11.08 arm64. A Connector/C bug; every MariaDB-server target (mariadb, columnstore) will hit it in this workload |
| clickhouse | PASS (`ClickHouse (via ODBC) 26.7.5.10`) — clickhouse-odbc 1.5.5 macOS zip, arm64, self-contained; server arm64 |
| oracle | PASS (`Oracle (via ODBC) 23.26.0200`) — **only with `NLS_LANG=.AL32UTF8` exported before the process starts**; with the entry's in-process `unicode_env` alone the Unicode literal step fails (`hello ?`). Instant Client 23.3 arm64 dmgs, `@rpath/libodbcinst` relinked. `gvenzl/oracle-free:slim` is arm64 native |
| db2 | PASS (`DB2/LINUXX8664 (via ODBC) 12.01.0500`) — IBM's `macarm64_odbc_cli.tar.gz` (libdb2.dylib arm64); server amd64 emulated, `--privileged --memory=3g` |
| informix | PASS (`IDS/UNIX64 (via ODBC) 12.10.0000`) — same arm64 clidriver; server amd64 emulated, unprivileged, `GL_USEGLU=1` |
| monetdb | PASS (`MonetDB (via ODBC) 11.55.0007`) — `libMonetODBC` built from the MonetDB 11.55 source (`cmake --target MonetODBC`; needs pkgconf and bison ≥ 3); Homebrew's bottle has no ODBC driver; server amd64 emulated |
| firebird | driver unavailable on macOS arm64: firebird-odbc-driver v3-0-1 ships `linux_libs`, `linux_arm64_libs`, `win_installers`, `win_arm64_installers` only |
| vertica | driver unavailable on macOS arm64: the macOS download is `vsql-*.mac.dmg` only, no ODBC |
| virtuoso | **FAIL** the process aborts (SIGABRT, no message, stack not unwindable) inside `SQLExecDirect` at the workload's first failing statement (`DROP TABLE` of a missing table). Same with unixODBC's own `isql -k` and with raw ctypes `SQLExecDirect`/`SQLExecDirectW` on an ANSI-connected handle; pyodbc (W connect + `wideAsUTF16=Y`) gets the proper `42S02 SR268: No table in drop table` and survives; an ASan build of the bridge sees nothing (the smash is in uninstrumented driver code). Connect and `SELECT` work. Driver `virtodbcu_r.so` from Homebrew virtuoso 7.2.17 (arm64, openssl@3 relinked); server arm64 |
| flightsql | **FAIL** the same abort class — SIGABRT inside `SQLExecDirect(W)` on the first failing statement, from the bridge, from `isql`, from raw ODBC (ANSI or UTF-16 connect) *and* from pyodbc (which dies even earlier). The driver needs `arrow-odbc.ini` beside the dylib (the pkg ships `.orig`). Arrow Flight SQL ODBC 0.9.7 armv8 dmg; sqlflite server arm64 |
| influxdb3 | **FAIL** identical (same driver); `load_influxdb3.py` wrote 100,002 points fine; server arm64 |
| dremio | **FAIL** identical (same driver); first-user bootstrap fine; dremio-oss (5 GB) run alone |
| ignite | driver unavailable on macOS arm64: `platforms/cpp` carries only `common/os/{linux,win}`; the Darwin build stops at `concurrent_os.cpp:18 fatal error: 'sys/sysinfo.h' file not found` |

`matrix_bench.py --rows 10000 --fetch-rows 100000` (clickhouse at 300 / 2,000 as on Linux; DuckDB on a file-backed database):

```
duckdb       DuckDB (via ODBC)                          fetch=4,238,635/s (pyodbc 1,299,832/s)  ingest=390,288/s array=385,861/s pyodbc=12,280/s
access       MDBTOOLS (via ODBC) 1.0.1                  fetch=2,205,341/s                       ingest=— (read-only)
azuresqledge Microsoft SQL Server (via ODBC) 15.00.2000 fetch=590,525/s (pyodbc 555,073/s)      ingest=42,577/s array=88,175/s pyodbc=61,371/s
clickhouse   ClickHouse (via ODBC) 26.7.5.10            fetch=727,548/s (pyodbc 487,468/s)      ingest=998/s array=958/s pyodbc=16/s
oracle       Oracle (via ODBC) 23.26.0200               fetch=78,908/s (pyodbc 55,935/s)        ingest=28,407/s array=30,251/s pyodbc=1,178/s
monetdb      MonetDB (via ODBC) 11.55.0007              fetch=598,932/s (pyodbc 560,305/s)      ingest=114,534/s array=148,798/s pyodbc=1,250/s
db2          DB2/LINUXX8664 (via ODBC) 12.01.0500       fetch=582,691/s (pyodbc 589,701/s)      ingest=82,281/s array=129,373/s pyodbc=4,869/s
informix     IDS/UNIX64 (via ODBC) 12.10.0000           fetch=562,870/s (pyodbc 385,082/s)      ingest=3,702/s array=35,174/s pyodbc=4,334/s
```
Load at start: duckdb/access ~5, azuresqledge 4.8, clickhouse 2.4, oracle 6.5, monetdb 10.5 (emulation), db2 ~3, informix ~3.9. Never idle.
The five-language rows are in [`LANGUAGE_BENCHMARKS-macos.md`](LANGUAGE_BENCHMARKS-macos.md).

Findings from this batch, for the docs: (1) the MariaDB Connector/C bulk-path NULL-`DATE` crash; (2) Oracle's `NLS_LANG` must precede `libsqora` loading; (3) Arrow Flight SQL ODBC 0.9.7 and Virtuoso 7.2.17 abort the process on the first statement error under unixODBC 2.3.12 on macOS 26 — four entries fail for that one reason, and it is not the bridge (isql and raw ODBC die identically); (4) IBM's arm64 clidriver makes Db2 and Informix first-class on Apple Silicon; (5) DuckDB's driver lets a C++ exception escape into Rust and faults Go's binding; (6) Go on macOS needs `CGO_CFLAGS`/`CGO_LDFLAGS` for unixODBC (the module hard-codes `/usr/local/opt/unixodbc`).

## Batch 2 (in progress): tier 3, and the two connector follow-ups

Docker Desktop's VM was raised from 7.65 GiB to 16 GiB for the heavy images (settings backed
up, to be restored). Load 5–7 throughout.

| entry | result |
|---|---|
| cockroachdb | PASS (`PostgreSQL (via ODBC) 18.0.0`), arm64 — python fetch 799,169/s (pyodbc 497,329), ingest 75,825 (array 61,278; pyodbc 2,426) |
| yugabyte | PASS (`PostgreSQL (via ODBC) 15.12.0`), arm64 — python fetch 858,915/s (pyodbc 496,826), ingest 36,527 (array 35,513; pyodbc 1,801) |
| citus | PASS (`PostgreSQL (via ODBC) 18.4.0`), amd64 emulated — python fetch 1,008,472/s (pyodbc 575,339), ingest 193,490 (array 233,103; pyodbc 4,202) |
| timescaledb | first run skipped by a variable-name slip (`TIMESCALE_ODBC_DRIVER`, not `TIMESCALEDB_`); re-running with cratedb, questdb, cloudberry, opengauss |

**The two MariaDB Connector/ODBC failures from batch 1, resolved to driver quirks** (driver
`libmaodbc.so`, `SQL_DRIVER_VER` 03.02.0009, Connector/C 3.4.9):

* **mysql** is a row-count quirk, not data loss: the 4-row `adbc_ingest` reports
  `rows_affected` 2 on create and on append, and `SELECT COUNT(*)` then says 8 — every row
  landed. With `adbc.odbc.array_binding=false` the same ingest reports (4, 4). The connector
  misreports the count only on the parameter-array path against MySQL.
* **mariadb** is parameter arrays only: `executemany` of `INSERT INTO t (d DATE) VALUES (?)`
  with rows `[(date,), (None,)]` segfaults in `libmariadb.3.dylib store_param+128` with arrays
  on and passes with them off (both rows read back), for the single column and for the full
  8-column compat row. `adbc_ingest` never crashed because its multi-row `INSERT … VALUES (…),(…)`
  binds scalars and uses no array. Against MySQL 8.4 (no bulk protocol in the connector) the
  same `executemany` does not crash.

Driver fix: `no_param_arrays` keyed on MariaDB Connector/ODBC ≥ 3.2 (the Linux matrix runs
3.1.15, where arrays are both correct and the faster path); both entries to be re-run.
| mysql (re-run at 34b5863) | PASS (`MySQL (via ODBC) 08.04.000011`) — python fetch 48,715/s (pyodbc 45,680), ingest 119,319 (array 130,556; pyodbc 4,626) |
| mariadb (re-run at 34b5863) | PASS (`MariaDB (via ODBC) 11.08.000008`) — python fetch 46,926/s (pyodbc 42,450), ingest 157,034 (array 142,486; pyodbc 4,219). The ~47k fetch on both MySQL-wire servers against ~800k on the PostgreSQL-wire ones is libmaodbc 3.2.9's read path, not the bridge — pyodbc gets the same |
| timescaledb | PASS (`PostgreSQL (via ODBC) 16.15.0`) |
| questdb | PASS (`PostgreSQL (via ODBC) 11.3.0`) |
| cloudberry | PASS (`PostgreSQL (via ODBC) 14.4.0`), amd64 emulated |
| cratedb | FAIL at 34b5863: `AssertionError: decimal128(28, 3)` — psqlodbc 18 describes a NUMERIC whose precision and scale CrateDB does not report as `decimal128(28, 3)`, where psqlodbc 16 on Linux says `decimal128(28, 6)`; the entry now accepts both, re-run pending |
| opengauss | server not runnable here: `enmotech/opengauss` arm64's MOT engine panics at start inside the Docker Desktop VM — `Libnuma library not installed or not configured`, `Invalid NUMA configuration numa_node_of_cpu(0) => -1`, `Failed to allocate highest thread identifier on node 0` → `PANIC: Failed to Initialize core services`; tried `--cap-add=SYS_NICE --shm-size=1g` and `--cpuset-cpus=0-7` |
| cratedb (re-run at 1f35a5c) | PASS (`PostgreSQL (via ODBC) 14.0.0`, CrateDB 6.4.3) — python fetch 454,878/s (pyodbc 352,906), ingest 24,740 (array 29,214; pyodbc 516) |
| timescaledb | python fetch 1,074,095/s (pyodbc 581,495), ingest 533,002 (array 508,477; pyodbc 4,370) |
| questdb | python fetch 545,566/s (pyodbc 373,440), ingest 112,365 (array 138,077; pyodbc 4,802); languages with `ADBC_BENCH_AUTOCOMMIT=1` |
| cloudberry | arm64 image, singlenode, `--shm-size=1g` — python fetch 1,029,974/s (pyodbc 578,469), ingest 12,327 (array 12,121; pyodbc 1,013) |
| risingwave | PASS (`PostgreSQL (via ODBC) 13.1400.0`) — Docker Desktop refused the compose bind-mount of `tests/compat/risingwave.toml` from `~/Documents` for this account (`error while creating mount source path`), so the README's `docker run` with the toml under `/private/tmp` was used (the same will apply to `columnstore.cnf` and `mongodbbi.drdl`); python fetch 735,801/s (pyodbc 472,795), ingest 23,843 (array 16,329; pyodbc 1,051) |
| materialize | PASS (`PostgreSQL (via ODBC) 9.5.0`) — python fetch 164,748/s (pyodbc 101,211), ingest 32,833 (array 27,349; pyodbc 2,423); languages with `ADBC_BENCH_AUTOCOMMIT=1` |
| ydb | **FAIL** `[HY000] (110) Status: GENERIC_ERROR Issues: <main>:1:1: Error: unrecognized configuration parameter "datestyle"` at `SQLDriverConnect` — psqlodbc 18.00.0002's `CC_connect` sends `SHOW DateStyle;` (connection.c:1109) before anything else and YDB's PG layer rejects it; psqlodbc 16 (Linux) does not issue it. User/GRANT setup as in the README; YDB answers `SELECT 1` through its CLI. A driver-version fact, amd64 emulated — **PASS on psqlodbc 16.00.0005** built from `REL-16_00_0005` for this entry (`PostgreSQL (via ODBC) 14.0.5`, `SQL_DRIVER_VER` confirmed via pyodbc); the 18.x fact stands |
| spanner | PASS (`PostgreSQL (via ODBC) 14.1.0`), emulator + PGAdapter, 300/2,000 rows — python fetch 81,367/s (pyodbc 112,280), ingest 6,115 (array 7,141; pyodbc 265); the four harness rows first failed (DDL inside a transaction — autocommit off) and pass with `ADBC_BENCH_AUTOCOMMIT=1`: fetch 106,509–126,408/s, ingest 7,061–8,761/s, in `LANGUAGE_BENCHMARKS-macos.md` |
| arcadedb | PASS (`PostgreSQL (via ODBC) 12.0.0`), read-only fixture — python fetch 289,023/s; the four harnesses first read 0 rows — `conn.py` was recreating `adbc_big` empty per connection (fixed `09a2a41`) — and read 274,520–304,373/s with the setup unset |

## Verified at the shipped state — main @ f6a54c8

Fresh `rm -rf build`, `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
-DCMAKE_PREFIX_PATH=<unixODBC prefix>`, `cmake --build build -j`: **0 warnings**.
macOS 26.5.2 (25F84), Apple M4 Max; Apple clang 21.0.0 (clang-2100.1.1.101, Command Line
Tools); CMake 4.4.2; unixODBC 2.3.12 from source (`SQLLEN` 8); Python 3.12.12,
adbc-driver-manager 1.12.0, pyarrow 25.0.1, pyodbc 5.3.0 from source; sqliteodbc 0.99991 over
system SQLite 3.51.0; psqlodbc 18.00.0002 from source over PostgreSQL 15.15 (Homebrew, local
cluster).

```
ctest --test-dir build      100% tests passed, 0 failed out of 8 (1.20 s)
  adbc_odbc_c_smoke test_utf16 test_types test_sqllen32 test_objects test_errors test_multirow test_partition
  (the TLS pair is not built on APPLE, by design)
compat sqlite      PASS  (SQLite (via ODBC) 3.51.0)
compat postgres    PASS  (PostgreSQL (via ODBC) 15.15.0)
```

Final macOS tally: **41 pass · 0 fail · 4 no driver for this OS · 1 server not runnable here**
(46). The four "driver aborts" turned out to be unixODBC's driver manager, not the drivers (batch 5
below): Flight SQL, InfluxDB 3, Dremio and — after one quirk stopped forcing its narrow path on a
four-byte build — Virtuoso all pass through an iODBC-built bridge.
Databend, GreptimeDB, Doris and StarRocks fail through MariaDB Connector/ODBC and pass through
MySQL's own connector via an iODBC-built bridge (batch 4 below); both results are kept.

## Batch 3: tier 4, the MySQL-wire servers (main @ 1f35a5c)

MariaDB Connector/ODBC 3.2.9 (libmaodbc, `SQL_DRIVER_VER 03.02.0009`) + Connector/C 3.4.9 for
every MySQL-wire entry, with the `≥ 3.2` quirk (`no_param_arrays`) in place; `PLUGIN_DIR`
appended where the entry needs `mysql_native_password` or `caching_sha2`. All server images
arm64-native. Load 3–7. Python columns are `matrix_bench.py` (10,000 / 100,000 rows).

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| tidb | PASS (`MySQL (via ODBC) 08.00.000011`) | 46,796 | 44,287 | 90,877 (105,077) | 3,296 |
| percona | PASS (`MySQL (via ODBC) 08.04.000011`) | 45,408 | 42,507 | 113,261 (133,650) | 4,783 |
| dolt | PASS (`MySQL (via ODBC) 08.00.000033`) | 44,927 | 43,156 | 92,431 (105,090) | 3,081 |
| databend | **FAIL** at `SQLDriverConnect`: `[HY000] (1105) [ma-3.2.9] Unknown table "default"."default".DUAL` — the connector's own connect-time probe `SELECT 1 FROM DUAL WHERE @@sql_mode LIKE '%ansi_quotes%'`; identical through pyodbc, with or without `NO_SSPS` | | | | |
| matrixone | PASS (`MySQL (via ODBC) 08.00.000030`) | 44,995 | 42,976 | 188,960 (188,201) | 3,116 |
| greptimedb | **FAIL** at `SQLDriverConnect`: `[42S02] (1146) [ma-3.2.9] (TableNotFound): Table not found: greptime.public.dual` — the same probe; identical through pyodbc | | | | |
| columnstore | PASS (`MariaDB (via ODBC) 11.01.000001`) — provisioning and user by hand, `columnstore.cnf` mounted from `/private/tmp` | 44,197 | 42,263 | 107,253 (89,203) | 4,188 |
| mongodbbi | PASS (`MySQL (via ODBC) 05.07.000012`; read-only) — mongosqld 2.14.30 linux-arm64 build inside the `mongo:7` arm64 container (no macOS build of 2.14.x) | 39,271 | — | — | — |
| doris | **FAIL** at `SQLExecute`: `[HY000] (1105) [ma-3.2.9][5.7.99] NullPointerException, msg: null` — Doris 2.1.0 NPEs on MariaDB Connector/ODBC's server-side prepared INSERT; with the connector's `PREPONCLIENT=1` the VARBINARY parameter is inlined as `_binary '<raw bytes>'`, which Doris rejects as a syntax error. Single-row parameterised INSERTs of int/varchar/date through pyodbc work. MySQL Connector/ODBC's `NO_SSPS=1` path, which passes on Linux, has no libmaodbc equivalent (BE alive; 2.27 GiB / 6 GiB) | | | | |
| starrocks | **FAIL** at `SQLExecute`: `[HY000] (1064) [ma-3.2.9][8.0.33] Getting syntax error at line 1, column 59 … Unexpected input ''''` — column 59 is the connector's inlined `_binary ''` literal, identical with and without `PREPONCLIENT=1`. Same cause as Doris: the binary-literal / prepared path of MariaDB Connector/ODBC, where MySQL Connector/ODBC with `NO_SSPS=1` passes on Linux (BE alive; 1.09 GiB / 5 GiB) | | | | |
| oceanbase | PASS (`MySQL (via ODBC) 05.07.000025`) — `MODE=SLIM`, boot in 40 s, 4.3 GiB / 6 GiB peak | 45,282 | 43,170 | 106,711 (102,495) | 3,435 |

The one number to read from this table: **every MySQL-wire server fetches at 39–47k rows/s
here, through the bridge and through pyodbc alike**, against 1.0–2.0M rows/s for the same
servers on Linux. The bridge's own path is the same on both platforms; what differs is the
client library — MariaDB Connector/ODBC here, MySQL Connector/ODBC on Linux — and pyodbc
hitting the same ceiling puts it in the connector's fetch path; batch 4 confirms it from the
other side, with the same servers reading at 1.3–4.5M rows/s through MySQL's connector. Ingest through the same connector runs 86–208k rows/s, 20–60× pyodbc.

Tier 4 through this connector: **33 pass, 8 fail (4 driver abort-on-error, 2 connector `DUAL` probe, 2 connector binary-literal/prepared path)**; batch 4 below takes the four connector failures through MySQL's own connector.

Also closed here: `ydb` on psqlodbc 16.00.0005 (`PostgreSQL (via ODBC) 14.0.5`): fetch
541,823 (pyodbc 389,313), ingest 1,822 (array 1,781; pyodbc 94), with `ADBC_BENCH_AUTOCOMMIT=1`
for the language harnesses. `go/monetdb` (autocommit on, `-no-native`); `go/db2` fetch stays
empty — the harness's second `SQLDriverConnect` fails on the IBM clidriver every time while
the other three languages reconnect fine.

## Batch 4: the four MariaDB-connector failures through MySQL's own connector (main @ 5bb3e3c)

MySQL Connector/ODBC **26.7.1 for macOS arm64 exists** (Oracle renumbered 9.x → 26.x; the
download page is JavaScript-only and its `/get/` URL refuses `curl`, so it is fetched through
the "No thanks, just start my download" link) — and it is **built for iODBC**: `libmyodbc26w.so`
links `@rpath/libiodbcinst.dylib`. It can only be used through a bridge built against iODBC;
relinking it to unixODBC is not valid (4-byte vs 2-byte `SQLWCHAR`, every call fails with an
empty diagnostic). That route exposed two bridge bugs, fixed on the way (`6843467`, `c2afdfe`,
`5bb3e3c`; see `docs/TROUBLESHOOTING.md`): the wide-text codecs assumed 2-byte units, and this
connector reads bound wide parameters inconsistently with how it writes columns, so on a
4-byte build it now takes the narrow UTF-8 route.

Recipe: iODBC 3.52.16 from the `openlink/iODBC` tag (`autogen`, `./configure --prefix=<iodbc>
--disable-static --disable-gui`, `make install`); the bridge with `cmake -S . -B build-iodbc
-DCMAKE_BUILD_TYPE=Release -DODBC_INCLUDE_DIR=<iodbc>/include -DODBC_LIBRARY=<iodbc>/lib/libiodbc.dylib`
(clean, 0 warnings, `ctest --test-dir build-iodbc` 8/8 — compiled against the 4-byte
`SQLWCHAR` for real); the connector with `xattr -c lib/*.so lib/*.dylib lib/plugin/*.so`,
`install_name_tool -add_rpath <iodbc>/lib -add_rpath <connector>/lib lib/libmyodbc26w.so`,
`codesign -f -s - lib/libmyodbc26w.so`, and `PLUGIN_DIR=<connector>/lib/plugin` (the entries'
`{plugin_dir}`). Docker VM 16 GiB, load 1.3–3.2, `ADBC_BENCH_AUTOCOMMIT=1` for the harnesses.

| entry | result | ADBC fetch | ADBC ingest (array) |
|---|---|---:|---:|
| databend | PASS (`MySQL (via ODBC) 8.0.90-v1.2.881`) | 2,034,078 | 13,157 (13,581) |
| greptimedb | PASS (`MySQL (via ODBC) 8.4.2`) | 1,336,557 | 25,700 (23,501) |
| doris | PASS (`MySQL (via ODBC) 5.7.99`), 300/2,000 rows, 2.86 GiB / 6 GiB | 261,990 | 1,197 (1,226) |
| starrocks | PASS (`MySQL (via ODBC) 8.0.33`), 300/2,000 rows, 1.69 GiB / 5 GiB | 301,326 | 1,040 (1,026) |

No pyodbc, odbc-api or arrow-odbc columns: those clients link unixODBC and cannot load an
iODBC driver. Two things the numbers say. Databend and GreptimeDB read at 2.0M and 1.3M
rows/s through this connector — the same Mac read every MySQL-wire server at 39–47k through
MariaDB Connector/ODBC, which settles where that ceiling lives. And the narrow UTF-8 binding
costs nothing measurable: the pre-fix build that bound narrow by a local patch read 2,234,246
and 1,295,615 rows/s on the same two servers.

## Batch 5: the four "driver aborts" were unixODBC (main @ 65cff5b)

Bridge-free root cause, established with a 45-line C program and lldb (no `.ips` crash report
is written for these aborts). Both macOS drivers — Virtuoso's `virtodbcu_r.so` (Homebrew 7.2.17,
whose formula has no unixODBC dependency) and the Arrow Flight SQL ODBC 0.9.7 armv8 build — are
built to iODBC's convention: `SQLWCHAR` is `wchar_t`, four bytes. Called directly through `dlopen`
with a 0xAA-filled buffer after a failing statement, both write their `SQLGetDiagRecW` text as
UCS-4 (`5b 00 00 00 4f 00 00 00 …` — "[Ope"; `5b 00 00 00 41 00 00 00 …` — "[Apa") and fill
`BufferLength × 4` bytes. unixODBC 2.3.12's driver manager, whose `SQLWCHAR` is two bytes, reads
that diagnostic on the first `SQL_ERROR` into a 12-byte stack array (`SQLWCHAR sqlstate[6]` in
`DriverManager/__info.c`, `extract_diag_error_w`), and the stack protector aborts the process:

```
frame #0: libsystem_c.dylib`__stack_chk_fail
frame #1: libodbc.2.dylib`extract_diag_error_w(...) at __info.c
frame #2: libodbc.2.dylib`function_return_ex(level=3, ..., ret_code=-1, ...) at __info.c:5215
frame #3: libodbc.2.dylib`SQLExecDirect(...) at SQLExecDirect.c:527
```

It fires on the first call that returns `SQL_ERROR` — `SQLExecDirect`, `SQLPrepare`,
`SQLExecDirectW` alike, with or without a successful statement before it; successful statements
are unaffected. The same program linked against iODBC 3.52.16 gets the proper diagnostics from
both drivers (`[42S02] … SQ074: Line 1: No table no_such_table_xyz`; `[HY000] (100) [Apache
Arrow][Flight SQL] … Catalog Error: Table with name no_such_table_xyz does not exist!`) and
survives. The Linux reference host reproduces the class without either driver: a fake ODBC
driver compiled with a 4-byte `SQLWCHAR` (`SQL_WCHART_CONVERT`) makes unixODBC 2.3.12 abort on
its first error, while the same driver with 2-byte units survives — on 2.3.12 and on 2.3.14 built from the release
tarball. Reported: [lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239) (the overflow, with that repro),
[openlink/virtuoso-opensource#1469](https://github.com/openlink/virtuoso-opensource/issues/1469) and [dremio/warpdrive#16](https://github.com/dremio/warpdrive/issues/16) (the undocumented width).

Through a bridge built against iODBC (65cff5b, 0 warnings), read-only entries, `matrix_bench.py`:

| entry | result | ADBC fetch |
|---|---|---:|
| flightsql | PASS (`sqlflite (via ODBC) 00.00.0000`, DuckDB 1.1.1) | 8,260,509 |
| influxdb3 | PASS (`InfluxDB IOx (via ODBC) 02.00.0000`) | 8,741,131 |
| dremio | PASS (`Dremio Server (via ODBC) 26.00.0005`) | 1,341,476 |
| virtuoso | PASS (`OpenLink Virtuoso (via ODBC) 07.20.3243`), ingest 3,128 (array 3,065) — after the `virtodbc` quirk stopped forcing the narrow path on a four-byte build. The experiment that settled it, bridge at d8f2b54 with a local knob: narrow path, conn unchanged → `statement literal 'héllo' matched nothing`; narrow + `CHARSET=UTF-8` → `hÃ©llo ð` (the UTF-8 bytes widened one per unit); wide path, conn unchanged → **PASS**; wide + `CHARSET=UTF-8` → literal matches nothing. `bigint_param_as_string` and `no_param_arrays` stay, both still needed. A narrow string literal in a `SELECT` (`SELECT 'héllo 🚀'`) comes back as Virtuoso's 8-bit VARCHAR bytes — server semantics, not a failure; NVARCHAR columns and bound parameters are fine | 252,282 |

No pyodbc / odbc-api cells (unixODBC-linked clients). The language rows followed once the
toolchains were reinstalled — `LANGUAGE_BENCHMARKS-macos.md`: 5.6–8.3M rows/s across five
languages on sqlflite and InfluxDB 3, 1.3–1.6M on Dremio, Virtuoso 240–258k with ingest ~3k. Side finding on the Flight SQL driver: `LogEnabled=true` with a real
`LogPath` makes `SQLAllocHandle(ENV)` fail with `IM004` — `spdlog::rotating_file_sink` throws
inside the driver's logger init.
