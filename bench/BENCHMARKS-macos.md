<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — macOS

Measured 2026-08-24 on a real machine (first non-CI macOS run), commit `a5bf1b4`.
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

### `tests/test_plug_and_play.py` — 1 of 2 flows passed at `a5bf1b4`; the second was Linux-specific as written

* `install --prefix + ADBC_DRIVER_PATH: CONNECTED ADBC ODBC Driver` — PASS.
* `install.sh + user config dir` — FAIL on macOS at `a5bf1b4`, **test-harness issue, not a driver
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

### Bonus: other Python tests

| test | result |
|---|---|
| `test_driver_load_errors.py` | 3 ok (`missing_driver_library_is_still_reported_as_missing`, `unreadable_driver_library_says_permission_denied`, `static_tls_exhaustion_is_explained`→SKIP fixture not built, as designed) |
| `test_prefetch.py` | 24 passed, 3 skipped (need PG_URI) |
| `test_delegate.py` | 8 PASS, 22 SKIP at `a5bf1b4` — **harness bug, fixed after this run**: the file hard-coded `build/libadbc_fake_native_driver.so`; the target is `.dylib` here, so every delegation test skipped. It now uses the platform's suffix |
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
