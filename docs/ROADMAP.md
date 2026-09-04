<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbcBridge roadmap

adbcBridge turns legacy database connectivity layers into Arrow-native ADBC drivers.
ODBC is the first bridge; the name leaves room for the others.

## 1. ODBC bridge (`libadbc_driver_odbc`) — now

| Milestone | Status |
|---|---|
| Query, types, NULLs, Unicode, parameters, bulk ingest, metadata, errors | done |
| Compatibility matrix: **53 databases** verified on Linux by one workload (`docs/COMPATIBILITY.md`) | done |
| Clients measured against all 46: Python, Rust, Go, Java, C# (`bench/LANGUAGE_BENCHMARKS.md`); R smoke-tested | done |
| Array-bound and multi-row bulk ingest, probed per driver, default on | done |
| Plug-and-play install: `install.sh`, driver manifest, `driver="odbc"` by name (Linux, macOS) | done |
| macOS verified on a real machine: SQLite, PostgreSQL 15, SQL Server 2022 (`bench/BENCHMARKS-macos.md`) | done |
| Windows: first build 2026-08-24 (ten defects across driver, tests and harnesses, three of them the ANSI code page in three disguises); compat campaign on an 8 GB laptop, then the full 46-entry re-measure on a 32 GB machine on 2026-08-25 (45 pass; MySQL Connector/ODBC 26.7.1 retires the astral class) — `bench/BENCHMARKS-windows.md` | done |
| macOS: 53 of 53 results (44 pass, 8 driver/server unavailable, 1 not run); a bridge built against iODBC is a supported configuration for vendor drivers that only ship iODBC builds (MySQL Connector/ODBC for macOS) — three width bugs found and fixed on the way (`docs/TROUBLESHOOTING.md`) | done |
| 2026-09-03 batch: SingleStore, SAP HANA Express, Exasol, Altibase, Kinetica, Actian Ingres and IBM Db2 for i (PUB400.COM) verified on Linux — 53 entries; macOS and Windows cells measured the same week (44 and 48 pass; every other cell names its reason in `docs/COMPATIBILITY.md`) | done |
| Windows: per-connection narrow-text route for ANSI-only drivers whose narrow path is UTF-8 (`narrow_sql` + `narrow_params` + `wchar_as_utf8`, keyed on the driver, no global state) — Apache Ignite passes on Windows, measured on the second Windows machine | done |
| Windows, to verify on the next Windows session (reported by the second Windows campaign, not reproduced on a machine still available): (1) whether `cmake --install` under MSVC can write the manifest key `windows_amd64_mingw` (CMake's `MINGW` is only set for a GNU toolchain; the campaign's own build log recorded `windows_amd64`); (2) the Go binding's SQLite test leaves the database file open after `Close` so `t.TempDir()` cleanup fails — the drivermgr `Close` calls `AdbcConnectionRelease`/`AdbcDatabaseRelease` synchronously, so the open handle is either a driver-side pool or a missing release in the test's reader path | next |
| Connection-level reader options (`adbc.odbc.batch_size`, `sqllen_32bit`) set before AdbcConnectionInit are discarded — `OdbcConnectionInit` copies the database defaults over them; re-apply the recorded pre-options after the copy | next |
| Windows: prefetch pipeline and parallel ingest — both pthreads, compiled out on `_WIN32`; a Win32 shim (SRWLOCK, CONDITION_VARIABLE, `_beginthreadex`) restores them | next |
| **Driver bootstrap**: `install.sh` / the Windows and macOS installers fetch the open-licence ODBC drivers a first run needs (sqliteodbc, psqlodbc, MariaDB Connector/ODBC, clickhouse-odbc) so SQLite/PostgreSQL/MySQL work with nothing else installed; vendor drivers (Oracle, Db2, SQL Server, Snowflake…) stay the user's download — their licences do not allow redistribution, and Windows already ships the SQL Server driver | next |
| ADBC Driver Foundry validation suite: driver defects D1–D14 | in progress |
| Foundry listing (adbc-drivers.org) | next |
| Release workflow: Linux x86_64/aarch64 (manylinux_2_28), macOS arm64 and Windows x64 libraries + Python wheels built on tag, attached to the GitHub Release (`.github/workflows/release.yml`) | done — [v0.1.0](https://github.com/singhpratech/adbcbridge/releases/tag/v0.1.0) on 2026-08-25: 4 libraries, 4 wheels + sdist, crate, nupkg, jar |
| PyPI via trusted publishing (`.github/workflows/publish-pypi.yml`, dispatched by hand with the tag once the Release workflow has published it); NuGet the same way (`publish-nuget.yml`); crates.io from a checkout of the tag | done (0.1.0 on PyPI, crates.io and nuget.org, 2026-08-26) |
| Language packages: Python wheel (`python/`), Rust crate (`rust/`), NuGet (`csharp/`), Maven (`java/`), Go module (`go/`) — built and attached to every release (the crate tested there, the others by their own suites under `tests/`); PyPI, crates.io and nuget.org carry 0.1.0; Maven Central is the remaining registry | in progress |
| Databases that need vendor downloads behind a login: Teradata, SAP HANA, Snowflake, Databricks/Spark (Simba), Hive | help wanted |

## 2. JDBC bridge (`libadbc_driver_jdbc`) — next

Same idea for the JDBC universe: load a JVM in-process (JNI), drive any JDBC driver, and
return Arrow batches through the ADBC ABI — so Python, R, Go, Rust and C# can use JDBC-only
drivers without a Java application. Apache Arrow already has a Java-side JDBC adapter; this
is the C-side one.

## 3. OLE DB bridge — later

Windows-only; covers the few remaining sources that have OLE DB providers but no ODBC driver.

## Non-goals

- Replacing native ADBC drivers where they exist (PostgreSQL, Snowflake, BigQuery, …): those
  will always be faster. adbcBridge is for everything else, and for the long tail of enterprise
  databases that will never get a native driver.
