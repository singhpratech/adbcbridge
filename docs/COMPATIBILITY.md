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
| GreptimeDB 1.1.4 (MySQL 8.4.2 wire) | MySQL Connector/ODBC (MySQL wire) | PASS | time-series store, driven over its **MySQL** wire (4002); its PostgreSQL wire (4003) cannot be reached at all — psqlodbc's connect handshake asks for `show transaction_isolation`, which GreptimeDB does not implement (only `SHOW TRANSACTION ISOLATION LEVEL`), so `SQLDriverConnect` fails as it does for H2. Every table needs a `TIME INDEX` column, so generated ingest DDL appends one (`ddl_extra_column`) plus `append_mode` (`ddl_table_options`) — outside append mode rows sharing a timestamp are merged; prepared-statement metadata types every parameter as a string and then rejects it, so `NO_SSPS=1` plus the Databend `_binary` quirk; no `DOUBLE PRECISION` type name and no `ANSI_QUOTES` (backtick-quoted identifiers); ingest 5.1k rows/s (6.8k with array binding), fetch 927k rows/s |
| InfluxDB 3 Core (InfluxDB IOx 2.0) | Flight SQL ODBC (Dremio, open source) | PASS (read) | a second Flight SQL server behind the same driver, and it needed no new driver quirk; the entry is read-only twice over (InfluxDB's SQL is query-only, so the tables are written as line protocol over HTTP, and the driver has no `SQLBindParameter`); the timestamp column is always `time` and there is no `DATE`, `DECIMAL` or binary type, so the entry aliases and casts in its `SELECT`; fetch 1.03M rows/s |
| Microsoft Access | MDB Tools | PASS (read) | driver has no DML |
| ArcadeDB 26.9 | psqlodbc (PG wire) | PASS (read) | multi-model engine behind the PG wire: no `CREATE TABLE` (a type plus one `CREATE PROPERTY` per column), so `adbc_ingest` cannot create its target and the entry runs the read side; `SQLColumns` and `SQLTables(SQL_ALL_TABLE_TYPES)` are queries its parser rejects, so `GetObjects` describes a zero-row SELECT and `GetTableTypes` falls back to the listing; `BoolsAsChar=0`, ISO-8601 `T` timestamp literals, `@rid`/`@type`/`@cat` in every `SELECT *`; also traverses a small graph (vertices, edges, `expand(out())`); fetch 332k rows/s |
| Materialize 26.38 | psqlodbc (PG wire) | PASS | streaming warehouse; PostgreSQL SQL layer, so no driver quirks -- but no `SAVEPOINT`, so psqlodbc needs `Protocol=7.4-0` for an ingest big enough to split into a second batch; `NUMERIC` is 39 digits, past decimal128, so it reads back as an exact string; also ingests into and reads back an incrementally maintained `MATERIALIZED VIEW`; ingest 6.5k rows/s (array binding), fetch 248k rows/s |
| Apache Ignite 2.17 | Ignite ODBC (built from the sources in the image) | PASS | in-memory key-value grid with a SQL engine: no prebuilt Linux driver exists, so `platforms/cpp` is built root-free (`-DWITH_ODBC=ON -DWITH_CORE=OFF`, no JVM); driver quirks handled: no wide SQL type at all (`SQL_WVARCHAR` refused by `SQLBindParameter`, `SQL_C_WCHAR` sized in `wchar_t`) so the UTF-8 narrow path, and parameter arrays that read the NULL indicator from row 0 — a NULL below the first row corrupts the batch and kills the node, so arrays are off. Server side: every table is a cache and must declare a `PRIMARY KEY`, which generated ingest DDL cannot, so `mode="create"` is impossible (`ingest_create=False`) and the entry ingests by appending into a keyed table; identifiers fold to upper case and the driver reports no quote character. Fetch 930k rows/s; append into a keyed table ~95k rows/s |
| MatrixOne 4.2 (MySQL 8.0.30 wire) | MySQL Connector/ODBC (MySQL wire) | PASS | `mysql_native_password` only, so the connector needs `PLUGIN_DIR`; a table without a PRIMARY KEY gets a hidden `__mo_fake_pk_col` that `SQLColumns` reports; a parameter bound into a `BIT` column aborts the server, so ingest sends booleans as `TINYINT`; describes a TEXT column as 5 characters (driver fix: bind a no-declared-length column at `long_bind_bytes`); ingest 4.4k rows/s, fetch 2.05M rows/s |
| StarRocks 4.1.4 (MySQL 8.0.33 wire) | MySQL Connector/ODBC (MySQL wire) | PASS | MPP columnar warehouse: no prepared statements but `SELECT`, so the connector runs with `NO_SSPS=1`, and the `_binary` date/timestamp/binary literals it then emits are sent as text instead (`temporal_binary_param_as_varchar`, restored -- it had been dead code since a bad merge); MySQL type names rejected in ingest DDL, and the portable fallback for a double is now `DOUBLE`, not `DOUBLE PRECISION`; no `ANSI_QUOTES` mode at all, so identifiers are quoted with backticks; `DECIMAL(10,3)` described at MySQL's display width (12,3); ingest 10 rows/s (~100 ms per `INSERT` is the server -- pyodbc cannot ingest here at all), fetch 406k rows/s |
| OpenSearch 3.8 (SQL plugin) | OpenSearch SQL ODBC (built from source) | PASS (read) | search engine reached through its bundled SQL plugin (`/_plugins/_sql`); the project publishes the driver for Windows and macOS only, so it is built for Linux — its never-compiled POSIX branch needed two compile fixes plus a `sem_init()` given `capacity` where WIN32/APPLE use `initial` (the pop semaphore starts full, so `pop()` corrupts an empty queue and `clear()` segfaults). Its **ANSI `SQLDriverConnect` cannot connect at all** — `CC_connect()` asks for the `SQL_ASCII` client encoding, which it does not support, unless `SQLDriverConnectW` set the unicode-driver flag — and returns `SQL_ERROR` with an empty diagnostic queue, so adbcbridge retries a connect that failed without a diagnostic through `SQLDriverConnectW`. Read-only twice over (the SQL plugin has no `INSERT`, the driver no `SQLBindParameter`), so the indices are written over the REST `_bulk` API; backtick identifiers, no `DECIMAL` or binary type, and the driver's type table has no `timestamp` entry so `ts` arrives as text; also runs `MATCH`/`MATCH_QUERY` full-text predicates; fetch ~120k rows/s |
| Apache Doris 2.1.0 (MySQL 5.7.99 wire) | MySQL Connector/ODBC (MySQL wire) | PASS | MPP analytic warehouse; reports `SQL_TC_NONE`, so the Databend quirk (`_binary` literals, portable ingest type names) applies unchanged and `NO_SSPS=1` is required (server-side prepare is point-`SELECT` only, and an `INSERT` through it throws a bare `NullPointerException` in the FE); every OLAP table needs a distribution clause, so generated ingest DDL appends `DISTRIBUTED BY RANDOM BUCKETS AUTO` plus `enable_duplicate_without_keys_by_default` (`ddl_table_options`), keyed on `@@version_comment` since `version()` is a bare MySQL number; no binary column type and no `DOUBLE PRECISION` spelling; `ANSI_QUOTES` is accepted but ignored, so identifiers are backtick-quoted; ingest 2.2k rows/s (2.3k with array binding), fetch 1.40M rows/s — pyodbc cannot ingest at all (`_binary` dates) |
| Apache Cloudberry 2.1.0-incubating (Greenplum fork) | psqlodbc (PG wire) | PASS | no driver quirks and no tolerance flags: an MPP cluster of PostgreSQL 14 segments behind one coordinator, driven by the `postgres` entry's types unchanged (and, unlike CockroachDB, needing no `PRIMARY KEY`); no Apache-published *server* image exists -- `apache/incubator-cloudberry` holds only `cbdb-build-*`/`cbdb-test-*` CI toolchains and `apache/cloudberry-db` does not exist -- so the community `woblerr/cloudberry` image is used, and it needs `--shm-size=1g` or `gpinitsystem` fails; `extra` steps cover what the standard workload cannot tell apart from `postgres`: a `DISTRIBUTED BY` table whose bulk-ingested rows occupy more than one segment plus an aggregate merged on the coordinator, and append-optimized **column-oriented** storage (read from `pg_am` as `ao_column` -- Greenplum 6's `relstorage` is gone in the PostgreSQL 14-based 2.x); ingest 9.9k rows/s (9.9k with array binding), fetch 1.69M rows/s |

## Driver available, free server available — queued for verification

Run root-free on a developer box: free Docker image + freely downloadable Linux driver.

| Database | Wire / driver | Server | Status |
|---|---|---|---|
| Google Cloud Spanner (emulator) | psqlodbc via PGAdapter | `gcr.io/cloud-spanner-emulator/emulator` | queued |
| MongoDB (BI Connector) | MySQL Connector/ODBC | `mongo` + `mongosqld` | queued |
| Vertica CE | Vertica ODBC | `vertica/vertica-ce` | queued |
| Exasol | Exasol ODBC | `exasol/docker-db` | needs privileged container + 4 GB |

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
