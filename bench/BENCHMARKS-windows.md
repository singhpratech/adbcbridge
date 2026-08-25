<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — Windows

**Status: measured on two machines — 45 of 46 databases pass on the second (14-core, 32 GB, every server in Docker Desktop) at `a4d6ce5` plus the four Windows fixes in `src/` that this campaign found, 1 fails on a vendor driver's ANSI-code-page conversion (ANSI-code-page conversion inside the driver: tdengine), none on the bridge; the first machine's campaign (7.7 GB laptop, 26 pass) is kept below as history; five languages × 46 databases on the second machine in `LANGUAGE_BENCHMARKS-windows.md`, 219 of 230 language rows measured, on top of the first machine's 142 language cells.** Until 2026-08-24 the Windows build had never succeeded on any commit, and
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

<!-- bigwin-begin -->
## Second machine — all 46 entries re-measured, 2026-08-25, main @ a4d6ce5 plus four Windows fixes in `src/`

**45 of 46 pass, 1 fail — the one failure a vendor driver's ANSI-code-page conversion, not the bridge's (ANSI-code-page conversion inside the driver: tdengine).**
This pass supersedes the first machine's column below wherever the two differ; the
difference is a driver version, a server that the 7.7 GB VM could not run, or one of the
four bridge defects this campaign found and fixed (Firebird, Informix, the Arrow Flight SQL
driver, Apache Ignite — see below; every row is measured with all four in the tree).

| | |
|---|---|
| OS | Windows 11 Home 23H2 (build 22631), x64 |
| CPU / RAM | Intel Core i9-13900HK, 14 cores / 20 threads; 32 GB |
| Driver manager | Windows ODBC (odbc32.dll), ANSI code page 1252 |
| Build | CMake 4.4.2, MSVC 19.44.35228, Windows SDK 10.0.26100; x64 Release, **7/7 ctest**, zero warnings; `tests/test_windows_text.py` 7 passed |
| Python | 3.12.10 x64, adbc-driver-manager 1.12.0, pyarrow 25.0.1, pyodbc 5.3.0, tzdata |
| Servers | every entry a Docker Desktop 4.88 (engine 29.7.2) container on WSL2, `.wslconfig` 20 GB / 12 CPUs / no swap; images and compose services from `tests/compat/docker-compose.yml` unchanged |
| ODBC drivers (64-bit) | SQLite3 ODBC Driver (3.43.2); DuckDB Driver 1.5.5.0; PostgreSQL Unicode(x64) psqlodbc 18.00.0002 and PostgreSQL Unicode 16(x64) 16.00.0007 (YDB); **MySQL ODBC 26.7 Unicode Driver** (Connector/ODBC 26.7.1); ODBC Driver 18 for SQL Server 18.6.2.1; ClickHouse ODBC 1.5.5; MonetDB ODBC 11.55.7 (20260615); Oracle in instantclient_23_0; IBM Db2 clidriver 12.1.4; Vertica 25.1.0; OpenSearch SQL ODBC 1.5.0.0; Arrow Flight SQL ODBC 00.09.0007 (Dremio); Apache Ignite 2.18.0; TDengine 3.4.2.5; Virtuoso 7.2.17; Firebird ODBC 3.0.1; Microsoft Access Database Engine 2016 x64 |
| Build features | no prefetch pipeline, no ingest fan-out (compiled out on `_WIN32`) |
| Load | compat runs 10 at a time; benchmarks 8 at a time with ~30 containers up — single samples under contention, so treat every number as a floor; oracle and opensearch re-measured alone agreed within 3% and 25% respectively |

Workload per entry: `tests/compat/test_matrix.py <entry>` (the compat workload) then
`bench/matrix_bench.py --rows 10000 --fetch-rows 100000 <entry>` (ClickHouse with
`--pyodbc-timeout 120`; Spanner compat only, see the first-machine note). Each benchmark ran
from its own copy of `bench/` + `tests/compat/`, because `matrix_bench.py` keeps its cache next
to itself and concurrent runs corrupt it.

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| sqlite | PASS (`SQLite (via ODBC) 3.43.2`) | 965,045 | 497,073 | 467,965 (489,790) | 354,867 |
| duckdb | PASS (`DuckDB (via ODBC) 1.5.5`) | 635,958 | 382,443 | 105,127 (109,474) | 1,120 |
| postgres | PASS (`PostgreSQL (via ODBC) 16.15.0`) | 371,764 | 192,102 | 264,062 (304,057) | 1,541 |
| mariadb | PASS (`MySQL (via ODBC) 11.8.9-MariaDB-ubu2404`) | 588,421 | 357,274 | 44,678 (40,097) | 1,389 |
| columnstore | PASS (`MySQL (via ODBC) 11.1.1-MariaDB-log`) | 497,803 | 376,152 | 4,578 (4,879) | 1,523 |
| oracle | PASS — Instant Client 23, `NLS_LANG=.AL32UTF8`; alone: fetch 30,874 / ingest 18,876 (`Oracle (via ODBC) 23.26.0200`) | 30,874 | 29,842 | 18,876 (19,271) | 292 |
| clickhouse | PASS — pyodbc ingest at one HTTP request per row, capped at 120 s (`ClickHouse (via ODBC) 26.7.5.10`) | 423,413 | 319,357 | 1,201 (1,202) | — |
| mssql | PASS (`Microsoft SQL Server (via ODBC) 16.00.4265`) | 867,720 | 528,646 | 30,429 (47,838) | 90,589 |
| azuresqledge | PASS (`Microsoft SQL Server (via ODBC) 16.00.5100`) | 920,637 | 590,090 | 13,965 (26,624) | 38,281 |
| mysql | PASS (`MySQL (via ODBC) 8.4.11`) | 605,429 | 399,614 | 38,869 (30,743) | 1,383 |
| tidb | PASS (`MySQL (via ODBC) 8.0.11-TiDB-v7.5.1`) | 579,121 | 361,928 | 50,019 (49,267) | 752 |
| dolt | PASS (`MySQL (via ODBC) 8.0.33`) | 526,988 | 126,788 | 27,345 (36,764) | 869 |
| databend | PASS — pyodbc ingest form refused by the server (`MySQL (via ODBC) 8.0.90-v1.2.881-ca29960f5c(rust-1.94.0-nightly-2026-04-17T07:20:31.684496168Z)`) | 560,952 | 437,956 | 3,770 (4,677) | — |
| percona | PASS (`MySQL (via ODBC) 8.4.11-11`) | 585,548 | 269,611 | 39,624 (38,920) | 1,163 |
| matrixone | PASS (`MySQL (via ODBC) 8.0.30-MatrixOne-v4.2.0`) | 752,149 | 475,380 | 43,257 (42,025) | 576 |
| doris | PASS — 26.7.1 fixes the astral class; FE+BE in one container, ~6 min to `Alive`; pyodbc ingest form refused by the server | 829,499 | 277,358 | 1,677 (1,898) | — |
| oceanbase | PASS — `NO_SSPS=1` added via `OCEANBASE_CONN`; the connector's stderr `Character set '45' is not a compiled character set` is harmless (utf8mb4_general_ci id 45 missing from the 26.7 client's charsets Index) (`MySQL (via ODBC) 5.7.25`) | 800,665 | 471,138 | 80,659 (76,696) | 2,284 |
| greptimedb | PASS — pyodbc ingest form refused by the server (`MySQL (via ODBC) 8.4.2`) | 321,240 | 191,745 | 55,848 (90,967) | — |
| starrocks | PASS — each INSERT is a load transaction; pyodbc's batch INSERT form is refused (`MySQL (via ODBC) 8.0.33`) | 714,882 | 516,061 | 3,430 (3,790) | — |
| mongodbbi | PASS (`MySQL (via ODBC) 5.7.12 mongosqld v2.14.22`) | 151,363 | — | — (—) | — |
| db2 | PASS — `Authentication=SERVER` in the connection string, see below (`DB2/LINUXX8664 (via ODBC) 12.01.0500`) | 398,715 | 431,224 | 57,557 (81,651) | 2,780 |
| informix | PASS — after the narrow-parameter fix below, with `DB2CODEPAGE=1208` set (`IDS/UNIX64 (via ODBC) 12.10.0000`) | 591,439 | 284,124 | 2,711 (91,977) | 2,308 |
| monetdb | PASS (`MonetDB (via ODBC) 11.55.0007`) | 361,206 | 282,512 | 76,589 (79,423) | 306 |
| vertica | PASS — single-row ingest is driver round-trip-bound; array binding is the real number (`Vertica Database (via ODBC) 25.03.0000`) | 189,248 | 173,115 | 2,011 (49,274) | 46,407 |
| cockroachdb | PASS (`PostgreSQL (via ODBC) 18.0.0`) | 271,045 | 233,829 | 31,478 (31,555) | 759 |
| yugabyte | PASS (`PostgreSQL (via ODBC) 15.12.0`) | 264,353 | 221,607 | 13,386 (15,649) | 681 |
| timescaledb | PASS (`PostgreSQL (via ODBC) 16.15.0`) | 360,484 | 224,157 | 213,915 (268,389) | 1,558 |
| citus | PASS (`PostgreSQL (via ODBC) 18.4.0`) | 259,309 | 285,972 | 275,236 (259,308) | 1,536 |
| cloudberry | PASS (`PostgreSQL (via ODBC) 14.4.0`) | 390,739 | 308,765 | 4,901 (6,252) | 28 |
| materialize | PASS (`PostgreSQL (via ODBC) 9.5.0`) | 103,790 | 96,458 | 17,041 (18,502) | 651 |
| opengauss | PASS (`PostgreSQL (via ODBC) 9.2.4`) | 257,499 | 219,314 | 58,393 (76,996) | 1,521 |
| cratedb | PASS (`PostgreSQL (via ODBC) 14.0.0`) | 252,400 | 236,161 | 11,314 (14,114) | 24 |
| questdb | PASS (`PostgreSQL (via ODBC) 11.3.0`) | 374,822 | 259,358 | 45,104 (62,752) | 2,784 |
| risingwave | PASS (`PostgreSQL (via ODBC) 13.1400.0`) | 242,606 | 245,511 | 19,758 (20,913) | 387 |
| spanner | PASS — compat only — bench not run against the emulator | — | — | — (—) | — |
| firebird | PASS — after the SQL_DRIVER_NAME fix below; array column = the UNION ALL bulk form, not parameter arrays (`Firebird (via ODBC) 06.03.1812 LI-V Firebird 5.0`) | 80,444 | 68,197 | 24,453 (23,638) | 1,326 |
| virtuoso | PASS — Virtuoso Open Source 7.2 Windows client driver (`OpenLink Virtuoso (via ODBC) 07.20.3243`) | 196,184 | 174,845 | 2,682 (2,811) | 2,801 |
| flightsql | PASS — after the text-as-binary fix below (1.8× the wide read); sqlflite 1.5.5 (`sqlflite (via ODBC) 00.00.0000.duckdb v1.1.1`) | 5,211,319 | — | — (—) | — |
| arcadedb | PASS | 104,786 | — | — (—) | — |
| influxdb3 | PASS — after the text-as-binary fix below; influxdb:3-core (`InfluxDB IOx (via ODBC) 02.00.0000`) | 6,137,567 | — | — (—) | — |
| ignite | PASS — after the narrow-text fix below; ANSI-only driver (`Apache Ignite (via ODBC) 02.04.0000`) | 775,660 | — | — (—) | — |
| opensearch | PASS — read-only; 67,410 rows/s under 8-way load, 84,094 alone (`OpenSearch (via ODBC) 3.8.0`) | 84,094 | — | — (—) | — |
| ydb | PASS (`PostgreSQL (via ODBC) 14.0.5`) | 340,051 | 255,062 | 1,995 (1,903) | 69 |
| dremio | PASS — after the text-as-binary fix below; Dremio 26.0.0005 (`Dremio Server (via ODBC) 26.00.0005-202509091642240013-f5051a07`) | 1,002,050 | — | — (—) | — |
| tdengine | **FAIL** — driver-side: taos_odbc converts through the ANSI code page (`UTF-32LE → CP1252` fails on 🚀); over websocket, the 3.4.2.5 client cannot speak native to the 3.3.6 server | 298,618 | — | — (—) | — |
| access | PASS — ACE 2016 x64 on the checked-in .mdb fixture; read-only entry, so fetch only, of a fixture-sized table (`ACCESS (via ODBC) 04.00.0000`) | 3,352,705 | — | — (—) | — |

Rows/s; `—` = not applicable (read-only entry) or the step failed/timed out (noted in the
result cell or in `docs/COMPATIBILITY.md`).

### What changed against the first machine

**MySQL Connector/ODBC 26.7.1 retires the astral `???` class.** dev.mysql.com's
Connector/ODBC page offers `mysql-connector-odbc-26.7.1-winx64.msi` (the "Innovation" track
that replaced 9.x — the first campaign's note that nothing newer than 8.4.0 is published for
Windows is obsolete). Installed, it replaces 8.4.0 and registers `MySQL ODBC 26.7 Unicode
Driver`; with it databend, greptimedb, matrixone, mongodbbi, starrocks and doris pass the full
workload, astral check included, with their stock connection strings (`NO_SSPS=1` still in
them via `{no_ssps}`). The 8.4.0 failure was pinned down first, on this machine, as read-side
and inside the connector: the server stores `héllo 🚀` byte-exact (`HEX(s)` =
`68C3A96C6C6F20F09F9A80`), and `SQLGetData` returns `3f 3f 3f` for the emoji under
SQL_C_CHAR and SQL_C_WCHAR alike, with or without `CHARSET=utf8mb4` — so no bridge-side
narrow/wide routing could have repaired it; only the driver upgrade does.

**Four bridge defects, found and fixed here.** All four are Windows-only and all four are
the same shape: a quirk that is right on Linux keyed on something that differs on Windows.

* *Firebird.* The driver answers `SQL_DRIVER_NAME` as `OdbcFb` on Linux and as `FirebirdODBC`
  on Windows (its DLL's name), and the quirk block keyed on the former — the one that switches
  parameter arrays off because the driver accepts `SQL_ATTR_PARAMSET_SIZE` and executes a
  single set — never fired. The first run stopped at the bridge's own guard (`accepted a
  parameter array of 2 sets but reported neither SQL_ATTR_PARAMS_PROCESSED_PTR nor
  SQL_ATTR_PARAM_STATUS_PTR`), which is the guard doing its job; matching both names passes the
  whole workload, UNION-ALL bulk form included.
* *Informix.* The IDS quirk sends the astral parameter narrow through `wchar_as_utf8`, which
  the Windows block at the end of the quirk table resets for every driver — so the parameter
  still went SQL_C_WCHAR and the INSERT failed `-415 Data conversion error`. `narrow_params`,
  the mechanism Ignite already used, keeps it on SQL_C_CHAR everywhere; with `DB2CODEPAGE=1208`
  in the environment (so the CLI driver reads those bytes as UTF-8 rather than cp1252) the
  workload passes. Verified first with pyodbc: SQL_C_CHAR + UTF-8 round-trips `héllo 🚀`
  byte-exact, wide UTF-16 gets -415.
* *Arrow Flight SQL ODBC (flightsql, influxdb3, dremio).* The driver's Windows build returns
  U+1F680 as U+F680 — the low 16 bits — through SQL_C_WCHAR and `?` through SQL_C_CHAR (the
  ANSI code page), pyodbc identical, which the first campaign recorded as unfixable. Its
  SQL_C_BINARY conversion of a text column hands the server's UTF-8 through byte-exact
  (probed with pyodbc against sqlflite and Dremio), so the reader now takes that route for this
  driver on Windows (`text_as_binary`). It is also 1.8× faster than the wide read on sqlflite —
  no UTF-16 conversion on either side.
* *Apache Ignite.* An ANSI-only driver (no `W` entry points) whose narrow path is UTF-8. The
  Windows driver manager maps every W call onto it through the ANSI code page — wide fetches
  came back `hÃ©llo ðŸš€`, a statement literal `'héllo'` matched nothing — while a narrow
  SQL_C_CHAR buffer and a narrow `SQLExecDirect` are handed over untouched (probed). The fix
  keeps the `wchar_as_utf8` fetch quirk alive on Windows for this one driver and adds
  `narrow_sql`, a per-connection route that sends caller statement text through the narrow
  entry points; the bridge's own ASCII probes stay wide. 2.5× the fetch rate of the wide path.

**OceanBase needs `NO_SSPS=1` too.** Its entry is the one MySQL-wire connection string
without `{no_ssps}`; on Windows every bound parameter then fails with `No data supplied for
parameters in prepared statement` (pyodbc identical). Measured with an `OCEANBASE_CONN`
override that appends it; the fix is one token in the entry.

**Db2 / Informix through IBM's 12.1.4 "ODBC and CLI" zip.** `db2cli install -setup` registers
`db2clio.dll`, which that package does not ship — the driver is `clidriver\bin\db2cli64.dll`
(a forwarder into `db2app64.dll`); and the 78-character name it registers
(`IBM DB2 ODBC DRIVER - <path>`) gets `IM002` from the Windows driver manager even once the
DLL path is right, while a short alias registered by hand works. Then every connection from
python failed `SQL1042C` while `db2cli.exe validate` succeeded from the same driver: the
default `SERVER_ENCRYPT` authentication needs the gsk8/ICC crypto libraries, which only
`db2cli.exe`'s own side-by-side manifests can load; `Authentication=SERVER` in the connection
string skips that step and both entries connect (`DB2_CONN`/`INFORMIX_CONN` overrides here;
the same keyword belongs in the entries). Informix additionally needs `DB2CODEPAGE=1208` (the
fix above). All of it is in `tests/compat/README.md`'s Windows notes for the two entries.

**TDengine over websocket.** The Windows client package is 3.4.2.5 and cannot speak the native
protocol to the 3.3.6.13 server the compose file runs (TCP connects, the server drops the
connection with `read invalid packet`; not an IPv6 matter), so the entry was measured through
taosadapter's websocket listener — `URL={ws://root:taosdata@127.0.0.1:16041}`, port 6041
published by a compose override. That reaches the server and reads every column but the NCHAR
one, where taos_odbc converts through `GetACP()` and nothing in the connection string changes
it; the one remaining failure of the column.

**Two harness defects surfaced by parallel runs** (Linux never runs entries concurrently):
`bench/matrix_bench.py` reads and writes `MATRIX_BENCHMARKS.md` and `.matrix_bench.json`
without `encoding="utf-8"` (a run without `PYTHONUTF8=1` writes cp1252 and the next run fails
on `×`), and the JSON cache is not safe for concurrent writers.

**Server-side notes for Windows hosts.** Vertica: run the `vcluster create_db` step from
PowerShell, not Git Bash (MSYS path conversion rewrites `/opt/vertica/bin/vcluster`), and
PowerShell 5.1 drops an empty `--password ""` argument, so the next token becomes the
password. ColumnStore: the bind-mounted `zz-adbc.cnf` is world-writable under Docker Desktop
and ignored; copy it into `/mnt/skysql/columnstore-container-configuration/`. Dremio: the
first compat run right after boot hit a `$scratch` metadata race (`Object 'adbc_t' not found`
immediately after CTAS); rerun clean.
<!-- bigwin-end -->

## First machine (i7-8550U, 7.7 GB) — historical campaign

## Verified at the shipped state — main @ 4b3d9ff

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
MySQL 8.4. Blocked there: MariaDB (its Connector/ODBC MSI is a browser-only download — retired in phase 2: MySQL's connector drives MariaDB 12.3 with `NO_SSPS=1`),
Firebird (its security database needs an administrator to bootstrap), Access (32-bit ACE
drivers only, recorded above).

**Phase 2 — Docker Desktop on WSL2.** First declined (administrator rights and a reboot),
then done: `wsl --install` registered WSL but did not enable Virtual Machine Platform; a
second elevated `wsl --install --no-distribution` staged it, then a reboot. Docker Desktop
29.7.2 from winget, engine ready 5 s after launch; its VM disk cost ~6 GB before the first
image. The VM is capped in `.wslconfig` at 2560 MB / 2 CPUs / no swap so an over-size
container fails visibly instead of thrashing the host; containers run one at a time at
`--memory=1g`, each image deleted before the next pull (10–50 s to ready for the psqlodbc
tier). The container entries were worked through in tiers; what did not fit is recorded as
`server not runnable here: RAM` with the evidence, and the twelve vendor-driver entries
that were not attempted carry one line each in `docs/COMPATIBILITY.md` — driver obtainable
or not, server runnable in a 2.4 GB VM or not, and why (an IBM or Oracle login, a 2–4 GB
image, a UAC click per MSI, or time). The machine keeps Python, the ODBC drivers and the
repository, so a re-verification is `pip install` and `cmake` away.

### Tier 3, batch 1 — psqlodbc "PostgreSQL Unicode(x64)" 18.00.0002, x64 Release at 4b3d9ff

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

### Tier 3, batch 2 — main @ cddd466 (ctest 7/7, zero warnings)

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| questdb | PASS (`PostgreSQL (via ODBC) 11.3.0`), psqlodbc 18, 344 MB | 87,206 | 97,229 | 31,219 (28,482) | 1,455 |
| tidb | PASS (`MySQL (via ODBC) 8.0.11-TiDB-v7.5.1`), Connector/ODBC 8.4.0 + `NO_SSPS=1` | 328,749 | 207,015 | 35,704 (32,783) | 750 |
| mariadb | PASS (`MySQL (via ODBC) 12.3.3-MariaDB`), native MariaDB 12.3.3 service through MySQL Connector/ODBC 8.4.0 + `NO_SSPS=1` | 410,497 | 199,643 | 56,855 (60,345) | 5,152 |
| dolt | PASS (`MySQL (via ODBC) 8.0.33`), Connector/ODBC 8.4.0 + `NO_SSPS=1` | 351,950 | 205,135 | 28,722 (33,281) | 538 |

**Finding — MySQL Connector/ODBC 8.4.0 on Windows needs `NO_SSPS=1` against every
non-MySQL server.** Without it TiDB and MariaDB fail identically at the first bound
parameter, `[HY000] (2031) No data supplied for parameters in prepared statement`, in
pyodbc as well — the driver/server pair, not the bridge. Against MySQL 8.4 itself and
Percona the default works. Linux runs Connector/ODBC 9.4, which needs no such setting for
these servers, and 9.x is not published for Windows (every 9.x URL 404s), so on Windows
the MySQL-wire entries other than MySQL and Percona carry `NO_SSPS=1` in their connection
string. The same finding retires the MariaDB blocker: MariaDB's own connector was never
needed. The `dolt` entry's hard-coded `PLUGIN_DIR={drvdir}/plugin` expanded to `/plugin`
on Windows, where the driver is a registered name rather than a path; the entry now uses
the conditional `{plugin_dir}` like the others, and the packaged MSI's compiled-in plugin
directory is correct as it is.

QuestDB's `ADBC_BENCH_AUTOCOMMIT=1` rule holds on Windows exactly as on Linux and macOS
(0 rows in all four languages without it). Some native-client rows were stopped rather
than left to run: `odbc-api` (Rust) and `System.Data.Odbc` (C#) ingest row by row at tens
of rows/s on CrateDB and Dolt, so 10,000 rows exceeds a 10-minute window (C# on CrateDB
was still going after 30); the language file marks them `stopped`. Java on Dolt hung on
the ADBC-only path for over 10 minutes and was killed — recorded as `hung`, unexplained.

### Tier 3, batch 3 — percona/arcadedb at cddd466, risingwave/materialize at 26bae50 (ctest 7/7, zero warnings)

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| percona | PASS (`MySQL (via ODBC) 8.4.11-11`), Connector/ODBC 8.4.0, default connection string | 177,759 | 149,370 | 9,738 (12,319) | 339 |
| arcadedb | PASS (`PostgreSQL (via ODBC) 12.0.0`), read-only fixture | 84,639 | — | — | — |
| risingwave | PASS (`PostgreSQL (via ODBC) 13.1400.0`), RisingWave 3.0.3, toml bind-mounted, 56 MB used of 1.5 GB | 187,927 | 121,140 | 15,227 (14,532) | 414 |
| materialize | PASS (`PostgreSQL (via ODBC) 9.5.0`), v26.38.2 at `--memory=1536m`, 872 MB used | 110,188 | 73,957 | 9,950 (10,409) | 545 |

Materialize's harness rows need `ADBC_BENCH_AUTOCOMMIT=1` (0 rows in all four languages
without it), as on Linux and macOS. From Git Bash, a bind-mount path must be protected with
`MSYS_NO_PATHCONV=1` or the `-v` argument is rewritten.

**Docker Desktop's WSL disk never shrinks.** After ten images pulled and deleted one at a
time, `docker system df` showed nothing, yet `docker_data.vhdx` under `%LOCALAPPDATA%` was
13.2 GB and host free space had fallen to 5.3 GB: the VHDX grows monotonically, `docker
rmi` frees space *inside* it for reuse but returns none to Windows. `wsl --manage
docker-desktop --set-sparse true` is refused on WSL 2.7.12 ("currently disabled due to
potential data corruption", `--allow-unsafe` not forced); `Optimize-VHD` / `diskpart
compact vdisk` need administrator rights. **Correction after a later cycle:** on WSL 2.7.12 `wsl --shutdown` *does* compact the VHDX
when it is mostly empty — 13,228 → 7,078 MB, host free space 6.7 → 13.2 GB. So the remedy that
needs no administrator is: delete images and volumes, quit Docker Desktop, `wsl --shutdown`,
relaunch. Budget ~13 GB of VHDX for a tier-3 sweep on top of Docker Desktop's ~6 GB while it
runs. Containers
also leave anonymous volumes behind (11 of them, 992 MB, after ten entries): `docker volume
prune -af` between entries.

### Tier 3, batch 4 (interim) — main @ 26bae50

| entry | result | ADBC fetch | pyodbc fetch | ADBC ingest (array) | pyodbc ingest |
|---|---|---:|---:|---:|---:|
| yugabyte | PASS (`PostgreSQL (via ODBC) 15.12.0`), YugabyteDB 2026.1.1.1 at `--memory=1536m` with the compose memory flags, 278 MB used | 181,786 | 128,431 | 9,768 (10,579) | 528 |
| opengauss | PASS (`PostgreSQL (via ODBC) 9.2.4`), openGauss 6.0.0 at `--memory=1536m` (503 MB used; the compose 3 GB is not needed), `--shm-size=1g --cap-add SYS_NICE` | 196,872 | 103,289 | 61,544 (58,290) | 1,035 |
| databend | **FAIL** at the astral check only: `héllo 🚀` stored byte-exact on every write path, read back as `héllo ???` on every read path including pyodbc — Connector/ODBC 8.4.0 decodes Databend's result sets as 3-byte `utf8`, since Databend implements neither `@@character_set_client` nor `@@character_set_connection` (both 0) and ignores `charset=`; the same driver reads 🚀 from MySQL, TiDB, Percona and MariaDB; Linux passes on 9.4. Everything else passes | 334,776 | — | 7,538 (7,705) | — |
| greptimedb | **FAIL** at the astral check only — same signature; GreptimeDB 1.1.4, 41 MB | 224,755 | 101,186 | 62,007 (50,294) | — |
| matrixone | **FAIL** at the astral check only — same signature; MatrixOne v4.2.0, `NO_SSPS=1` needed, 405 MB | 484,587 | 235,948 | 32,259 (34,868) | 683 |

| spanner | PASS (`PostgreSQL (via ODBC) 14.1.0`), emulator + PGAdapter, two 1 GB containers, compat passed twice on fresh emulators — **bench not runnable**: `matrix_bench.py` hung twice after compat (emulator: `cross-database references are not implemented`), and killing it left the emulator refusing every connection (`FATAL: UNAVAILABLE`); recorded, not retried | — | — | — | — |
| cloudberry | PASS (`PostgreSQL (via ODBC) 14.4.0`), Cloudberry 2.1.0-incubating at `--memory=1536m --shm-size=1g`, 369 MB used, ready in ~40 s | 224,017 | 150,536 | 3,591 (4,171) | 276 |

| mongodbbi | **FAIL** at the astral check only — fourth member of the class; mongosqld v2.14.22 over mongo:7, read-only, 125 MB | 97,643 | — | — | — |

| clickhouse | PASS (`ClickHouse (via ODBC) 26.7.5.10`), clickhouse-odbc 1.5.5 from the GitHub MSI, server at 1 GB (97 MB used); pyodbc ingest at one HTTP request per row was at 3,501 rows after 15 min and was capped (`--pyodbc-timeout 120`) | 159,763 | 107,933 | 282 (516) | — |
| doris | **server not runnable here**: 7 GB image against 5–10 GB free host disk, ~2.5 GB resident under a 6 GB cap, 2.4 GB VM — not attempted, since the pull would have filled the disk under Docker | | | | |

| ydb | PASS (`PostgreSQL (via ODBC) 14.0.5`) with psqlodbc 16.00.0007, registered by hand — 18.00.0002 fails at connect (`unrecognized configuration parameter "datestyle"`), and a DLL path in `Driver=` gets `IM002` from the Windows driver manager (unixODBC accepts one); 1,024 MB used at 1536m | 101,712 | 61,361 | 750 (756) | 38 |
| starrocks | **FAIL** at the astral check only — fifth member of the class; StarRocks 4.x at 1536m (976 MB used) | 402,109 | 213,907 | 2,603 (3,957) | — |

| oceanbase | **server not runnable here: RAM** — `MODE=SLIM` at 1536m stalled 17 min at 1.442/1.5 GiB after `observer program health check ok`, never `boot success` (OOMKilled=false) | | | | |
| azuresqledge | PASS (`Microsoft SQL Server (via ODBC) 16.00.5100`), Azure SQL Edge Developer image at 1536m (514 MB used), msodbcsql 18 | 237,448 | 136,506 | 12,127 (21,976) | 20,548 |
| columnstore | PASS (`MySQL (via ODBC) 11.1.1-MariaDB-log`) at `3ca34a5`, Connector/ODBC 8.4.0 + `NO_SSPS=1`, fresh container at 1536m, utf8mb4 from the start — **FAIL before `52d3533`**: generated DDL refused because the ColumnStore probe never ran through MySQL's connector; a bridge bug this column found, fixed, and re-measured green here. Not in the astral class: MariaDB exposes the charset variables | 294,795 | 190,464 | 13,206 (13,432) | 1,466 |

| monetdb | PASS (`MonetDB (via ODBC) 11.55.0007`), MonetDB ODBC Installer 20260615, container at 1 GB (19 MB used) | 268,684 | 167,720 | 55,696 (55,757) | 236 |
| virtuoso | **FAIL, bridge-side, fix landed not re-measured**: two virtodbc.dll faults — `SQLSetPos(SQL_POSITION)` fails, and the bound-column indicator array is written at a 4-byte stride on a block cursor. `9944616` skipped the repair; the stride is read correctly by the `ind_stride_32bit` quirk. Peer measured the equivalent prototype PASS (fetch 90,426/s); machine closed, committed build not re-measured | — | 75,746 | 1,194 | — |
| ignite | **FAIL, driver ANSI-only, bridge fix designed/deferred**: ignite.odbc.dll exports no `W` entry point, so the Windows DM mangles statement literals and wide fetches (pyodbc identical). `narrow_params` (`9944616`) fixed the parameter side; routing text narrow needs a per-connection switch (peer prototype PASS, fetch 348,579/s) — deferred, `private/` | 274,871 | — | — | — |
| tdengine | **FAIL, driver**: taos-odbc's Windows build is ANSI and its iconv has no `CP1252 → UTF-8` table, so statement text handed over in the system code page cannot be converted | | | | |
| flightsql | **FAIL, driver, astral only**: the Flight SQL ODBC Windows build returns U+1F680 as U+F680 (low 16 bits kept), pyodbc identical; everything else passes, fastest fetch of the campaign | 2,013,077 | — | — | — |
| influxdb3 | **FAIL, driver**: the same low-16-bit truncation; everything else passes | 1,592,073 | — | — | — |

**A second astral class on Windows**, driver-side like the first: the Arrow Flight SQL ODBC driver's Windows build keeps the low 16 bits of a non-BMP code point when it builds UTF-16 — 🚀 (U+1F680) comes back as U+F680 — on a literal and on stored data, through pyodbc and the bridge identically (sqlflite, InfluxDB 3).

**A class, not five incidents:** MySQL Connector/ODBC 8.4.0 (the only version published for Windows) reads result sets from a MySQL-wire server that lacks the character-set session variables as 3-byte `utf8`, so astral-plane characters come back as `???` on read while storage is byte-exact; the same driver reads 🚀 from MySQL, Percona, MariaDB, TiDB and Dolt, which expose the variables, and Linux passes on 9.4. Affected: databend, greptimedb, matrixone, mongodbbi, starrocks. Everything else in
their workloads passes, and their read rates are among the best on this machine.

openGauss operator note: the `adbc` role must be created *inside* the container as `omm` via
`gsql`, as the compat README's recipe says — a role created over a remote `psql` session
authenticates locally but every remote MD5 login then fails with `FATAL: Invalid
username/password,login denied`. `NO_SSPS=1` tally so far: needed on tidb, mariadb, dolt and
matrixone; already in the entry for databend and greptimedb; not needed on percona.

**A Windows-only environment fact, found by CrateDB:** Windows has no system time-zone
database, so pyarrow's timestamp-with-timezone conversion raises `ArrowInvalid: The
zoneinfo module or pytz package must be installed` until the `tzdata` PyPI package is
installed. It bit CrateDB first only because that was the first Windows entry whose
workload produces a tz-aware timestamp; any entry with a `timestamptz` column fails the
same way on a fresh box. The setup line above now includes `tzdata`.

## Host — first human run, 2026-08-24, main @ 18e1a8d

| | |
|---|---|
| OS | Windows 11 Pro, build 26200 (24H2/25H2 branch), x64 |
| CPU / RAM | Intel Core i7-8550U @ 1.80 GHz, 4 cores / 8 threads (mobile U-series); 7.7 GB RAM, ~1.2 GB free at bench time |
| Driver manager | Windows ODBC (odbc32.dll), ANSI code page 1252 |
| Build | CMake 4.4.2, MSVC 19.44.35228 (VS Build Tools 17.14, toolset 14.44), Windows SDK 10.0.26100; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release`; DLL links, **7/7 ctest** (`adbc_odbc_c_smoke` is POSIX-only by design), **zero warnings** at /W3; `cmake --install` puts `bin\libadbc_driver_odbc.dll`, `lib\adbc_driver_odbc.lib` and `etc\adbc\drivers\odbc.toml` (keyed `windows_amd64`) in the prefix |
| Python | `C:\...\Python312\python.exe` 3.12.10 x64 (no `py` launcher on this machine), adbc-driver-manager 1.12.0, pyarrow 25.0.1, pyodbc 5.3.0, pytest 9.1.1 |
| ODBC drivers (64-bit) | SQL Server; ODBC Driver 17 for SQL Server; ODBC Driver 18 for SQL Server; SQLite3 ODBC Driver (sqliteodbc_w64, SQLite 3.43.2). Access, Excel and Text drivers on this machine are **32-bit only** and cannot be loaded by a 64-bit adbcBridge — the 64-bit Access Database Engine 2016 redistributable is needed first |
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
| postgres | PASS | `PostgreSQL (via ODBC) 16.15.0` — native install, psqlodbc 18.00.0002 "PostgreSQL Unicode(x64)"; **FAIL at 18e1a8d–76223c9** with `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xe9`, see the second bug below |

### Python: adbcBridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | 209,082 (array binding 161,088) | 146,264 | 456,214 | 254,287 |
| duckdb | 74,005 (array 73,952) | 590 | 592,742 | 267,814 |
| mssql | 31,269 (array 36,209) | 21,468 | 583,138 | 270,097 |
| postgres | 86,343 (array 151,542) | 9,506 | 235,955 | 151,745 |
| mysql | 38,535 (array 42,144) | 6,145 | 397,443 | 269,100 |

All at main @ 5b932c5, x64 Release, **single sample each**, servers as native Windows installs.
**Run-to-run variance on this machine swamps build-to-build comparison**: two postgres
fetches on the same build minutes apart read 187,893 and 235,955 rows/s (26% apart) — a
4-core mobile CPU with ~1.2 GB free, thermally limited, with the database server and Defender
on the same box. DuckDB's fetch read 839,721 rows/s on the pre-fix narrow path and 592,742 on
the wide one, which *looks* like a wide-path cost and cannot be distinguished from that noise;
a real answer needs a quiet machine and repeated runs. Earlier single-sample sqlite line at
18e1a8d: 175,704 / 153,763 / 457,935 / 256,221.

Raw line: `sqlite  SQLite (via ODBC) 3.43.2  fetch=457,935/s (pyodbc 256,221/s, native —/s)  ingest=175,704/s array=173,043/s pyodbc=153,763/s`
— fetch 1.79× pyodbc, ingest 1.14×, on a 4-core mobile CPU with 1.2 GB free and a build with neither prefetch nor fan-out; not comparable with the Linux rows.

### `tests/test_sqlite.py` — FAILED at `18e1a8d`, and it was a real bug

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

At 18e1a8d–76223c9, against PostgreSQL 16.15 with `server_encoding`/`client_encoding` UTF8
(the server verifiably holding `68c3a96c6c6f` for `héllo`), `SELECT 'héllo'::varchar` raised
`UnicodeDecodeError: byte 0xe9` in pyarrow and `SELECT '日本語'::text` came back as `???`,
silently and irreversibly. The reader bound `SQL_CHAR`/`SQL_VARCHAR`/`SQL_LONGVARCHAR` as
`SQL_C_CHAR` on the assumption that the narrow path carries UTF-8 — true on unixODBC and
iODBC, never on the Windows driver manager, which transcodes narrow data to the ANSI code
page (1252 here). SQLite, SQL Server (`NVARCHAR` → wide path) and DuckDB had passed by luck
of the driver; psqlodbc, which fronts 14 of the 46, honours the conversion. Fixed in
`76223c9`: on Windows every character column is read as `SQL_C_WCHAR`, the `wchar_as_utf8`
quirk (whose premise is the same assumption) is off there, and catalog string reads go the
same way. Verified at `5b932c5`: all four probes byte-exact (`68c3a96c6c6f`,
`e697a5e69cace8aa9e`), the four entries above PASS, `tests/test_windows_text.py` 9/9,
`ctest` 7/7, zero warnings. No truncation or doubling seen on these four drivers, whose
`column_size` is in characters; drivers that report bytes are the ones to watch next.

### Five languages, five databases (`bench/*/run.sh`, ROWS=10000 FETCH_ROWS=100000 REPS=1)

**24 of 25 cells** on Windows: [`LANGUAGE_BENCHMARKS-windows.md`](LANGUAGE_BENCHMARKS-windows.md).
adbcBridge ingest / fetch rows/s, single samples on the 4-core laptop:

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
route to those three from x64 adbcBridge is to replace Office, and the alternative — a
32-bit adbcBridge build with a 32-bit Python — is out of scope: the project targets 64-bit
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
