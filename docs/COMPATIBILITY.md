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
| MariaDB ColumnStore 23.02 | MariaDB Connector/ODBC (MariaDB wire) | PASS | columnar engine inside MariaDB 11.1: standard-SQL ingest DDL (`LONG VARCHAR`/`BIT` rejected), no `VARBINARY` column type; needs `columnstore_cache_inserts=ON` (bound-parameter inserts are ~2 rows/s without it) and `provision` to start the backend processes; ingest 14.9k rows/s (54.6k with array binding), fetch 1.41M rows/s |
| MySQL 8.4 | MySQL Connector/ODBC | PASS | driver executes parameter arrays row by row |
| Dolt 2.3.1 (MySQL 8.0.33 wire) | MySQL Connector/ODBC | PASS | `mysql_native_password` only, so the connector needs `PLUGIN_DIR` |
| SQL Server 2022 | msodbcsql 18 | PASS | |
| Oracle 23ai Free | Instant Client ODBC | PASS | no `SQL_C_SBIGINT` |
| IBM Db2 12.1 | Db2 clidriver | PASS | 32-bit `SQLLEN` |
| ClickHouse 26 | clickhouse-odbc | PASS | one HTTP request per row on ingest |
| CockroachDB 26 | psqlodbc (PG wire) | PASS | |
| Percona Server 8.4 | MySQL Connector/ODBC (MySQL wire) | PASS | drop-in MySQL fork: same entry as MySQL, no quirks; ingest 21.1k rows/s, fetch 1.18M rows/s |
| YugabyteDB 2026.1 | psqlodbc (PG wire) | PASS | |
| TimescaleDB 2.29 | psqlodbc (PG wire) | PASS | |
| Citus 14.1 (PostgreSQL 18) | psqlodbc (PG wire) | PASS | no quirks; the single container must be registered as its own worker (`citus_set_coordinator_host` + `shouldhaveshards`) before `create_distributed_table()` works; ingest 107k rows/s (array binding), fetch 1.86M rows/s |
| CrateDB 6.4 | psqlodbc (PG wire) | PASS | eventually consistent (`REFRESH TABLE`); no binary or `DATE` column type; ingest 626 rows/s, fetch 767k rows/s |
| QuestDB 10 | psqlodbc (PG wire) | PASS | own type system behind the PG wire: standard-SQL ingest DDL, `true`/`false` boolean params, no usable parameter arrays; `SQLColumns` fails, `GetObjects` describes a zero-row SELECT instead |
| RisingWave 3.0 | psqlodbc (PG wire) | PASS | no driver quirks; server side: no type modifiers at all (`VARCHAR`, `NUMERIC` unqualified) and writes are visible only after `FLUSH`; ingest 983 rows/s, fetch 991k rows/s |
| MonetDB 11.55 | MonetDBODBClib | PASS | no usable parameter arrays; `SQLEndTran` unreliable under pyodbc |
| TiDB 7.5 | MySQL Connector/ODBC (MySQL wire) | PASS | tarball driver needs `PLUGIN_DIR` for `mysql_native_password` |
| Firebird 5 | Firebird ODBC 3.5 | PASS | `wchar_t`-sized wide strings; no usable parameter arrays |
| Databend | MySQL Connector/ODBC (MySQL wire) | PASS | no prepared statements (`NO_SSPS=1`); `_binary` literals; MySQL type names in ingest DDL |
| Azure SQL Edge 16.0 | msodbcsql 18 | PASS | SQL Server 2022 engine; no quirks |
| OpenLink Virtuoso 7.2 | Virtuoso ODBC (`virtodbc.so`) | PASS | ODBC-native server; no `SQL_C_WCHAR` (UTF-8 narrow path), no `SQL_C_SBIGINT`, date parameter arrays repeat row 0; no `BOOLEAN` type; ingest 11.9k rows/s, fetch 1.04M rows/s |
| openGauss 6.0 | psqlodbc (PG wire) | PASS | no quirks: a PostgreSQL 9.2 fork, driven by the `postgres` entry's types unchanged; server-side setup only (`CAP_SYS_NICE` for the MOT engine's `mbind()`, `max_process_memory` >= 2 GB, and a role created after start-up because the initial user cannot log in remotely); ingest 9.5k rows/s (36.9k with array binding), fetch 1.30M rows/s |
| IBM Informix 15.0.1 | Db2 clidriver (DRDA) | PASS | same `libdb2.so` as Db2, keyed on `SQL_DBMS_NAME`: no `SQL_C_WCHAR` params (UTF-8 narrow path), no `SQL_C_BIT` params; 32-bit `SQLLEN`; `BYTE` described as IBM's `SQL_BLOB` (-98); server side: `GL_USEGLU=1` for 4-byte UTF-8, `DELIMIDENT=y` for quoted identifiers, `FRACTION(5)` timestamps; ingest 15.6k rows/s (130k array), fetch 751k rows/s |
| Arrow Flight SQL (sqlflite 1.5.5 / DuckDB 1.1.1) | Flight SQL ODBC (Dremio, open source) | PASS (read) | driver has no `SQLBindParameter` at all, so no writes; `SQLColumns` segfaults on the first `SQLFetch`, so `GetObjects` describes a zero-row SELECT instead (`no_sql_columns`); every `DECIMAL` described as (19,0); fetch 1.32M rows/s |
| Microsoft Access | MDB Tools | PASS (read) | driver has no DML |
| Materialize 26.38 | psqlodbc (PG wire) | PASS | streaming warehouse; PostgreSQL SQL layer, so no driver quirks -- but no `SAVEPOINT`, so psqlodbc needs `Protocol=7.4-0` for an ingest big enough to split into a second batch; `NUMERIC` is 39 digits, past decimal128, so it reads back as an exact string; also ingests into and reads back an incrementally maintained `MATERIALIZED VIEW`; ingest 6.5k rows/s (array binding), fetch 248k rows/s |
| Google Cloud Spanner (emulator + PGAdapter 0.55) | psqlodbc (PG wire) | PASS | two driver quirks, both keyed on a PGAdapter-only setting because `version()` just says PostgreSQL 14.1: psqlodbc inlines a parameter array's timestamps as `'...'::timestamp`, a type Spanner does not have, so a batch binding a timestamp goes row-at-a-time (`no_timestamp_param_arrays`); and every Spanner table needs a PRIMARY KEY, so generated ingest DDL adds a surrogate `GENERATED BY DEFAULT AS IDENTITY` column (`ingest_key_column`). Server side: no 32-bit integer, no `TIMESTAMP WITHOUT TIME ZONE` (so `ts` reads back zone-aware), no modifier on `NUMERIC`, no DDL inside a transaction; also ingests into and reads back an `INTERLEAVE IN PARENT` child table. Writes are slow and get slower with batch size: ingest 233 rows/s (287 with array binding), fetch 36.0k rows/s at `--rows 300 --fetch-rows 2000`, which is the size this entry is benchmarked at |
| MatrixOne 4.2 (MySQL 8.0.30 wire) | MySQL Connector/ODBC (MySQL wire) | PASS | `mysql_native_password` only, so the connector needs `PLUGIN_DIR`; a table without a PRIMARY KEY gets a hidden `__mo_fake_pk_col` that `SQLColumns` reports; a parameter bound into a `BIT` column aborts the server, so ingest sends booleans as `TINYINT`; describes a TEXT column as 5 characters (driver fix: bind a no-declared-length column at `long_bind_bytes`); ingest 4.4k rows/s, fetch 2.05M rows/s |

## Driver available, free server available — queued for verification

Run root-free on a developer box: free Docker image + freely downloadable Linux driver.

| Database | Wire / driver | Server | Status |
|---|---|---|---|
| MongoDB (BI Connector) | MySQL Connector/ODBC | `mongo` + `mongosqld` | queued |
| Apache Ignite | ignite-odbc | `apacheignite/ignite` | queued |
| Vertica CE | Vertica ODBC | `vertica/vertica-ce` | queued |
| OpenSearch | opensearch-sql-odbc | `opensearchproject/opensearch` | queued |
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
