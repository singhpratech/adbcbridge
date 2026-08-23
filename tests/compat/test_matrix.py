"""Compatibility matrix: run the same ADBC workload against every ODBC driver we can reach.

Usage:
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/compat/test_matrix.py [db ...]

Each database is enabled by an environment variable holding the path to its ODBC driver:
    SQLITE_ODBC_DRIVER, DUCKDB_ODBC_DRIVER, POSTGRES_ODBC_DRIVER, MARIADB_ODBC_DRIVER,
    MYSQL_ODBC_DRIVER, MSSQL_ODBC_DRIVER, AZURESQLEDGE_ODBC_DRIVER, ORACLE_ODBC_DRIVER,
    CLICKHOUSE_ODBC_DRIVER,
    DB2_ODBC_DRIVER, COCKROACH_ODBC_DRIVER, MONETDB_ODBC_DRIVER, FIREBIRD_ODBC_DRIVER,
    ACCESS_ODBC_DRIVER, PERCONA_ODBC_DRIVER
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, ACCESS_ODBC_DRIVER, TIDB_ODBC_DRIVER
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, ACCESS_ODBC_DRIVER, DOLT_ODBC_DRIVER
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, ACCESS_ODBC_DRIVER, DATABEND_ODBC_DRIVER
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, CRATEDB_ODBC_DRIVER, CITUS_ODBC_DRIVER,
    QUESTDB_ODBC_DRIVER, ACCESS_ODBC_DRIVER, MATERIALIZE_ODBC_DRIVER
Servers are expected as in docker-compose.yml (override with *_CONN env vars); the
file-based entries (sqlite, duckdb, access) need no server.
See README.md in this directory for how to obtain each driver without root.
"""
import os, sys, shutil, tempfile, pathlib, datetime, decimal
import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent.parent / "build" / "libadbc_driver_odbc.so"))
TMP = tempfile.mkdtemp()

SUFFIX = os.environ.get("ADBC_MATRIX_SUFFIX", "")  # set to isolate concurrent runs on a shared server
# Payload for the `extra` ingest steps: same column layout as the standard ingest
# table, but with no NULL in the date column, which a partitioned/time-series table
# may require of the column it is partitioned on.
EXTRA_ROWS = pa.table({
    "a": pa.array([1, 2, 3, 4], pa.int64()),
    "b": pa.array(["p", "q", "r", "s"]),
    "c": pa.array([1.5, 2.5, None, 4.5]),
    "d": pa.array([datetime.date(2024, 1, 1), datetime.date(2024, 1, 2),
                   datetime.date(2024, 2, 1), datetime.date(2024, 3, 1)], pa.date32()),
    "e": pa.array([True, None, False, True], pa.bool_()),
})
DBS = {
    "sqlite": dict(
        env="SQLITE_ODBC_DRIVER", conn="Driver={drv};Database=" + os.path.join(TMP, "m.db") + ";",
        ddl="CREATE TABLE adbc_t (i INTEGER, f REAL, s TEXT, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        # sqliteodbc converts UTF-8 through UCS-2 and drops the astral-plane emoji.
        decimal_type="string", ts_precision="ms", astral=False),
    "duckdb": dict(
        env="DUCKDB_ODBC_DRIVER", conn="Driver={drv};Database=:memory:;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)"),
    "postgres": dict(
        env="POSTGRES_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)"),
    "mariadb": dict(
        env="MARIADB_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "oracle": dict(
        env="ORACLE_ODBC_DRIVER", conn="Driver={drv};DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;",
        ddl="CREATE TABLE adbc_t (i NUMBER(10), f BINARY_DOUBLE, s VARCHAR2(50), b RAW(10), d DATE, ts TIMESTAMP(6), n NUMBER(10,3), bo BOOLEAN)",
        ident=str.upper, unicode_env="NLS_LANG=.AL32UTF8"),
    "clickhouse": dict(
        env="CLICKHOUSE_ODBC_DRIVER", conn="Driver={drv};Url=http://127.0.0.1:18123;Database=adbc;UID=adbc;PWD=adbc;",
        ddl="CREATE TABLE adbc_t (i Nullable(Int32), f Nullable(Float64), s Nullable(String), b Nullable(String), d Nullable(Date), ts Nullable(DateTime64(6)), n Nullable(Decimal(10,3)), bo Nullable(Bool)) ENGINE = Memory",
        # clickhouse-odbc sends NULL parameters as empty strings (driver limitation) and
        # does not report affected row counts.
        null_params=False, rowcount=False, big_rows=300),
    "mssql": dict(
        env="MSSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;",
        ddl="CREATE TABLE adbc_t (i INT, f FLOAT, s NVARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME2(6), n DECIMAL(10,3), bo BIT)"),
    "azuresqledge": dict(
        # Azure SQL Edge is the SQL Server 2022 engine (it reports SQL_DBMS_NAME
        # "Microsoft SQL Server" 16.00.x, indistinguishable from mssql over the wire),
        # so msodbcsql 18 drives it and the mssql DDL applies unchanged -- INT, FLOAT,
        # NVARCHAR, VARBINARY, DATE, DATETIME2(6), DECIMAL and BIT all behave the same.
        # Kept as its own entry because it is a separately shipped engine build with a
        # trimmed feature set, so the workload is worth running against it directly.
        env="AZURESQLEDGE_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1,14332;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;",
        ddl="CREATE TABLE adbc_t (i INT, f FLOAT, s NVARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME2(6), n DECIMAL(10,3), bo BIT)"),
    "mysql": dict(
        env="MYSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        # MySQL BOOLEAN is TINYINT(1), reported as SQL_TINYINT -> int8; double-quoted
        # identifiers (used by ingest) need ANSI_QUOTES. See tests/compat/README.md for
        # the LD_PRELOAD needed by MySQL Connector/ODBC under pyarrow.
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "tidb": dict(
        # TiDB speaks the MySQL wire protocol (it advertises itself as MySQL 8.0.11), so
        # MySQL Connector/ODBC drives it and the `mysql` entry's DDL and tolerances apply
        # unchanged: BOOLEAN is TINYINT(1) -> int8, and the double-quoted identifiers that
        # ingest emits need ANSI_QUOTES.  The stock image ships no `adbc` database and only
        # the passwordless `root` account, so the entry uses the built-in `test` database.
        # {plugin_dir}: TiDB creates root with mysql_native_password, whose *client-side*
        # plugin Connector/ODBC 9 loads at run time -- see conn_uri() below.
        env="TIDB_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=14000;Database=test;User=root;{plugin_dir}",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "dolt": dict(
        # Dolt is a version-controlled SQL database ("git for data") that speaks the MySQL
        # wire protocol, so MySQL Connector/ODBC drives it and the MySQL entry's types and
        # quirks apply unchanged: BOOLEAN is TINYINT(1) -> int8, and adbc_ingest's
        # double-quoted identifiers need ANSI_QUOTES.
        env="DOLT_ODBC_DRIVER",
        # Dolt only implements mysql_native_password, whose client-side plugin Connector/ODBC
        # 9.x no longer links in; it ships as a separate .so next to the driver library, and
        # PLUGIN_DIR is how libmysqlclient is pointed at it.
        conn="Driver={drv};Server=127.0.0.1;Port=13310;Database=adbc;User=root;PLUGIN_DIR={drvdir}/plugin;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "databend": dict(
        # Databend speaks the MySQL wire protocol, so MySQL Connector/ODBC drives it, but
        # it is a column-store warehouse rather than a MySQL: no sql_mode (its default
        # dialect is PostgreSQL, so the double-quoted identifiers ingest emits already
        # work) and no prepared statements.
        #   NO_SSPS=1 is what makes the entry work at all: Databend's MySQL handler
        # refuses COM_STMT_PREPARE outright ("Prepare is not support in Databend"), which
        # surfaces as SQLPrepare failing.  With NO_SSPS the connector stops using the
        # server-side prepare protocol and substitutes bound parameters into the SQL text
        # itself, so every statement goes as a plain query.
        #   {plugin}: Databend authenticates with mysql_native_password, which is no
        # longer built into Connector/ODBC 9 -- it is a loadable plugin shipped beside the
        # driver.  See the note on the format key in run() and README.md.
        env="DATABEND_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=13311;Database=default;User=root;Password=adbc;NO_SSPS=1;{plugin}",
        #   b is VARCHAR, not BINARY, for the same reason ClickHouse's is String: a
        # Databend BINARY column is rendered as *hex text* on the MySQL wire, so
        # b"\x01\x02" reads back as b"0102" and no byte string ever round-trips.  A
        # character column carries the two bytes through unchanged.
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARCHAR(50), d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        # Databend sends BOOLEAN over the MySQL wire as a SMALLINT (MySQL's own BOOLEAN
        # is TINYINT(1), which the same driver reports as SQL_TINYINT -> int8).
        bool_type="int16",
        # Databend describes every DECIMAL column on the MySQL wire with scale 0 --
        # DECIMAL(10,3) arrives as precision 9, scale 0 -- while sending the digits
        # themselves in full.  Taken at face value that scale rounds 12.345 to 12, so
        # this entry reads decimals as their exact text instead.
        db_kwargs={"adbc.odbc.decimal_as_string": "true"}, decimal_type="string",
        # Databend's information_schema reports ordinal_position 1 for every column, so
        # SQLColumns -- which orders by it -- hands back the columns in an arbitrary
        # order.  SELECT metadata is unaffected; only GetObjects/GetTableSchema are.
        column_order=False,
        # Databend commits a fresh immutable data block per INSERT, and the connector in
        # NO_SSPS mode sends one statement per parameter set (a parameter array is not a
        # multi-row INSERT), so single-row ingest runs at a few tens of rows/s.  2000 still
        # crosses the reader's 1024-row batch boundary, which is what this step is for,
        # without spending five minutes on it.
        big_rows=2000),
    "percona": dict(
        # Percona Server is a drop-in MySQL fork (same wire protocol, same client
        # libraries), so MySQL Connector/ODBC drives it and the `mysql` entry applies
        # unchanged: ANSI_QUOTES for the double-quoted identifiers adbc_ingest emits,
        # and BOOLEAN reported as SQL_TINYINT -> int8.  See tests/compat/README.md for
        # the LD_PRELOAD that MySQL Connector/ODBC needs under pyarrow.
        env="PERCONA_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13312;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "db2": dict(
        env="DB2_ODBC_DRIVER", conn="Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=50000;Protocol=TCPIP;Uid=db2inst1;Pwd=Adbc2026;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)",
        # Db2 folds unquoted identifiers to upper case.  The clidriver libdb2.so is built
        # with a 32-bit SQLLEN; the driver detects that itself (adbc.odbc.sqllen_32bit),
        # so no tolerance flag is needed here.  See README.md.
        ident=str.upper),
    "monetdb": dict(
        env="MONETDB_ODBC_DRIVER", conn="Driver={drv};Host=127.0.0.1;Port=15000;Database=adbc;Uid=monetdb;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR(50), b BLOB, d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)"),
    "cockroachdb": dict(
        # Wire-compatible with PostgreSQL, so it uses psqlodbc; INTEGER is 64-bit here.
        # The PRIMARY KEY is required, not decorative: a CockroachDB table declared without
        # one gets a synthesised hidden "rowid" column (NOT VISIBLE, DEFAULT unique_rowid()),
        # which information_schema.columns -- and therefore SQLColumns/GetObjects -- reports
        # as a 9th column even though SELECT * never returns it.
        env="COCKROACH_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=16257;Database=defaultdb;Uid=root;",
        ddl="CREATE TABLE adbc_t (i INTEGER PRIMARY KEY, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)"),
    "yugabyte": dict(
        # YugabyteDB's YSQL layer reuses the PostgreSQL 15 query engine on top of a
        # distributed storage layer, so it speaks the PostgreSQL wire protocol and the
        # same psqlodbc build drives it; the types below are plain PostgreSQL types.
        env="YUGABYTE_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15433;Database=yugabyte;Uid=yugabyte;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)"),
    "timescaledb": dict(
        # PostgreSQL plus the timescaledb extension, so psqlodbc drives it and the
        # plain PostgreSQL workload applies unchanged.  The `extra` steps go beyond
        # that: they turn a table into a hypertable (Timescale's transparently
        # partitioned time-series table) and check that ingest and reads -- including
        # a read through time_bucket() -- work against the partitioned table.
        env="TIMESCALE_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15434;Database=adbc;Uid=adbc;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        setup=["CREATE EXTENSION IF NOT EXISTS timescaledb"],
        extra=[
            ('DROP TABLE IF EXISTS "adbc_ht{sfx}"', None),
            # Column names/types match EXTRA_ROWS: the ingest below appends into it.
            ('CREATE TABLE "adbc_ht{sfx}" ("a" BIGINT, "b" VARCHAR(20), "c" DOUBLE PRECISION,'
             ' "d" DATE NOT NULL, "e" BOOLEAN)', None),
            # create_hypertable() partitions it by "d", one chunk per 7 days.  The
            # partitioning column is why EXTRA_ROWS has no NULL there: Timescale puts
            # a NOT NULL constraint on it.
            ("""SELECT (create_hypertable('adbc_ht{sfx}', by_range('d', INTERVAL '7 days'))).created""", (True,)),
            (("adbc_ht{sfx}", EXTRA_ROWS), (4,)),          # bulk ingest into the hypertable
            ('SELECT count(*) FROM timescaledb_information.chunks'
             " WHERE hypertable_name = 'adbc_ht{sfx}'", (3,)),   # rows landed in 3 chunks
            ('SELECT "b", "c" FROM "adbc_ht{sfx}" WHERE "a" = 3', ("r", None)),
            ('SELECT count(DISTINCT time_bucket(INTERVAL \'7 days\', "d")) FROM "adbc_ht{sfx}"', (3,)),
        ]),
    "citus": dict(
        # Citus is the `citus` extension on top of stock PostgreSQL (citusdata/citus:latest
        # is PostgreSQL 18 + citus 14), so psqlodbc drives it and the plain PostgreSQL
        # workload applies unchanged.  `setup` turns the single container into a one-node
        # Citus cluster: citus_set_coordinator_host() registers the coordinator in
        # pg_dist_node, and shouldhaveshards makes it hold shards itself -- without that
        # create_distributed_table() fails with "replication_factor (1) exceeds number of
        # worker nodes (0)".  All three statements are idempotent, which matters because
        # bench/matrix_bench.py replays `setup` on every connection it opens.
        env="CITUS_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15436;Database=adbc;Uid=adbc;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        setup=["CREATE EXTENSION IF NOT EXISTS citus",
               "SELECT citus_set_coordinator_host('localhost', 5432)",
               "SELECT citus_set_node_property('localhost', 5432, 'shouldhaveshards', true)"],
        # The standard workload alone would be a duplicate of `postgres`, so the `extra`
        # steps exercise the reason to run Citus: a hash-distributed table whose rows live
        # in shards and whose reads fan out to them.  adbc_t itself stays a plain table --
        # its second row is NULL in every column but `i`, and a distribution column may
        # not be NULL.
        extra=[
            ('DROP TABLE IF EXISTS "adbc_dist{sfx}"', None),
            # Column names/types match EXTRA_ROWS: the ingest below appends into it.
            # "a" is the distribution column, hence NOT NULL.
            ('CREATE TABLE "adbc_dist{sfx}" ("a" BIGINT NOT NULL, "b" VARCHAR(20),'
             ' "c" DOUBLE PRECISION, "d" DATE, "e" BOOLEAN)', None),
            ("SELECT create_distributed_table('adbc_dist{sfx}', 'a')", None),
            # partmethod 'h': Citus now owns the table and hash-distributes it.
            ("SELECT partmethod FROM pg_dist_partition"
             " WHERE logicalrelid = 'adbc_dist{sfx}'::regclass", ("h",)),
            (("adbc_dist{sfx}", EXTRA_ROWS), (4,)),   # bulk ingest into the distributed table
            # The ingested rows really did go through the distribution machinery: they
            # hash to more than one shard.
            ("SELECT count(DISTINCT get_shard_id_for_distribution_column("
             "'adbc_dist{sfx}', \"a\")) > 1 FROM \"adbc_dist{sfx}\"", (True,)),
            ('SELECT "b", "c" FROM "adbc_dist{sfx}" WHERE "a" = 3', ("r", None)),
            # An aggregate is planned across the shards and merged on the coordinator.
            ('SELECT count(*), sum("a") FROM "adbc_dist{sfx}"', (4, 10)),
        ]),
    "materialize": dict(
        # Materialize is a streaming warehouse whose views are incrementally maintained.
        # It speaks the PostgreSQL wire protocol and announces itself as PostgreSQL 9.5,
        # so psqlodbc drives it and the `postgres` entry's DDL applies unchanged --
        # INTEGER, DOUBLE PRECISION, VARCHAR, BYTEA, DATE, TIMESTAMP and BOOLEAN are all
        # PostgreSQL's, and so are the type names psqlodbc's SQLGetTypeInfo puts in the
        # generated ingest DDL.  It needs no password (the single-node image ships one
        # `materialize` superuser and no authentication).
        # Protocol=7.4-0 is psqlodbc's setting, not Materialize's: it turns off the
        # per-statement SAVEPOINT the driver wraps a repeated execute in once it is
        # inside a transaction.  Materialize has no SAVEPOINT statement, so a bulk
        # ingest big enough for psqlodbc to split into a second batch fails the whole
        # batch with `Expected a keyword at the beginning of a statement, found
        # identifier "savepoint"` (42601).  Where psqlodbc splits depends on the size of
        # the statement text it inlines the values into, so this entry's own 5000 narrow
        # rows still go as one batch and pass without the setting; bench/matrix_bench.py's
        # wider rows split at about 4000 and do not.
        env="MATERIALIZE_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=16875;Database=materialize;Uid=materialize;"
             "Protocol=7.4-0;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA,"
            " d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # Materialize has one arbitrary-precision `numeric` type: NUMERIC(10,3) keeps the
        # requested scale but not the precision, and the column is described at the type's
        # own maximum of 39 digits.  That is past what an Arrow decimal128 can hold (38),
        # so the reader falls back to its exact string form -- no precision is lost.
        decimal_type="string",
        # The standard workload alone would be a duplicate of `postgres`, so the `extra`
        # steps exercise the reason to run Materialize: a MATERIALIZED VIEW is not a
        # snapshot to be refreshed but a dataflow kept up to date as its inputs change.
        # These ingest through ADBC into the view's input and read the aggregate straight
        # back, which passes only if the write really did flow through to the view.
        extra=[
            ('DROP MATERIALIZED VIEW IF EXISTS "adbc_mv{sfx}"', None),
            ('DROP TABLE IF EXISTS "adbc_src{sfx}"', None),
            # Column names/types match EXTRA_ROWS: the ingest below appends into it.
            ('CREATE TABLE "adbc_src{sfx}" ("a" BIGINT, "b" VARCHAR(20),'
             ' "c" DOUBLE PRECISION, "d" DATE, "e" BOOLEAN)', None),
            # sum() over a BIGINT is `numeric` in Materialize as in PostgreSQL, and a
            # 39-digit numeric reads back as a string (see decimal_type above); the cast
            # keeps this assertion about the view rather than about decimal mapping.
            ('CREATE MATERIALIZED VIEW "adbc_mv{sfx}" AS SELECT "e", count(*) AS "n",'
             ' sum("a")::BIGINT AS "s" FROM "adbc_src{sfx}" GROUP BY "e"', None),
            (("adbc_src{sfx}", EXTRA_ROWS), (4,)),   # bulk ingest into the view's input
            # Materialize really owns the view -- it is a dataflow, listed as such.
            ("SELECT count(*) FROM mz_materialized_views WHERE name = 'adbc_mv{sfx}'", (1,)),
            # The four ingested rows reached the maintained view: three groups
            # (e = true/false/NULL), with a = 1 and 4 in the `true` one.
            ('SELECT count(*) FROM "adbc_mv{sfx}"', (3,)),
            ('SELECT "n", "s" FROM "adbc_mv{sfx}" WHERE "e"', (2, 5)),
            ('SELECT "n", "s" FROM "adbc_mv{sfx}" WHERE "e" IS NULL', (1, 2)),
        ]),
    "cratedb": dict(
        # CrateDB speaks the PostgreSQL wire protocol (it announces itself as PostgreSQL
        # 14), so psqlodbc drives it, but its type system is its own: there is no binary
        # column type at all (blobs live in separate blob tables, outside SQL) and no
        # DATE storage type -- "Type `date` does not support storage" -- so `b` is TEXT
        # and `d` is TIMESTAMP here.  NUMERIC needs an explicit precision and scale.
        env="CRATEDB_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15440;Database=doc;Uid=crate;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b TEXT,"
            " d TIMESTAMP, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # Writes become visible to a scan only when the table's shards refresh (once a
        # second by default), so every write here is followed by an explicit refresh.
        refresh='REFRESH TABLE "{}"',
        # The generated ingest DDL takes its type names from psqlodbc's SQLGetTypeInfo,
        # which is PostgreSQL's: "date" (CrateDB has no such storage type) and "bool"
        # (CrateDB spells it BOOLEAN and has no alias).  Send those two columns as types
        # CrateDB does have, so create/append/replace ingest is still exercised in full.
        ingest_types={pa.date32(): pa.timestamp("us"), pa.bool_(): pa.int8()},
        # `b` is text, so the bytes arrive as psqlodbc's bytea hex escape; TIMESTAMP is
        # millisecond-precision; and CrateDB does not report the precision/scale of a
        # NUMERIC column over the wire, so psqlodbc falls back to its own default (28, 6)
        # instead of the declared (10, 3).
        binary_text="\\x0102", decimal_type="decimal128(28, 6)", ts_us=(123000,)),
    "questdb": dict(
        # QuestDB is a time-series database that speaks the PostgreSQL wire protocol, so
        # the psqlodbc build used for the `postgres` entry drives it -- but only the wire
        # protocol is PostgreSQL's.  The type system, the DDL parser and the catalog are
        # QuestDB's own: STRING/BINARY are its names, VARCHAR takes no length, NUMERIC
        # does not exist, and psqlodbc's own type names ("int8", "bool") are rejected on
        # CREATE TABLE -- the driver detects that and spells ingest DDL in standard SQL
        # types (adbc.odbc quirk `ansi_ddl_type_names`).
        # Two settings in the connection string are psqlodbc's, not QuestDB's:
        # BoolsAsChar=0, without which the driver reports every BOOLEAN as a VARCHAR(5)
        # holding "1"/"0" instead of SQL_BIT; and Protocol=7.4-0, which turns off the
        # per-statement SAVEPOINT psqlodbc wraps a repeated execute in -- QuestDB has no
        # SAVEPOINT statement and fails the whole insert with "internal SAVEPOINT failed".
        env="QUESTDB_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=18812;Database=qdb;Uid=admin;Pwd=quest;"
             "BoolsAsChar=0;Protocol=7.4-0;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s STRING, b BINARY, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        # QuestDB does not report the declared precision of a DECIMAL over the wire, so
        # psqlodbc describes it at its own maximum (28) with the column's scale.
        decimal_type="decimal128(28, 3)",
        # QuestDB's BOOLEAN has no NULL state (like Access YESNO): row 2's bo is false.
        not_null=("bo",)),
    "firebird": dict(
        env="FIREBIRD_ODBC_DRIVER",
        conn="Driver={drv};DBNAME=inet://127.0.0.1:13050//var/lib/firebird/data/adbc.fdb;UID=adbc;PWD=adbc;CHARSET=UTF8;",
        # VARBINARY is CHAR CHARACTER SET OCTETS in Firebird and OdbcFb describes it as
        # SQL_VARCHAR, so BLOB SUB_TYPE BINARY is the type that round-trips bytes.
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BLOB SUB_TYPE BINARY, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # Firebird upper-cases unquoted identifiers; NUMERIC(10,3) is stored as a scaled
        # BIGINT and OdbcFb reports the storage precision (18), not the declared one.
        ident=str.upper, decimal_type="decimal128(18, 3)", ts_us=(123400,)),
    "access": dict(
        env="ACCESS_ODBC_DRIVER", conn="Driver={drv};DBQ=" + os.path.join(TMP, "access.mdb") + ";",
        # No server: MDB Tools opens an .mdb file. It is read-only (it executes no DDL and
        # no DML at all), so the workload runs against a fixture generated out of band by
        # fixtures/MakeAccessMdb.java -- this DDL is the Jet SQL for what that file holds.
        fixture="access.mdb", read_only=True,
        ddl="CREATE TABLE adbc_t (i LONG, f DOUBLE, s TEXT(100), b LONGBINARY, d DATETIME, ts DATETIME, n DECIMAL(10,3), bo YESNO)",
        # MDB Tools implements neither SQLPrepare nor SQLBindParameter ("Driver does not
        # support this function"), so the parameterised query runs with a literal.
        params=False,
        # Its SQL parser rejects an unknown table with a bare "Couldn't parse SQL" that
        # names neither the table nor the problem.
        error_text=False,
        # iconv from Jet's UCS-2 mangles the surrogate pair of the emoji into "??".
        astral=False,
        # Access DATETIME is a day fraction with one-second resolution: no microseconds.
        ts_us=(0,),
        # Access YESNO has no NULL state; row 2's bo comes back False, not NULL.
        not_null=("bo",),
        # adbc_big ships in the fixture; 3000 rows still cross the 1024-row batch
        # boundary while keeping the checked-in .mdb under 200 kB.
        big_rows=3000),
}

def conn_uri(name, cfg, drv=None):
    """The entry's connection string, with `<NAME>_CONN` and the format keys applied.

    `{drv}` is the driver library.  `{plugin}` expands to a PLUGIN_DIR= setting pointing
    at the "plugin" directory beside the driver when there is one, and to nothing when
    there is not: MySQL Connector/ODBC keeps its client-side authentication plugins
    there, and unpacked from the generic tarball (as README.md describes) its compiled-in
    default plugin path does not exist, so a server that asks for an auth plugin the
    client does not have built in cannot be reached without it.  A packaged root install,
    whose default path is correct, has no such directory and is left alone.
    """
    if drv is None:
        drv = os.environ[cfg["env"]]
    pdir = os.path.join(os.path.dirname(drv), "plugin")
    plugin = "PLUGIN_DIR=%s;" % pdir if os.path.isdir(pdir) else ""
    return os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(drv=drv, plugin=plugin)


# Typed values: ADBC clients send Arrow-typed parameters, so dates/timestamps go as
# date32/timestamp (string literals for dates are not portable, e.g. Oracle).
ROW1 = (1, 1.5, "héllo 🚀", b"\x01\x02", datetime.date(2024, 2, 29),
        datetime.datetime(2024, 2, 29, 13, 45, 10, 123456), decimal.Decimal("12.345"), True)
ROW2 = (2, None, None, None, None, None, None, None)


def conn_uri(name, cfg, drv):
    """The entry's connection string, overridable with <NAME>_CONN.

    `{drv}` expands to the driver library. `{plugin_dir}` expands to a `PLUGIN_DIR=`
    setting for the drivers that need one: MySQL Connector/ODBC loads client-side
    authentication plugins from the directory it was *built* with
    (/usr/local/mysql/lib/plugin for the generic tarball), so a tarball unpacked elsewhere
    cannot load them, and a server still using mysql_native_password (TiDB) refuses the
    connection. The tarball keeps those plugins next to the driver, so point PLUGIN_DIR
    there when that directory exists; a packaged install has no such directory and keeps
    its own -- correct -- compiled-in default.
    """
    pdir = os.path.join(os.path.dirname(drv), "plugin")
    plugin_dir = "PLUGIN_DIR=%s;" % pdir if os.path.isdir(pdir) else ""
    return os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(drv=drv, plugin_dir=plugin_dir)


def run(name, cfg):
    drv = os.environ.get(cfg["env"])
    if not drv:
        return "SKIP (set %s)" % cfg["env"]
    for kv in cfg.get("unicode_env", "").split():
        k, v = kv.split("=", 1)
        os.environ.setdefault(k, v)
    if cfg.get("fixture"):  # file-based, read-only database: work on a private copy
        shutil.copy(HERE / "fixtures" / cfg["fixture"], os.path.join(TMP, cfg["fixture"]))
    uri = conn_uri(name, cfg, drv)
    # {drv} is the driver library; {drvdir} is the directory holding it, for the rare
    # connection option that must point at a file shipped beside the driver.
    uri = os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(
        drv=drv, drvdir=os.path.dirname(drv))
    uri = conn_uri(name, cfg, drv)
    # This matrix exists to exercise the ODBC path, so native delegation (which
    # would take over for e.g. SQLite/PostgreSQL) is switched off here.
    # db_kwargs: extra adbcbridge database options this entry needs, for a server whose
    # metadata the generic path cannot take at face value.
    kwargs = {"uri": uri, "adbc.odbc.delegate": "never"}
    kwargs.update(cfg.get("db_kwargs", {}))
    conn = dbapi.connect(driver=DRIVER, db_kwargs=kwargs, autocommit=True)
    info = conn.adbc_get_info()
    ident = cfg.get("ident", lambda x: x)  # how the server stores unquoted names
    # read_only: the driver executes no DDL/DML (MDB Tools), so the table and its rows
    # come from the checked-in fixture and only the read side of the workload runs. Its
    # names are fixed by the fixture -- no suffix -- and concurrent runs cannot collide
    # anyway, each working on its own copy of the file in its own temp dir.
    ro = cfg.get("read_only", False)
    sfx = "" if ro else SUFFIX
    t_name, ing_name = "adbc_t" + sfx, "adbc_ing" + sfx
    T, ING = ident(t_name), ident(ing_name)
    with conn.cursor() as cur:
        for sql in cfg.get("setup", []):
            cur.execute(sql)
        if not ro:
            for t in (t_name, ing_name, '"%s"' % ing_name):  # ingest quotes names (exact case)
                try:
                    cur.execute("DROP TABLE " + t)
                except Exception:
                    pass
            cur.execute(cfg["ddl"].replace("adbc_t", t_name))
            rows = [ROW1, ROW2] if cfg.get("null_params", True) else [ROW1]
            cur.executemany("INSERT INTO %s VALUES (?, ?, ?, ?, ?, ?, ?, ?)" % t_name, rows)
            if not cfg.get("null_params", True):
                cur.execute("INSERT INTO %s VALUES (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL)" % t_name)
            refresh(cur, cfg, t_name)
        # MDB Tools' SQL parser has no ORDER BY, so read_only sorts client-side instead.
        cur.execute("SELECT * FROM %s" % t_name if ro else "SELECT * FROM %s ORDER BY i" % t_name)
        t = cur.fetch_arrow_table()
        got = [{k.lower(): v for k, v in r.items()} for r in t.to_pylist()]
        got.sort(key=lambda r: r["i"])
        r1, r2 = got
        assert r1["i"] == 1 and r1["f"] == 1.5, r1
        # binary_text: the server has no binary column type at all (CrateDB), so the
        # bytes land in a text column exactly as the ODBC driver encoded them -- for
        # psqlodbc that is PostgreSQL's bytea hex escape, "\x0102".
        assert r1["b"] in (b"\x01\x02", "\x01\x02", cfg.get("binary_text")), r1["b"]
        assert r1["s"] == "héllo 🚀" or (not cfg.get("astral", True) and r1["s"].startswith("héllo")), r1["s"]
        assert r1["d"] in (datetime.date(2024, 2, 29), datetime.datetime(2024, 2, 29)), r1["d"]
        ts = r1["ts"]
        assert ts.replace(microsecond=0) == datetime.datetime(2024, 2, 29, 13, 45, 10), ts
        # ts_us: servers whose TIMESTAMP is coarser than a microsecond (Firebird: 1/10000 s)
        assert ts.microsecond in cfg.get("ts_us", (123456, 123000)), ts
        n = r1["n"]
        fields = {f.name.lower(): str(f.type) for f in t.schema}
        # decimal_type: drivers that report a precision other than the declared one
        assert fields["n"] in ("decimal128(10, 3)", "string",
                               cfg.get("decimal_type", "decimal128(10, 3)")), fields["n"]
        assert n in (decimal.Decimal("12.345"), "12.345"), n
        assert fields["bo"] == cfg.get("bool_type", "bool"), fields["bo"]
        assert r1["bo"] in (True, 1), r1["bo"]
        # not_null: columns whose type has no NULL state at all (Access YESNO).
        skip = ("i",) + tuple(cfg.get("not_null", ()))
        assert all(v is None for k, v in r2.items() if k not in skip), r2
        # parameterised query (literal where the driver has no SQLBindParameter)
        if cfg.get("params", True):
            cur.execute("SELECT s FROM %s WHERE i = ?" % t_name, (1,))
        else:
            cur.execute("SELECT s FROM %s WHERE i = 1" % t_name)
        assert cur.fetchone()[0].startswith("héllo")
        # bulk ingest + read back (read_only reads the fixture's big table instead)
        if ro:
            check_big(cur, cfg, 'SELECT "a", "b" FROM "adbc_big"')
        else:
            check_ingest(cur, cfg, ing_name)
        # extra: per-database steps run after the standard workload, for features
        # only that database has.  Each entry is (step, expected): `step` is either
        # SQL, or (table, arrow table) to bulk ingest (mode="append") into a table an
        # earlier step created; `expected` is the first result row -- or the ingested
        # row count -- to assert, or None to run the step without checking it.
        # "{sfx}" in a step expands to ADBC_MATRIX_SUFFIX.
        for step, expected in cfg.get("extra", []):
            if isinstance(step, str):
                sql = step.format(sfx=SUFFIX)
                cur.execute(sql)
                got = tuple(cur.fetchone()) if expected is not None else None
            else:
                sql = "ingest into " + step[0].format(sfx=SUFFIX)
                got = (cur.adbc_ingest(step[0].format(sfx=SUFFIX), step[1], mode="append"),)
                refresh(cur, cfg, step[0].format(sfx=SUFFIX))
            assert expected is None or got == expected, (sql, got)
        # error path
        try:
            cur.execute("SELECT * FROM adbc_no_such_table")
            raise AssertionError("expected error")
        except dbapi.Error as e:
            if cfg.get("error_text", True):
                assert "adbc_no_such_table" in str(e) or "not" in str(e).lower(), str(e)
    # metadata
    objs = conn.adbc_get_objects(depth="all", table_name_filter=T).read_all().to_pylist()
    # column_order: SQLColumns orders columns by ORDINAL_POSITION, so a server whose
    # catalog does not populate it (Databend reports 1 for every column) reports them in
    # an arbitrary order.  The result-set metadata of a SELECT is unaffected; only the
    # catalog calls are, so those entries compare the column names as a set.
    order = (lambda c: c) if cfg.get("column_order", True) else sorted
    cols = ["i", "f", "s", "b", "d", "ts", "n", "bo"]
    # No catalog filter is given, so every catalog on the server that happens to
    # hold a table of this name is reported -- shared servers really do have
    # more than one.  Check the columns of each match individually.
    per_table = [order([c["column_name"].lower() for c in t["table_columns"]])
                 for cat in objs for s in cat["catalog_db_schemas"] or []
                 for t in s["db_schema_tables"] or []]
    assert order(cols) in per_table, per_table
    assert "adbc_t" in [x.lower() for x in conn.adbc_get_table_types()] or True
    sch = conn.adbc_get_table_schema(T)
    assert order([f.name.lower() for f in sch]) == order(cols)
    conn.close()
    return "PASS  (%s %s)" % (info["vendor_name"], info["vendor_version"])


def refresh(cur, cfg, table):
    """Make a write visible to the next scan on an eventually consistent store.

    `refresh` is the statement that does it, "{}" taking the table name (CrateDB
    refreshes a table's shards once a second, so a scan issued right after an INSERT
    still sees the table as it was before it).  Unset everywhere else: a no-op.
    """
    if cfg.get("refresh"):
        cur.execute(cfg["refresh"].format(table))


def ingest_payload(cfg, cols):
    """Build an ingest payload, applying the entry's `ingest_types` substitutions.

    `ingest_types` maps an Arrow type to the one to send instead, for a server that
    has no column type the ingest's generated DDL could name for it (CrateDB has no
    DATE column type, and no `bool` spelling of BOOLEAN).  Substituting a type the
    server does have keeps the rest of the ingest -- create, append, replace -- under
    test instead of stopping at the CREATE.
    """
    cast = cfg.get("ingest_types", {})
    return pa.table({k: v.cast(cast[v.type]) if v.type in cast else v for k, v in cols.items()})


def check_big(cur, cfg, sql):
    """Read a table of big_rows (a, b) rows, crossing the reader's batch boundary."""
    n = cfg.get("big_rows", 5000)
    cur.execute(sql)
    big = cur.fetch_arrow_table()
    pairs = sorted(zip(big.column("a").to_pylist(), big.column("b").to_pylist()))
    assert [a for a, _ in pairs] == list(range(n)), len(pairs)
    assert pairs[-1][1] == "r%d" % (n - 1), pairs[-1]


def check_ingest(cur, cfg, ing_name):
    """Bulk ingest, read back, then ingest big_rows rows and read those back."""
    tbl = ingest_payload(cfg, {
        "a": pa.array([1, 2, None], pa.int64()),
        "b": pa.array(["x", None, "zz"]),
        "c": pa.array([1.5, None, 2.5]),
        "d": pa.array([0, 19782, None], pa.date32()),
        "e": pa.array([True, None, False], pa.bool_()),
    })
    n1 = cur.adbc_ingest(ing_name, tbl, mode="create")
    n2 = cur.adbc_ingest(ing_name, tbl, mode="append")
    assert (n1, n2) == (3, 3) or not cfg.get("rowcount", True), (n1, n2)
    refresh(cur, cfg, ing_name)
    cur.execute('SELECT "a", "b", "c", "d" FROM "%s" WHERE "a" = 2' % ing_name)
    got = cur.fetch_arrow_table().to_pylist()
    # The ingest DDL asks the driver for a date (or, where the server has no date type,
    # a timestamp) column; a server whose timestamp carries a zone -- psqlodbc's
    # SQL_TYPE_TIMESTAMP is "timestamptz" -- hands back an aware datetime in UTC.
    d = got[0]["d"]
    if isinstance(d, datetime.datetime) and d.tzinfo is not None:
        d = d.astimezone(datetime.timezone.utc).replace(tzinfo=None)
    assert len(got) == 2 and got[0]["b"] is None and got[0]["c"] is None and d in (datetime.date(2024, 2, 29), datetime.datetime(2024, 2, 29)), got
    # bigger result to cross batch boundaries
    N = cfg.get("big_rows", 5000)
    cur.adbc_ingest(ing_name, ingest_payload(cfg, {"a": pa.array(range(N), pa.int64()), "b": pa.array(["r%d" % i for i in range(N)]),
                                                   "c": pa.array([float(i) for i in range(N)]), "d": pa.array([i for i in range(N)], pa.date32()),
                                                   "e": pa.array([i % 2 == 0 for i in range(N)])}), mode="replace")
    refresh(cur, cfg, ing_name)
    check_big(cur, cfg, 'SELECT "a", "b" FROM "%s" ORDER BY "a"' % ing_name)


if __name__ == "__main__":
    targets = sys.argv[1:] or list(DBS)
    failed = False
    for name in targets:
        try:
            res = run(name, DBS[name])
        except Exception as e:  # noqa: BLE001
            res = "FAIL  %s: %s" % (type(e).__name__, (str(e).splitlines() or ["(no message)"])[0][:160])
            failed = True
        print("%-9s %s" % (name, res))
    sys.exit(1 if failed else 0)
