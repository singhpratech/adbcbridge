<!-- SPDX-License-Identifier: Apache-2.0 -->
# Compatibility tracker

adbcbridge speaks ODBC, so it can reach anything that has an ODBC driver — the ODBC
ecosystem covers a few hundred data sources. That is a statement about *reachability*.
This file tracks what has actually been **verified**: `tests/compat/test_matrix.py`
(types, NULLs, Unicode incl. emoji, parameters, bulk ingest, batched reads, GetObjects,
error mapping) passing against a real server or file, plus the numbers in
[`bench/MATRIX_BENCHMARKS.md`](../bench/MATRIX_BENCHMARKS.md).

Every verified driver so far needed at least one workaround (see the README matrix), so
an unverified driver should be expected to work on the generic path *and* to have a
quirk or two waiting. Bug reports with the driver name and SQLSTATE are what move rows
from the second table to the first.

## Verified

| Database | Wire / driver | Matrix | Notes |
|---|---|---|---|
| SQLite 3.45 | sqliteodbc | PASS | native delegation to `adbc_driver_sqlite` when installed |
| DuckDB | duckdb-odbc | PASS | no usable parameter arrays |
| PostgreSQL 16 | psqlodbc | PASS | native delegation to `adbc_driver_postgresql` when installed |
| MariaDB 11 | MariaDB Connector/ODBC | PASS | |
| MySQL 8.4 | MySQL Connector/ODBC | PASS | driver executes parameter arrays row by row |
| SQL Server 2022 | msodbcsql 18 | PASS | |
| Oracle 23ai Free | Instant Client ODBC | PASS | no `SQL_C_SBIGINT` |
| IBM Db2 12.1 | Db2 clidriver | PASS | 32-bit `SQLLEN` |
| ClickHouse 26 | clickhouse-odbc | PASS | one HTTP request per row on ingest |
| CockroachDB 26 | psqlodbc (PG wire) | PASS | |
| YugabyteDB 2026.1 | psqlodbc (PG wire) | PASS | |
| TimescaleDB 2.29 | psqlodbc (PG wire) | PASS | |
| CrateDB 6.4 | psqlodbc (PG wire) | PASS | eventually consistent (`REFRESH TABLE`); no binary or `DATE` column type; ingest 626 rows/s, fetch 767k rows/s |
| QuestDB 10 | psqlodbc (PG wire) | PASS | own type system behind the PG wire: standard-SQL ingest DDL, `true`/`false` boolean params, no usable parameter arrays; `SQLColumns` fails, `GetObjects` describes a zero-row SELECT instead |
| MonetDB 11.55 | MonetDBODBClib | PASS | no usable parameter arrays; `SQLEndTran` unreliable under pyodbc |
| Firebird 5 | Firebird ODBC 3.5 | PASS | `wchar_t`-sized wide strings; no usable parameter arrays |
| Microsoft Access | MDB Tools | PASS (read) | driver has no DML |

## Driver available, free server available — queued for verification

Run root-free on a developer box: free Docker image + freely downloadable Linux driver.

| Database | Wire / driver | Server | Status |
|---|---|---|---|
| Citus | psqlodbc | `citusdata/citus` | queued |
| RisingWave | psqlodbc | `risingwavelabs/risingwave` | queued |
| Materialize | psqlodbc | `materialize/materialized` | queued |
| openGauss | psqlodbc | `enmotech/opengauss` | queued |
| Google Cloud Spanner (emulator) | psqlodbc via PGAdapter | `gcr.io/cloud-spanner-emulator/emulator` | queued |
| TiDB | MySQL Connector/ODBC | `pingcap/tidb` | queued |
| Dolt | MySQL Connector/ODBC | `dolthub/dolt-sql-server` | queued |
| Percona Server | MySQL Connector/ODBC | `percona` | queued |
| Databend | MySQL Connector/ODBC | `datafuselabs/databend` | queued |
| MatrixOne | MySQL Connector/ODBC | `matrixorigin/matrixone` | queued |
| MariaDB ColumnStore | MariaDB Connector/ODBC | `mariadb/columnstore` | queued |
| MongoDB (BI Connector) | MySQL Connector/ODBC | `mongo` + `mongosqld` | queued |
| Azure SQL Edge | msodbcsql | `mcr.microsoft.com/azure-sql-edge` | queued |
| IBM Informix | Db2 clidriver (DRDA) | `icr.io/informix/informix-developer-database` | queued |
| OpenLink Virtuoso | virtodbc | `openlink/virtuoso-opensource-7` | queued |
| Apache Ignite | ignite-odbc | `apacheignite/ignite` | queued |
| Vertica CE | Vertica ODBC | `vertica/vertica-ce` | queued |
| OpenSearch | opensearch-sql-odbc | `opensearchproject/opensearch` | queued |
| Arrow Flight SQL (any server) | Flight SQL ODBC (Dremio, open source) | `voltrondata/sqlflite` | queued |
| Apache Doris | MySQL Connector/ODBC | `apache/doris` | queued (large) |
| StarRocks | MySQL Connector/ODBC | `starrocks/allin1-ubuntu` | queued (large) |
| Exasol | Exasol ODBC | `exasol/docker-db` | needs privileged container + 4 GB |
| Greenplum / Cloudberry | psqlodbc | `cloudberrydb` | queued (large) |

## Driver available, no server you can run

These exist only as hosted services (or the server is not redistributable), so verifying
them means an account with the vendor. The ODBC drivers themselves are free downloads.
Not verified; expected to work on the generic path.

| Database | Driver | What it takes |
|---|---|---|
| Snowflake | Snowflake ODBC | trial account |
| Google BigQuery | Simba ODBC for BigQuery | GCP project |
| Amazon Redshift / Athena | Amazon ODBC drivers | AWS account |
| Databricks | Simba Spark ODBC | workspace |
| Azure Synapse / SQL Database | msodbcsql 18 | Azure subscription (same driver as SQL Server — the verified one) |
| SAP HANA | HANA client ODBC | HANA Express needs 16 GB+ |
| Teradata | Teradata ODBC | Vantage Express VM |
| Cassandra / ScyllaDB | DataStax ODBC | driver download needs registration |
| Apache Hive / Impala / Spark Thrift | Cloudera / Simba ODBC | driver download needs registration |
| Trino / Presto | Starburst ODBC | driver download needs registration |
| Progress OpenEdge, Actian Zen, InterSystems IRIS/Caché, SAP ASE/IQ, Informix CSDK | vendor ODBC | vendor SDK / licence |

## Known not to work

| Database / driver | Why |
|---|---|
| H2 2.4 (PG mode) + psqlodbc | psqlodbc's connect handshake sends `SET DateStyle = 'ISO';SET extra_float_digits = 2;show transaction_isolation`, and H2's parser rejects the last two in every `MODE` (no `SHOW`, and `SET` takes only H2's own setting names). `SQLDriverConnect` therefore fails with `42001` and adbcbridge never gets a handle, so no quirk can apply. H2 has no ODBC driver of its own. See `tests/compat/README.md` for the wire-level repro |
| libSQL server (`sqld`) | No ODBC route at all. It was queued on the assumption that sqld still had the PostgreSQL wire listener `psqlodbc` could drive, but `SQLD_PG_LISTEN_ADDR` is silently ignored: sqld serves only its own HTTP/JSON protocol (Hrana) and gRPC, nothing listens on 5432, and a PG startup packet gets `ECONNRESET`. `--pg-listen-addr` is absent from `sqld --help` and no `pgwire` string is in the binary, in `latest` (0.24.33) and in `v0.22.0`, the oldest tag the registry carries — the code was dropped upstream before that. There is no libSQL ODBC driver, and sqliteodbc opens local files, not a remote sqld. See `tests/compat/README.md` |
| Elasticsearch SQL ODBC | Windows-only driver |
| Microsoft Text/Excel ODBC | Windows-only; on Linux read CSV/Parquet/Excel through DuckDB's ODBC driver instead |
