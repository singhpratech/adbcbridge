<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbcbridge roadmap

adbcbridge turns legacy database connectivity layers into Arrow-native ADBC drivers.
ODBC is the first bridge; the name leaves room for the others.

## 1. ODBC bridge (`libadbc_driver_odbc`) — now

| Milestone | Status |
|---|---|
| Query, types, NULLs, Unicode, parameters, bulk ingest, metadata, errors | done |
| Compatibility matrix: SQLite, DuckDB, PostgreSQL, MariaDB, SQL Server, Oracle, ClickHouse | done |
| Clients verified: Python, Go, Rust, C | done |
| Clients: Java, C#, R | in progress |
| More databases: MySQL, Firebird, MonetDB, CockroachDB, MS Access (MDB Tools), Db2, Vertica, Exasol | in progress |
| Array-bound bulk ingest on every database (then default on) | in progress |
| ADBC Driver Foundry validation suite: fix driver defects D1–D14 | in progress |
| Plug-and-play install: `install.sh`, driver manifest, `driver="odbc"` by name | in progress |
| Foundry listing (adbc-drivers.org) | next |
| Windows + macOS binaries in CI releases | next |
| Databases that need vendor downloads behind a login: Teradata, SAP HANA, Informix, Snowflake, Databricks/Spark (Simba), Hive | help wanted — the bridge is generic, we need someone with access to run `tests/compat/test_matrix.py` |

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
