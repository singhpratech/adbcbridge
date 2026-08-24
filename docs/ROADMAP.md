<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbcbridge roadmap

adbcbridge turns legacy database connectivity layers into Arrow-native ADBC drivers.
ODBC is the first bridge; the name leaves room for the others.

## 1. ODBC bridge (`libadbc_driver_odbc`) — now

| Milestone | Status |
|---|---|
| Query, types, NULLs, Unicode, parameters, bulk ingest, metadata, errors | done |
| Compatibility matrix: **46 databases** verified on Linux by one workload (`docs/COMPATIBILITY.md`) | done |
| Clients measured against all 46: Python, Rust, Go, Java, C# (`bench/LANGUAGE_BENCHMARKS.md`); R smoke-tested | done |
| Array-bound and multi-row bulk ingest, probed per driver, default on | done |
| Plug-and-play install: `install.sh`, driver manifest, `driver="odbc"` by name (Linux, macOS) | done |
| macOS verified on a real machine: SQLite, PostgreSQL 15, SQL Server 2022 (`bench/BENCHMARKS-macos.md`) | done |
| Windows: first build 2026-08-24 (four MSVC defects, one text-encoding bug found and fixed); SQLite verified (`bench/BENCHMARKS-windows.md`) | done |
| Windows: prefetch pipeline and parallel ingest — both pthreads, compiled out on `_WIN32`; a Win32 shim (SRWLOCK, CONDITION_VARIABLE, `_beginthreadex`) restores them | next |
| **Driver bootstrap**: `install.sh` / the Windows and macOS installers fetch the open-licence ODBC drivers a first run needs (sqliteodbc, psqlodbc, MariaDB Connector/ODBC, clickhouse-odbc) so SQLite/PostgreSQL/MySQL work with nothing else installed; vendor drivers (Oracle, Db2, SQL Server, Snowflake…) stay the user's download — their licences do not allow redistribution, and Windows already ships the SQL Server driver | next |
| ADBC Driver Foundry validation suite: driver defects D1–D14 | in progress |
| Foundry listing (adbc-drivers.org) | next |
| Windows + macOS binaries in CI releases | next |
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
  will always be faster. adbcbridge is for everything else, and for the long tail of enterprise
  databases that will never get a native driver.
