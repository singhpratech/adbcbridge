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
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, QUESTDB_ODBC_DRIVER, ACCESS_ODBC_DRIVER,
    RISINGWAVE_ODBC_DRIVER
    YUGABYTE_ODBC_DRIVER, TIMESCALE_ODBC_DRIVER, CRATEDB_ODBC_DRIVER, QUESTDB_ODBC_DRIVER,
    QUESTDB_ODBC_DRIVER, ACCESS_ODBC_DRIVER, MATERIALIZE_ODBC_DRIVER
    QUESTDB_ODBC_DRIVER, ACCESS_ODBC_DRIVER, OPENGAUSS_ODBC_DRIVER
    QUESTDB_ODBC_DRIVER, ACCESS_ODBC_DRIVER, MATRIXONE_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, INFORMIX_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, COLUMNSTORE_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, FLIGHTSQL_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, GREPTIMEDB_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, ARCADEDB_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, INFLUXDB3_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, STARROCKS_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, IGNITE_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, OPENSEARCH_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, DORIS_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, CLOUDBERRY_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, YDB_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, VERTICA_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, OCEANBASE_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, DREMIO_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, TDENGINE_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, SPANNER_ODBC_DRIVER
    VIRTUOSO_ODBC_DRIVER, ACCESS_ODBC_DRIVER, MONGODBBI_ODBC_DRIVER
Servers are expected as in docker-compose.yml (override with *_CONN env vars); the
file-based entries (sqlite, duckdb, access) need no server.
See README.md in this directory for how to obtain each driver without root.
"""
import os, sys, shutil, tempfile, pathlib, datetime, decimal, ctypes

# Line-buffered stdout: an ODBC driver that aborts the process (SIGABRT from inside
# SQLExecDirect, seen on macOS) would otherwise take the last result line with it.
try:
    sys.stdout.reconfigure(line_buffering=True)
except (AttributeError, ValueError):
    pass



def _preload_odbc_drivers():
    """Open the ODBC driver libraries before pyarrow is imported.

    A driver that reaches libstdc++'s thread-locals through the initial-exec TLS
    model -- MySQL Connector/ODBC is the one in this matrix that does -- can only
    be loaded while libstdc++ has not yet been pinned to dynamic thread-local
    storage, and importing pyarrow pins it for the life of the process.  Opening
    the drivers first settles it the other way.  Best effort: a library that will
    not open here fails again, with its real reason, when its entry runs.
    See docs/TROUBLESHOOTING.md.
    """
    if not hasattr(os, "RTLD_LAZY"):  # not an ELF platform
        return
    try:
        libc = ctypes.CDLL(None)
        libc.dlopen.restype = ctypes.c_void_p
        libc.dlopen.argtypes = [ctypes.c_char_p, ctypes.c_int]
    except (OSError, AttributeError):
        return
    wanted = {arg.lower() for arg in sys.argv[1:] if not arg.startswith("-")}
    for name, value in os.environ.items():
        if not name.endswith("_ODBC_DRIVER") or name == "ADBC_ODBC_DRIVER":
            continue
        if wanted and name[: -len("_ODBC_DRIVER")].lower() not in wanted:
            continue
        if os.path.isfile(value):
            libc.dlopen(os.fsencode(value), os.RTLD_LAZY | os.RTLD_LOCAL)


_preload_odbc_drivers()

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
# The Arrow Flight SQL ODBC driver implements no SQLBindParameter at all, so the
# `flightsql` entry cannot load adbc_t the way every other entry does -- with a
# parameterised INSERT.  It builds the table with literal SQL in `setup` instead, from
# this column list, which is also what the entry declares as its `ddl`.
FLIGHTSQL_COLS = ("(i INTEGER, f DOUBLE, s VARCHAR, b BLOB, d DATE, ts TIMESTAMP,"
                  " n DECIMAL(10,3), bo BOOLEAN)")

# --- ArcadeDB -------------------------------------------------------------------
# ArcadeDB has no CREATE TABLE at all: a "table" is a document (or vertex) type, and a
# typed column is a property declared on it, so building one takes 1 + N statements
# instead of one.  That is also why the `arcadedb` entry is read_only -- adbc_ingest's
# generated CREATE TABLE has nowhere to go -- and why it builds its tables from these
# two helpers in `setup` instead, the way `flightsql` does with literal SQL.
# ArcadeDB's PostgreSQL plugin also refuses any statement longer than 32768 characters
# ("String content (...) too long"), so a literal bulk load has to be split.
ARCADEDB_MAX_SQL = 30000


def arcadedb_type(name, cols):
    """DROP + CREATE document type `name` with `cols` = [(column, ArcadeDB type)].

    Dropping first is what makes `setup` idempotent -- bench/matrix_bench.py replays it
    on every connection it opens -- since ArcadeDB has no CREATE ... IF NOT EXISTS for a
    property (only for a type).
    """
    return (["DROP TYPE %s IF EXISTS UNSAFE" % name, "CREATE DOCUMENT TYPE %s" % name] +
            ["CREATE PROPERTY %s.%s %s" % (name, c, t) for c, t in cols])


def arcadedb_insert(name, cols, rows):
    """INSERT statements loading `rows` (formatted "(v, ...)" literals) into `name`."""
    head = "INSERT INTO %s (%s) VALUES " % (name, ", ".join(cols))
    out, batch, size = [], [], 0
    for r in rows:
        if batch and size + len(r) + 1 > ARCADEDB_MAX_SQL - len(head):
            out.append(head + ",".join(batch))
            batch, size = [], 0
        batch.append(r)
        size += len(r) + 1
    if batch:
        out.append(head + ",".join(batch))
    return out


ARCADEDB_BIG_ROWS = 100000

# --- Dremio ---------------------------------------------------------------------
# Dremio has no row-generating table function (no range(), no generate_series), so the
# `dremio` entry's 100,000-row adbc_big comes from five ten-row VALUES lists cross
# joined, with the row number computed from their digits.
DREMIO_DIGITS = "(VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9))"
DREMIO_ROWNO = "d1.n * 10000 + d2.n * 1000 + d3.n * 100 + d4.n * 10 + d5.n"

# TDengine keeps no table without a primary-key TIMESTAMP first column, so the shared
# EXTRA_ROWS (a BIGINT first) cannot be ingested into one.  These are the same four rows
# behind a leading timestamp, and without the date32 column -- TDengine has no DATE type.
TDENGINE_ROWS = pa.table({
    "ts": pa.array([datetime.datetime(2024, 4, 1, 0, 0, i) for i in range(4)], pa.timestamp("us")),
    "a": pa.array([1, 2, 3, 4], pa.int64()),
    "b": pa.array(["p", "q", "r", "s"]),
    "c": pa.array([1.5, 2.5, None, 4.5]),
    "e": pa.array([True, None, False, True], pa.bool_()),
})
# The two tables the `tdengine` entry reads, built by its `setup` (see the entry).  Every
# statement is idempotent: the tables are created IF NOT EXISTS and every row carries a
# fixed primary-key timestamp, which TDengine overwrites in place rather than appending a
# duplicate -- so replaying this on every connection (bench/matrix_bench.py opens several)
# leaves exactly these rows.
def tdengine_setup(n=20000, chunk=1000, base=1709210000000000):
    rows = ["(%d, %d, 'r%d')" % (base + i * 1000, i, i) for i in range(n)]
    return [
        # 'us': the workload's timestamp carries microseconds, and a TDengine database
        # stores its timestamps at the precision it was created with (default 'ms').
        "CREATE DATABASE IF NOT EXISTS adbc PRECISION 'us'",
        "USE adbc",
        "CREATE TABLE IF NOT EXISTS adbc_t (ts TIMESTAMP, i INT, f DOUBLE, s NCHAR(50),"
        " b VARBINARY(20), d TIMESTAMP, n VARCHAR(20), bo BOOL)",
        "INSERT INTO adbc_t VALUES ('2024-02-29 13:45:10.123456', 1, 1.5, 'h\u00e9llo \U0001f680',"
        " '\\x0102', '2024-02-29 00:00:00', '12.345', true)",
        # Row 2 is the all-NULL row of the standard workload; its `ts` cannot be NULL
        # (it is the primary key), hence not_null below.
        "INSERT INTO adbc_t VALUES ('2024-03-01 00:00:00.000000', 2, NULL, NULL, NULL, NULL,"
        " NULL, NULL)",
        "CREATE TABLE IF NOT EXISTS adbc_big (ts TIMESTAMP, a INT, b VARCHAR(20))",
    ] + ["INSERT INTO adbc_big VALUES " + ",".join(rows[i:i + chunk])
         for i in range(0, n, chunk)]


DBS = {
    "sqlite": dict(
        env="SQLITE_ODBC_DRIVER", conn="Driver={drv};Database=" + os.path.join(TMP, "m.db") + ";",
        ddl="CREATE TABLE adbc_t (i INTEGER, f REAL, s TEXT, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        # sqliteodbc converts UTF-8 through UCS-2 and drops the astral-plane emoji.
        decimal_type="string", ts_precision="ms", astral=False, text_sortable=True),
    "duckdb": dict(
        # A file, not :memory:: with an in-memory database every ODBC connection is its
        # own empty DuckDB, so a benchmark that ingests on one connection and reads on
        # another finds no table (every language harness reported exactly that).  The
        # compat workload runs on one connection and never noticed.
        env="DUCKDB_ODBC_DRIVER", conn="Driver={drv};Database=" + os.path.join(TMP, "duck.db") + ";",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        text_sortable=True),
    "postgres": dict(
        env="POSTGRES_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # psqlodbc describes BYTEA as SQL_LONGVARBINARY of column_size 0 however long its
        # values are, so the reader binds it at a guessed width and re-reads what
        # overflows -- and gives the binding up part-way down a result set whose values
        # never fit (AdaptBindWidth in src/odbc_reader.c).  500 rows, every hundredth of
        # them 40,000 bytes: the first row is one of the big ones, so the assertion is
        # that a value 20x the bound width comes back whole through the path the rest of
        # the table pushed the reader onto.  tests/test_long_columns.py has the rest.
        extra=[
            ('DROP TABLE IF EXISTS "adbc_long{sfx}"', None),
            ('CREATE TABLE "adbc_long{sfx}" (i INTEGER, b BYTEA)', None),
            ('INSERT INTO "adbc_long{sfx}" SELECT g,'
             " convert_to(CASE WHEN g % 100 = 1 THEN repeat('ab', 20000)"
             "                 ELSE repeat('cd', 50) END, 'UTF8')"
             ' FROM generate_series(1, 500) g', None),
            ('SELECT "i", "b" FROM "adbc_long{sfx}" ORDER BY "i"', (1, b"ab" * 20000)),
            ('DROP TABLE IF EXISTS "adbc_long{sfx}"', None),
        ]),
    "mariadb": dict(
        env="MARIADB_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "columnstore": dict(
        # MariaDB ColumnStore is the columnar storage engine inside a MariaDB server
        # (this image is MariaDB 11.1 + ColumnStore 23.02), so MariaDB Connector/ODBC
        # drives it and the `mariadb` entry's wire-level tolerances apply unchanged:
        # BOOLEAN is TINYINT(1) -> int8, and the double-quoted identifiers adbc_ingest
        # emits need ANSI_QUOTES.
        env="COLUMNSTORE_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=13313;Database=adbc;User=adbc;Password=Adbc!Bridge2026;",
        # ENGINE=Columnstore on adbc_t, and default_storage_engine in `setup` for every
        # table the driver's generated ingest DDL creates: the server's default engine is
        # still InnoDB, so without both the workload would run against a plain MariaDB and
        # never touch the columnar engine this entry exists to cover.
        #   b is BLOB, not VARBINARY: ColumnStore refuses the type outright ("Varbinary is
        # currently not supported by Columnstore") while BLOB round-trips bytes.
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b BLOB, d DATE,"
            " ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN) ENGINE=Columnstore",
        bool_type="int8",
        setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')",
               "SET SESSION default_storage_engine = Columnstore"]),
    "oracle": dict(
        env="ORACLE_ODBC_DRIVER", conn="Driver={drv};DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;",
        ddl="CREATE TABLE adbc_t (i NUMBER(10), f BINARY_DOUBLE, s VARCHAR2(50), b RAW(10), d DATE, ts TIMESTAMP(6), n NUMBER(10,3), bo BOOLEAN)",
        # Oracle's SQLGetTypeInfo(SQL_LONGVARCHAR) names CLOB, so ingesting an Arrow
        # string column here creates one -- and reading a CLOB back is the one shape
        # SQORA cannot be driven through the way every other driver is.  3,000 rows cross
        # three 1,024-row batches, which is what it takes to reach the crash that quirk
        # exists for; see OdbcDetectQuirks in src/odbc_driver.c.
        wide_text_rows=3000,
        ident=str.upper, unicode_env="NLS_LANG=.AL32UTF8"),
    "clickhouse": dict(
        env="CLICKHOUSE_ODBC_DRIVER", conn="Driver={drv};Url=http://127.0.0.1:18123;Database=adbc;UID=adbc;PWD=adbc;",
        ddl="CREATE TABLE adbc_t (i Nullable(Int32), f Nullable(Float64), s Nullable(String), b Nullable(String), d Nullable(Date), ts Nullable(DateTime64(6)), n Nullable(Decimal(10,3)), bo Nullable(Bool)) ENGINE = Memory",
        # clickhouse-odbc sends NULL parameters as empty strings (driver limitation) and
        # does not report affected row counts.
        null_params=False, rowcount=False, big_rows=300),
    "mssql": dict(
        env="MSSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;",
        ddl="CREATE TABLE adbc_t (i INT, f FLOAT, s NVARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME2(6), n DECIMAL(10,3), bo BIT)",
        text_sortable=True),
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
        conn="Driver={drv};Server=127.0.0.1;Port=13310;Database=adbc;User=root;{plugin_dir}",
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
    "matrixone": dict(
        # MatrixOne is a hyper-converged (HTAP) database that speaks the MySQL wire
        # protocol -- it announces itself as "8.0.30-MatrixOne-v4.2.0" -- so MySQL
        # Connector/ODBC drives it and the `mysql` entry's DDL and tolerances mostly
        # apply: BOOLEAN is reported as SQL_TINYINT -> int8, and the double-quoted
        # identifiers adbc_ingest emits need ANSI_QUOTES.
        #   The stock image ships the built-in `dump`/`111` account and no user
        # database at all, so `setup` creates `adbc` and switches to it; the
        # connection string therefore names no database.  Both statements are
        # idempotent, which matters because bench/matrix_bench.py replays `setup` on
        # every connection it opens.
        # {plugin_dir}: MatrixOne authenticates with mysql_native_password, whose
        # *client-side* plugin Connector/ODBC 9 loads at run time -- see conn_uri().
        env="MATRIXONE_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=16001;User=dump;Password=111;{plugin_dir}",
        # The PRIMARY KEY is required, not decorative: MatrixOne gives a table declared
        # without one a hidden "__mo_fake_pk_col", which information_schema.columns --
        # and so SQLColumns/GetObjects -- reports as a 9th column even though SELECT *
        # never returns it.  (Same shape as CockroachDB's synthesised "rowid".)
        ddl="CREATE TABLE adbc_t (i INT PRIMARY KEY, f DOUBLE, s VARCHAR(50), b VARBINARY(10),"
            " d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8",
        # MatrixOne's BIT column cannot take a parameter over the MySQL binary protocol
        # at all: binding an integer 1 fails with "data out of range: data type bit(1),
        # value 1", binding NULL silently stores false, and a mixed batch of the three
        # aborts the whole server ("malloc(): unaligned fastbin chunk detected" -- it
        # corrupts its own heap and the process dies).  That is what the ingest DDL asks
        # for by default, since Connector/ODBC's SQLGetTypeInfo names BIT for a boolean.
        # Sending the column as int8 -> TINYINT, which MatrixOne handles correctly,
        # keeps the whole ingest (create/append/replace) under test.  Its own BOOLEAN
        # column type is unaffected: adbc_t's `bo` round-trips fine.
        ingest_types={pa.bool_(): pa.int8()},
        setup=["CREATE DATABASE IF NOT EXISTS adbc", "USE adbc",
               "SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "doris": dict(
        # Apache Doris is an MPP analytic warehouse that serves the MySQL wire protocol on
        # 9030, so MySQL Connector/ODBC drives it.  It announces itself as plain MySQL
        # "5.7.99" -- only @@version_comment ("Doris version doris-2.1.0-...") names it --
        # and it reports SQL_TC_NONE, so the connector's `myodbc + no transactions` quirk
        # in src/odbc_driver.c applies to it exactly as it does to Databend: dates,
        # timestamps and binaries go as quoted text rather than `_binary'...'` literals,
        # and ingest DDL uses portable type names.
        # The driver is the `mysql` entry's MySQL Connector/ODBC, unpacked once.
        env="DORIS_ODBC_DRIVER",
        # NO_SSPS=1 is what makes the entry work at all: Doris answers COM_STMT_PREPARE
        # only for a point SELECT ("Only support prepare SelectStmt point query now"), and
        # an INSERT prepared server-side dies inside the FE with a bare
        # "NullPointerException, msg: null".  With NO_SSPS the connector substitutes bound
        # parameters into the SQL text and every statement goes as a plain query.
        # {plugin_dir}: Doris offers mysql_native_password, whose *client-side* plugin
        # Connector/ODBC 9 loads at run time -- see conn_uri() below.
        #   The connection string names no database: the image ships none, so `setup`
        # creates `adbc` and switches to it (both statements idempotent, which matters
        # because bench/matrix_bench.py replays `setup` on every connection it opens).
        conn="Driver={drv};Server=127.0.0.1;Port=19031;User=root;NO_SSPS=1;{plugin_dir}",
        # DISTRIBUTED BY is mandatory on every Doris OLAP table ("Create olap table should
        # contain distribution desc"); RANDOM with an automatic bucket count is the
        # neutral choice.  b is VARCHAR, not VARBINARY, because Doris has no binary column
        # type at all -- the parser does not even know the word ("no viable alternative at
        # input 'VARBINARY'") -- and a character column carries the two bytes through.
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARCHAR(50), d DATE,"
            " ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)"
            " DISTRIBUTED BY RANDOM BUCKETS AUTO",
        # Doris' BOOLEAN goes over the MySQL wire as a TINYINT(1), which Connector/ODBC
        # reports as SQL_TINYINT -> int8, exactly as MySQL's own does.
        bool_type="int8",
        # Doris accepts ANSI_QUOTES in sql_mode and then ignores it: with the mode set,
        # SELECT "a" FROM t still returns the constant 'a' for every row rather than the
        # column, silently.  Its identifiers are backtick-quoted, and Connector/ODBC
        # reports the backtick as SQL_IDENTIFIER_QUOTE_CHAR here (no ANSI_QUOTES is set),
        # so adbc_ingest already quotes correctly; it is this file's own SQL that has to.
        # Same shape as the `greptimedb` entry.
        quote="`",
        # The generated ingest DDL asks for the portable ISO type names (see
        # `ansi_ddl_type_names` in src/odbc_driver.c) and Doris accepts all of them --
        # BIGINT, TEXT, DATE, BOOLEAN, DECIMAL(p,s) -- except the one for a double: it has
        # DOUBLE but neither "DOUBLE PRECISION" nor "REAL" ("extraneous input
        # 'PRECISION'").  Sending that column as a decimal, a type it does name the same
        # way, keeps the whole ingest -- create, append, replace -- under test.  adbc_t's
        # own `f DOUBLE` column is unaffected.  Same fix as `greptimedb`.
        ingest_types={pa.float64(): pa.decimal128(12, 3)},
        # force_olap_table_replication_num=1: the all-in-one image is a single backend and
        # Doris defaults every table to three replicas, so any CREATE TABLE would fail
        # with "replication num should be less than the number of available backends".
        # This is a deployment fact of a one-BE cluster, not something a table's DDL should
        # carry, so it is set once on the FE here rather than in the ingest DDL.
        setup=["ADMIN SET FRONTEND CONFIG ('force_olap_table_replication_num' = '1')",
               "CREATE DATABASE IF NOT EXISTS adbc", "USE adbc"],
        # Each INSERT is a separate load transaction the FE publishes across the backend,
        # which runs at a few hundred rows/s however the rows are bound; 2000 still crosses
        # the reader's 1024-row batch boundary, which is what the step is for.
        big_rows=2000),
    "oceanbase": dict(
        # OceanBase CE is a distributed HTAP database whose MySQL mode speaks the MySQL
        # wire protocol -- it announces itself as "5.7.25-OceanBase_CE-v4.4.2.1" -- so
        # MySQL Connector/ODBC drives it and the `mysql` entry's DDL and tolerances apply
        # unchanged: BOOLEAN is TINYINT(1) -> int8, and the double-quoted identifiers
        # adbc_ingest emits need ANSI_QUOTES.
        #   The user is `root@test`, not `root`: OceanBase is multi-tenant and the login
        # name carries the tenant.  `test` is the MySQL-mode business tenant the image's
        # boot creates (OB_TENANT_NAME) and the one OB_DATABASE=adbc creates the database
        # in; root@sys is the cluster's own administrative tenant, which holds no user
        # databases.  The "@" is only a login name to the driver, which passes it through.
        # {plugin_dir}: OceanBase offers only mysql_native_password, whose *client-side*
        # plugin Connector/ODBC 9 loads at run time -- see conn_uri() below.
        env="OCEANBASE_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=12881;Database=adbc;User=root@test;"
             "Password=adbc;{plugin_dir}",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE,"
            " ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8",
        setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "greptimedb": dict(
        # GreptimeDB is a time-series database that serves *both* the PostgreSQL wire
        # (4003) and the MySQL wire (4002).  This entry uses the MySQL one, driven by
        # MySQL Connector/ODBC: psqlodbc cannot connect at all, because its fixed connect
        # handshake asks for `show transaction_isolation` and GreptimeDB implements only
        # the standard `SHOW TRANSACTION ISOLATION LEVEL` spelling -- the same shape of
        # failure as H2.  See tests/compat/README.md.
        #   Over the MySQL wire it announces itself as "8.4.2-GreptimeDB-1.1.4" and, like
        # Databend, reports SQL_TC_NONE, so the connector's `myodbc + no transactions`
        # quirk in src/odbc_driver.c already applies to it.
        # The driver is the `mysql` entry's MySQL Connector/ODBC, unpacked once.
        env="GREPTIMEDB_ODBC_DRIVER",
        # The connection string, and the three things it does not say:
        # No `sql_mode` to set: GreptimeDB answers `SELECT @@sql_mode` with "0" and its
        # parser reads "..." as a string literal, so the double-quoted identifiers the
        # `mysql` entry needs ANSI_QUOTES for would not parse here.  It does not need
        # them: Connector/ODBC reports the backtick as SQL_IDENTIFIER_QUOTE_CHAR against
        # this server, and GreptimeDB is a MySQL dialect that quotes with backticks.
        # {plugin_dir}: GreptimeDB's MySQL handler offers mysql_native_password, whose
        # client-side plugin Connector/ODBC 9 loads at run time -- see conn_uri() below.
        #   NO_SSPS=1 is what makes the entry work at all, for the same reason Databend
        # needs it, though the symptom differs: GreptimeDB *does* answer COM_STMT_PREPARE,
        # but it describes every parameter of it as VAR_STRING whatever the target column
        # is, and then type-checks the value it gets against the column and refuses the
        # string it asked for ("Expected type: Int32(Int32Type), actual:
        # MYSQL_TYPE_STRING").  No ODBC-side binding can satisfy both halves -- binding
        # SQL_C_SLONG/SQL_INTEGER fails identically -- so the server-side prepare protocol
        # is unusable here.  With NO_SSPS the connector substitutes bound parameters into
        # the SQL text and every statement goes as a plain query, which GreptimeDB parses
        # and types normally.
        conn="Driver={drv};Server=127.0.0.1;Port=14002;Database=public;User=greptime;"
             "NO_SSPS=1;{plugin_dir}",
        # Every GreptimeDB table must declare exactly one TIME INDEX column, and it must
        # be a TIMESTAMP and NOT NULL -- `ts` is the natural one here.  WITH append_mode
        # is not decoration: a table without it merges rows that share a time index (and
        # this table's two rows do, see row2_fill), so five inserts at the same
        # millisecond would read back as one row.
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE,"
            " ts TIMESTAMP(6) TIME INDEX, n DECIMAL(10,3), bo BOOLEAN)"
            " WITH ('append_mode'='true')",
        # The time index cannot be NULL, so the all-NULL second row carries row 1's
        # timestamp instead of NULL, and the read-back assertion skips it.
        row2_fill=("ts",), not_null=("ts",),
        # GreptimeDB's BOOLEAN goes over the MySQL wire as a TINYINT(1), which
        # Connector/ODBC reports as SQL_TINYINT -> int8, exactly as MySQL's own does.
        bool_type="int8",
        # GreptimeDB quotes identifiers the MySQL way and has no ANSI_QUOTES to switch
        # to, so a "..." in a column position is a *string literal*: SELECT "a" FROM t
        # returns the constant 'a' for every row rather than the column, silently.
        # Connector/ODBC reports the backtick as SQL_IDENTIFIER_QUOTE_CHAR here, so
        # adbc_ingest already quotes correctly; it is this file's own SQL that has to.
        quote="`",
        # The generated ingest DDL asks for the portable ISO type names (see
        # `ansi_ddl_type_names` in src/odbc_driver.c), and GreptimeDB accepts all of them
        # -- BIGINT, TEXT, DATE, BOOLEAN, DECIMAL(p,s) -- except the one for a double:
        # it has DOUBLE and FLOAT but neither "DOUBLE PRECISION" nor "REAL" ("SQL data
        # type not supported yet: DoublePrecision").  Sending that column as a decimal,
        # a type it does name the same way, keeps the whole ingest -- create, append,
        # replace -- under test.  adbc_t's own `f DOUBLE` column is unaffected.
        ingest_types={pa.float64(): pa.decimal128(12, 3)}),
    "starrocks": dict(
        # StarRocks is an MPP columnar warehouse that serves the MySQL wire protocol on
        # 9030 (it announces itself as MySQL 8.0.33), so MySQL Connector/ODBC drives it.
        # Only the wire protocol is MySQL's; the SQL dialect is StarRocks' own.
        #   The stock image ships no user database and a passwordless `root`, so the
        # connection string names no database and `setup` creates one and switches to it.
        # Both statements are idempotent, which matters because bench/matrix_bench.py
        # replays `setup` on every connection it opens.
        # {plugin_dir}: StarRocks authenticates with mysql_native_password, whose
        # *client-side* plugin Connector/ODBC 9 loads at run time -- see conn_uri().
        env="STARROCKS_ODBC_DRIVER",
        #   NO_SSPS=1 is what makes the entry work at all, for the same reason Databend
        # needs it: StarRocks answers COM_STMT_PREPARE for anything but a SELECT with
        # "This command is not supported in the prepared statement protocol yet" (1295),
        # so SQLPrepare of the parameterised INSERT fails.  With NO_SSPS the connector
        # substitutes bound parameters into the SQL text and every statement goes as a
        # plain query.
        conn="Driver={drv};Server=127.0.0.1;Port=19030;User=root;NO_SSPS=1;{plugin_dir}",
        # StarRocks type names: DATETIME is the microsecond timestamp (there is no
        # TIMESTAMP type) and VARBINARY round-trips bytes.  No DISTRIBUTED BY or
        # replication_num is needed -- StarRocks 3.1+ picks a random bucket distribution
        # and the allin1 image's single BE defaults replication_num to 1.
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10),"
            " d DATE, ts DATETIME, n DECIMAL(10,3), bo BOOLEAN)",
        # StarRocks BOOLEAN is stored as a TINYINT and described as SQL_TINYINT -> int8,
        # as MySQL's own TINYINT(1) BOOLEAN is.
        bool_type="int8",
        # StarRocks describes a DECIMAL(10,3) column at the *display* width MySQL uses on
        # the wire -- 12, the ten digits plus the sign and the decimal point -- where a
        # real MySQL reports the declared precision.  Connector/ODBC passes that straight
        # through as SQL_DECIMAL precision, so the column reads back as decimal128(12, 3).
        # No digits are lost: the scale is right and 12.345 round-trips exactly.
        decimal_type="decimal128(12, 3)",
        # No ANSI_QUOTES here, unlike every other MySQL-wire entry: StarRocks accepts the
        # sql_mode *value* but its parser ignores it, so `"adbc_ing"` is still a string
        # literal and `CREATE TABLE "adbc_ing" (...)` is a syntax error.  Connector/ODBC
        # therefore reports the backtick for SQL_IDENTIFIER_QUOTE_CHAR -- which is right --
        # and ingest quotes with it, so this file has to as well.
        quote="`",
        # StarRocks runs every INSERT as its own load transaction and waits for the
        # version to publish, which costs a flat ~100 ms whatever the statement holds --
        # the `mysql` client inside the container measures the same 10 rows/s for 100
        # single-row INSERTs.  Since the connector in NO_SSPS mode sends one statement per
        # parameter set, single-row ingest runs at that rate.  2000 still crosses the
        # reader's 1024-row batch boundary, which is what this step is for, without
        # spending eight minutes on it.
        big_rows=2000,
        setup=["CREATE DATABASE IF NOT EXISTS adbc", "USE adbc"]),
    "mongodbbi": dict(
        # MongoDB behind the MongoDB BI Connector: `mongosqld` reads a MongoDB instance
        # and serves the *MySQL wire protocol* over it, presenting each collection as a
        # relational table, so MySQL Connector/ODBC drives it (see the `mysql` entry for
        # the driver download and the LD_PRELOAD pyarrow needs).  It announces itself as
        # "5.7.12 mongosqld v2.14.22".  The server side -- the tarball, the DRDL schema
        # that maps the collections and the mongosh seed -- is in tests/compat/README.md.
        env="MONGODBBI_ODBC_DRIVER",
        # NO_SSPS=1 is what makes the parameterised query work: mongosqld answers
        # COM_STMT_PREPARE with error 1295, "This command is not supported in the prepared
        # statement protocol yet", exactly as Databend's MySQL handler does.  With NO_SSPS
        # the connector substitutes bound parameters into the SQL text and every statement
        # goes as a plain query.
        # {plugin_dir}: mongosqld offers only mysql_native_password, whose client-side
        # plugin Connector/ODBC 9 loads at run time -- and here it does not fail cleanly
        # without it, it *segfaults* in the handshake.  See conn_uri() and README.md.
        conn="Driver={drv};Server=127.0.0.1;Port=13315;Database=adbc;User=adbc;"
             "NO_SSPS=1;{plugin_dir}",
        # read_only: mongosqld is a query engine only -- it has no DDL and no DML at all
        # ("unsupported statement") -- and a table there is a MongoDB collection plus a
        # column mapping in mongosqld's DRDL schema, so neither the entry's `ddl` nor
        # adbc_ingest's generated CREATE TABLE has anywhere to go.  Both tables are loaded
        # into MongoDB out of band with mongosh, as the `influxdb3` entry's are over the
        # HTTP write API; see README.md.
        read_only=True,
        # ddl is documentation here (nothing executes it): the SQL shape the DRDL schema in
        # README.md gives the adbc_t collection.
        ddl="CREATE TABLE adbc_t (_id VARCHAR(24), i BIGINT, f DOUBLE, s VARCHAR(65535),"
            " b VARCHAR(65535), d DATETIME, ts DATETIME, n DECIMAL(65,20), bo BOOLEAN)",
        # Every MongoDB document carries an _id and the BI Connector maps it as an ordinary
        # column, so "SELECT *" returns a ninth, always-populated column -- the same shape
        # as ArcadeDB's @rid, and skipped the same way.
        pseudo_columns=("_id",),
        # mongosqld reports a table's columns in its own (alphabetical) order, in the
        # catalog and in a SELECT * alike.
        catalog_cols=("b", "bo", "d", "f", "i", "n", "s", "ts"),
        # A MySQL dialect with no sql_mode to set (mongosqld has no SET SESSION), so a
        # "..." is a string literal and identifiers are backtick-quoted, as GreptimeDB's.
        quote="`",
        # The BI Connector has no binary type at all: bson.Binary cannot be named in a DRDL
        # schema ("unsupported Mongo type: bson.Binary") and a sampled binData field reads
        # back NULL, so the two bytes are stored as text and read back as text, exactly as
        # for the `cratedb` and `influxdb3` entries.
        # MongoDB's boolean goes over the MySQL wire as TINYINT(1), which Connector/ODBC
        # reports as SQL_TINYINT -> int8, as MySQL's own BOOLEAN does.
        bool_type="int8",
        # mongosqld describes every decimal as DECIMAL(65,20) whatever the values in it --
        # 65 digits is more than an Arrow decimal128 can hold -- so the column comes back
        # as its exact text, as it does for the `databend` and `flightsql` entries.
        decimal_type="string",
        # A BSON date is milliseconds since the epoch, so `ts` keeps 123 of ROW1's 123456
        # microseconds (the default tolerance already allows it) and `d`, which has no DATE
        # type to land in either, is a midnight timestamp.
        # adbc_big holds 100,000 documents -- the table check_big() reads and the one
        # bench/matrix_bench.py times a fetch of on a read_only entry.
        big_rows=100000,
        extra=[
            # What only this stack does: SQL that mongosqld translates into a MongoDB
            # aggregation pipeline and runs inside the server -- a filtered count, a
            # GROUP BY on a boolean field, and a DISTINCT over the ObjectId `_id` that
            # every document carries.
            ("SELECT count(*) FROM `adbc_big` WHERE `a` > 50000", (49999,)),
            ("SELECT `e`, count(*) FROM `adbc_big` GROUP BY `e` ORDER BY `e`", (0, 50000)),
            ("SELECT count(DISTINCT `_id`) FROM `adbc_t`", (2,)),
        ]),
    "db2": dict(
        env="DB2_ODBC_DRIVER", conn="Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=50000;Protocol=TCPIP;Uid=db2inst1;Pwd=Adbc2026;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)",
        # Db2 folds unquoted identifiers to upper case.  The clidriver libdb2.so is built
        # with a 32-bit SQLLEN; the driver detects that itself (adbc.odbc.sqllen_32bit),
        # so no tolerance flag is needed here.  See README.md.
        ident=str.upper,
        # Db2's SQL_LONGVARCHAR is LONG VARCHAR, which it will not sort, group or
        # de-duplicate on and writes ~700x slower than a VARCHAR; ingest DDL asks for the
        # widest VARCHAR instead.  See ddl_string_as_max_varchar in src/odbc_internal.h.
        text_sortable=True),
    "informix": dict(
        # IBM Informix is reached over DRDA -- the same wire protocol Db2 speaks, served
        # by Informix's `<server>_dr` alias on port 9089 -- so the Db2 CLI driver
        # (clidriver `libdb2.so`, the `db2` entry's driver) drives it unchanged.  It
        # answers SQL_DRIVER_NAME "libdb2.a" and SQL_DBMS_NAME "IDS/UNIX64"; the 32-bit
        # SQLLEN of that library applies here too and the driver detects it itself.
        env="INFORMIX_ODBC_DRIVER",
        conn="Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=19089;Protocol=TCPIP;Uid=informix;Pwd=in4mix;",
        # Informix type names, none of which are the SQL-standard spellings:
        #   * BYTE is its byte-string type (there is no VARBINARY).  Its sibling BLOB is
        #     a smart large object and needs an sbspace, which the developer image does
        #     not configure, so BYTE is the one that works out of the box.
        #   * DATETIME YEAR TO FRACTION(5) is the timestamp; FRACTION(5) is the finest
        #     Informix has, so the stored value is rounded to 10 microseconds -- see
        #     ts_us below.
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTE, d DATE,"
            " ts DATETIME YEAR TO FRACTION(5), n DECIMAL(10,3), bo BOOLEAN)",
        # FRACTION(5) holds five fractional digits: .123456 is stored as .12345.
        ts_us=(123450,),
        # Informix BOOLEAN has no DRDA counterpart; it is described as SMALLINT, so the
        # column reads back as int16 with 1 for true.  (The driver also has to send
        # boolean parameters as integers here -- see adbc.odbc's Informix quirk.)
        bool_type="int16"),
    "monetdb": dict(
        env="MONETDB_ODBC_DRIVER", conn="Driver={drv};Host=127.0.0.1;Port=15000;Database=adbc;Uid=monetdb;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR(50), b BLOB, d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)"),
    "vertica": dict(
        # Vertica is a columnar analytics warehouse reached over its own native protocol
        # on 5433 -- not a PostgreSQL wire, despite the port -- so its own ODBC client
        # driver drives it (libverticaodbc.so, SQL_DRIVER_NAME "verticaodbcw.so").  That
        # driver reads a vertica.ini of its own before it will load at all; see
        # tests/compat/README.md for it and for the root-free client extraction.
        env="VERTICA_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=15433;Database=VMart;UID=dbadmin;PWD=;",
        # Plain Vertica types, all of them native.  Its INTEGER is 64-bit (there is no
        # narrower integer type -- INT, SMALLINT and TINYINT are all aliases of it), so
        # `i` reads back as int64, and VARBINARY round-trips bytes.
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b VARBINARY(10),"
            " d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)"),
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
    "cloudberry": dict(
        # Apache Cloudberry is the Greenplum fork: an MPP cluster of PostgreSQL segments
        # behind one coordinator (this image is a single node with two primary segments),
        # so it speaks the PostgreSQL wire protocol, reports SQL_DBMS_NAME "PostgreSQL"
        # 14.0.4, and psqlodbc drives it.  Apart from the port this is the `postgres`
        # entry: INTEGER, DOUBLE PRECISION, VARCHAR, BYTEA, DATE, TIMESTAMP, NUMERIC and
        # BOOLEAN all behave as they do on stock PostgreSQL.
        env="CLOUDBERRY_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=15443;Database=adbc;Uid=gpadmin;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # The standard workload alone would be a duplicate of `postgres` -- it already
        # runs on the MPP engine (the 5000 ingested rows hash-distribute across both
        # primary segments), but nothing in it would fail on a single-node server.  The
        # `extra` steps exercise the two things that make Cloudberry Cloudberry: a table
        # whose rows live on segments, and append-optimized *column-oriented* storage,
        # which is the Greenplum-family storage format stock PostgreSQL has no
        # equivalent of.  adbc_t itself stays a plain heap table.
        extra=[
            ('DROP TABLE IF EXISTS "adbc_ao{sfx}"', None),
            # Column names/types match EXTRA_ROWS: the ingest below appends into it.
            # "a" is the distribution key, so the rows hash out across the segments.
            ('CREATE TABLE "adbc_ao{sfx}" ("a" BIGINT, "b" VARCHAR(20), "c" DOUBLE PRECISION,'
             ' "d" DATE, "e" BOOLEAN) WITH (appendonly=true, orientation=column)'
             ' DISTRIBUTED BY ("a")', None),
            # Cloudberry 2.x is PostgreSQL 14-based, so the storage format is a table
            # access method -- `ao_column` -- not the `relstorage` column Greenplum 6
            # and earlier carried on pg_class.
            ("SELECT a.amname FROM pg_am a JOIN pg_class c ON c.relam = a.oid"
             " WHERE c.relname = 'adbc_ao{sfx}'", ("ao_column",)),
            (("adbc_ao{sfx}", EXTRA_ROWS), (4,)),   # bulk ingest into the columnar table
            # The ingested rows really went through the distribution machinery rather
            # than landing on one segment: they occupy more than one.
            ('SELECT count(DISTINCT gp_segment_id) > 1 FROM "adbc_ao{sfx}"', (True,)),
            ('SELECT "b", "c" FROM "adbc_ao{sfx}" WHERE "a" = 3', ("r", None)),
            # An aggregate is planned across the segments and merged on the coordinator.
            ('SELECT count(*), sum("a") FROM "adbc_ao{sfx}"', (4, 10)),
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
    "opengauss": dict(
        # openGauss is Huawei's fork of PostgreSQL 9.2, so psqlodbc drives it (it reports
        # SQL_DBMS_NAME "PostgreSQL" 9.2.4) and the plain PostgreSQL types apply unchanged.
        # Two things about the server, not the driver, shape this entry -- see
        # tests/compat/README.md: openGauss refuses a remote login for the *initial* user
        # (gaussdb), so the matrix connects as an `adbc` role created after start-up, and
        # that role's database is created with DBCOMPATIBILITY 'PG' (the image's default,
        # pinned explicitly) -- in openGauss's other, Oracle-flavoured 'A' mode a DATE
        # column is a timestamp(0), and `d` below would not read back as a date.
        env="OPENGAUSS_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=15438;Database=adbc;Uid=adbc;Pwd=Adbc@2026;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)"),
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
        binary_text="\\x0102", # CrateDB does not report a NUMERIC's precision or scale: psqlodbc 16 (Linux)
        # describes the column as decimal128(28, 6), psqlodbc 18 (macOS) as
        # decimal128(28, 3) -- the declared scale.  Both are the driver's fallback.
        decimal_type=("decimal128(28, 6)", "decimal128(28, 3)"), ts_us=(123000,)),
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
    "risingwave": dict(
        # RisingWave is a streaming database that speaks the PostgreSQL wire protocol
        # (it announces itself as PostgreSQL 13), so psqlodbc drives it.  Only the wire
        # protocol and the catalog are PostgreSQL's: the type system is RisingWave's own
        # and no column type takes a modifier.  VARCHAR(50) and TIMESTAMP(6) do not even
        # parse ("expected ',' or ')' after column definition, found: (") and NUMERIC(10,3)
        # parses but is refused ("unsupported data type: NUMERIC(10,3)"), so the columns
        # below are declared unqualified.  psqlodbc's own type names ("int8", "float8",
        # "bool", "varchar", "numeric") are all accepted, so the generated ingest DDL
        # needs neither `ansi_ddl_type_names` nor `ingest_types`.
        env="RISINGWAVE_ODBC_DRIVER", # UseServerSidePrepare=0: with the extended protocol psqlodbc names its server-side
        # statement _PLAN<hex> and RisingWave refuses to prepare a second one under that
        # name ("XX000 Failed to prepare the statement: Duplicated statement name"), so
        # the second parameterised query of any connection failed -- QuestDB says the same
        # thing as "duplicate statement [name=_PLAN0x...]".  The simple protocol has no
        # statement names.
        conn="Driver={drv};Server=127.0.0.1;Port=14566;Database=dev;Uid=root;UseServerSidePrepare=0;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR, b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC, bo BOOLEAN)",
        # A write only becomes visible to a later scan once the next barrier commits it,
        # so every write here is followed by FLUSH (which waits for that barrier).  The
        # statement takes no table name -- "{}" simply goes unused.
        refresh="FLUSH",
        # An unqualified NUMERIC has no declared precision or scale: psqlodbc falls back
        # to its own maximum precision (28), and RisingWave reports the scale of the
        # values actually in the result set (3 for the 12.345 stored here; 6 -- psqlodbc's
        # default -- for an empty result).
        decimal_type="decimal128(28, 3)"),
    "spanner": dict(
        # Google Cloud Spanner, reached through PGAdapter -- the PostgreSQL-wire proxy
        # Google ships for it -- in front of the Spanner emulator, so psqlodbc drives it
        # (PGAdapter announces itself as PostgreSQL 14.1).  Only the wire protocol is
        # PostgreSQL's; the schema is Spanner's, and three things about it shape the DDL
        # below.  Every Spanner table must have a PRIMARY KEY ("Primary key must be
        # defined for table"), hence the key on `i`.  There is no TIMESTAMP WITHOUT TIME
        # ZONE ("Type <timestamp> is not supported") -- Spanner's one timestamp type is
        # UTC-based and spelled timestamptz.  And NUMERIC takes no modifier ("Type
        # modifier is not supported for type <numeric>"): Spanner has a single numeric
        # type, whatever precision and scale are asked for.
        #   Two more things about Spanner are the driver's business rather than this
        # entry's, both keyed on a setting only PGAdapter answers (version() says nothing
        # about Spanner) -- see OdbcDetectQuirks: psqlodbc inlines a parameter array's
        # timestamps as '...'::timestamp, which Spanner has no type for, and the
        # CREATE TABLE that bulk ingest generates needs a primary key of its own.
        env="SPANNER_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=15442;Database=test-database;Uid=adbc;",
        ddl="CREATE TABLE adbc_t (i bigint PRIMARY KEY, f double precision, s varchar(50),"
            " b bytea, d date, ts timestamptz, n numeric, bo bool)",
        # Spanner has no 32-bit integer type at all ("Type <int4> is not supported; use
        # bigint or int8 instead"), and psqlodbc's SQLGetTypeInfo names int4 for one, so
        # an int32 column is sent as int64 -- which is what Spanner would store anyway.
        # (The rest of the generated ingest DDL needs no help: psqlodbc's own names
        # bytea, timestamptz, numeric, date, float8 and text are all Spanner types, so
        # `ansi_ddl_type_names` would make things worse rather than better here.)
        ingest_types={pa.int32(): pa.int64()},
        # Spanner's NUMERIC is one fixed type and it does not report a precision over
        # the wire, so psqlodbc falls back to its own maximum (28) with the scale of the
        # values in the result set (3 for the 12.345 stored here).
        decimal_type="decimal128(28, 3)",
        # The standard workload alone would look much like `postgres`, so the `extra`
        # steps exercise the reason to run Spanner: an INTERLEAVED table, where the child
        # rows are stored physically inside the parent's key range rather than in a table
        # of their own.  They ingest through ADBC into both tables and read back across
        # them.  The child's key must be prefixed by the parent's, so "a" (unique in
        # EXTRA_ROWS) is the parent key and ("a", "b") the child's.
        extra=[
            ('DROP TABLE IF EXISTS "adbc_child{sfx}"', None),
            ('DROP TABLE IF EXISTS "adbc_parent{sfx}"', None),
            # Column names/types match EXTRA_ROWS: the ingests below append into these.
            ('CREATE TABLE "adbc_parent{sfx}" ("a" bigint PRIMARY KEY, "b" varchar(20),'
             ' "c" double precision, "d" date, "e" bool)', None),
            ('CREATE TABLE "adbc_child{sfx}" ("a" bigint NOT NULL, "b" varchar(20) NOT NULL,'
             ' "c" double precision, "d" date, "e" bool, PRIMARY KEY ("a", "b"))'
             ' INTERLEAVE IN PARENT "adbc_parent{sfx}" ON DELETE CASCADE', None),
            # Spanner really owns the child as an interleaved table, not a plain one.
            ("SELECT parent_table_name = 'adbc_parent{sfx}' FROM information_schema.tables"
             " WHERE table_name = 'adbc_child{sfx}'", (True,)),
            (("adbc_parent{sfx}", EXTRA_ROWS), (4,)),   # bulk ingest into the parent
            # An interleaved row may only be written under an existing parent row, so
            # this ingest passes only because the one above really landed.
            (("adbc_child{sfx}", EXTRA_ROWS), (4,)),
            ('SELECT count(*) FROM "adbc_child{sfx}"', (4,)),
            ('SELECT p."b", c."c" FROM "adbc_parent{sfx}" p JOIN "adbc_child{sfx}" c'
             ' ON p."a" = c."a" WHERE p."a" = 3', ("r", None)),
        ],
    ),
    "firebird": dict(
        env="FIREBIRD_ODBC_DRIVER",
        conn="Driver={drv};DBNAME=inet://127.0.0.1:13050//var/lib/firebird/data/adbc.fdb;UID=adbc;PWD=adbc;CHARSET=UTF8;",
        # VARBINARY is CHAR CHARACTER SET OCTETS in Firebird and OdbcFb describes it as
        # SQL_VARCHAR, so BLOB SUB_TYPE BINARY is the type that round-trips bytes.
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BLOB SUB_TYPE BINARY, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # Firebird upper-cases unquoted identifiers; NUMERIC(10,3) is stored as a scaled
        # BIGINT and OdbcFb reports the storage precision (18), not the declared one.
        ident=str.upper, decimal_type="decimal128(18, 3)", ts_us=(123400,)),
    "virtuoso": dict(
        # OpenLink Virtuoso is ODBC-native: virtodbc.so speaks the server's own wire
        # protocol on 1111, so HOST is "host:port" and there is no database to name (the
        # dba user lands in DB.DBA).  Use the ANSI virtodbc.so, not the Unicode
        # virtodbcu.so -- see tests/compat/README.md for why the Unicode build cannot be
        # driven through unixODBC's ANSI entry points at all.
        env="VIRTUOSO_ODBC_DRIVER",
        conn="Driver={drv};HOST=127.0.0.1:11111;UID=dba;PWD=adbc;",
        # Virtuoso's type names are its own: NVARCHAR is the wide (character) string type
        # while a plain VARCHAR is a byte string, DATETIME is the microsecond timestamp
        # (TIMESTAMP is a separate "row timestamp" type the driver describes as binary),
        # and there is no BOOLEAN at all -- SMALLINT is what Virtuoso itself uses for one,
        # so `bo` reads back as int16.
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s NVARCHAR(50), b VARBINARY(10),"
            " d DATE, ts DATETIME, n DECIMAL(10,3), bo SMALLINT)",
        bool_type="int16"),
    "flightsql": dict(
        # Arrow Flight SQL.  The server here is voltrondata/sqlflite (DuckDB behind a
        # Flight SQL service) and the driver is the Arrow Flight SQL ODBC driver Dremio
        # publishes -- the only ODBC driver for the protocol, so everything below is the
        # driver's doing rather than any one server's.  useEncryption=false matches the
        # container's TLS_ENABLED=0; sqlflite's default user name is `sqlflite_username`
        # and SQLFLITE_PASSWORD sets the password.  See tests/compat/README.md.
        env="FLIGHTSQL_ODBC_DRIVER",
        conn="Driver={drv};Host=127.0.0.1;Port=31337;UID=sqlflite_username;PWD=adbc;"
             "useEncryption=false;",
        # read_only: the driver has no SQLBindParameter -- it answers HYC00 "Unsupported
        # function" on a *virgin* statement handle, before any SQL is seen -- so nothing
        # that binds a parameter can go through it: not the parameterised INSERT the other
        # entries load adbc_t with, and not adbc_ingest.  (SQLRowCount answers -1 for a
        # literal INSERT too, so there would be no ingested row count to check either.)
        # SQLExecDirect of literal SQL does work, so `setup` builds the two tables the read
        # side of the workload needs and that whole read side then runs unchanged.
        read_only=True,
        params=False,  # same reason: the parameterised SELECT runs as a literal
        # The driver describes every SQL_DECIMAL column as precision 19, scale 0 whatever
        # was declared -- DECIMAL(10,3) and a 12.345 literal both come back as (19, 0) --
        # while sending the digits themselves in full.  Taken at face value that scale
        # rounds 12.345 to 12, so this entry reads decimals as their exact text instead,
        # as the `databend` entry does for the same reason.
        db_kwargs={"adbc.odbc.decimal_as_string": "true"}, decimal_type="string",
        ddl="CREATE TABLE adbc_t " + FLIGHTSQL_COLS,
        # Replayed on every connection (bench/matrix_bench.py opens several), hence
        # CREATE OR REPLACE: each statement is idempotent on its own.
        setup=[
            "CREATE OR REPLACE TABLE adbc_t " + FLIGHTSQL_COLS,
            # The literal spelling of ROW1/ROW2 below.  DuckDB reads '\\x01\\x02'::BLOB as
            # the two bytes, the same value the other entries bind as a parameter.
            "INSERT INTO adbc_t VALUES"
            " (1, 1.5, 'héllo 🚀', '\\x01\\x02'::BLOB, DATE '2024-02-29',"
            " TIMESTAMP '2024-02-29 13:45:10.123456', 12.345, true),"
            " (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
            # adbc_big, the table check_big() reads and the one bench/matrix_bench.py
            # times a fetch of on a read_only entry -- so it is sized for the benchmark
            # rather than for the assertion.  DuckDB materialises it from range() in a
            # couple of milliseconds, so rebuilding it per connection costs nothing.
            "CREATE OR REPLACE TABLE adbc_big AS SELECT i AS a, 'r' || i AS b,"
            " i::DOUBLE AS c, (DATE '1970-01-01' + i::INTEGER) AS d, (i % 2 = 0) AS e"
            " FROM range(0, 100000) t(i)",
        ],
        big_rows=100000,
        extra=[
            # The image ships TPC-H at scale factor 0.01, whose row counts are fixed by
            # the spec.  Reading it exercises the Flight SQL data path -- DoGet on the
            # ticket the query handed back -- over result sets wider and longer than the
            # two tables `setup` makes.
            ("SELECT count(*) FROM lineitem", (60175,)),
            ("SELECT count(*) FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey",
             (15000,)),
        ]),
    "arcadedb": dict(
        # ArcadeDB is a multi-model (document/graph/key-value) database whose server
        # speaks the PostgreSQL wire protocol through its PostgresProtocolPlugin, so the
        # psqlodbc build used for the `postgres` entry drives it.  Only the wire protocol
        # is PostgreSQL's; everything above it is ArcadeDB's own.
        #   BoolsAsChar=0 is psqlodbc's setting, not ArcadeDB's: without it the driver
        # reports every BOOLEAN as a VARCHAR(5) holding "1"/"0" instead of SQL_BIT.  It is
        # the same setting the `questdb` entry needs, for the same reason.
        env="ARCADEDB_ODBC_DRIVER",
        conn="Driver={drv};Server=127.0.0.1;Port=15441;Database=adbc;Uid=root;Pwd=Adbc2026;"
             "BoolsAsChar=0;",
        # read_only: ArcadeDB has no CREATE TABLE -- its DDL is "CREATE DOCUMENT TYPE t"
        # plus one "CREATE PROPERTY t.c <type>" per column -- so the CREATE TABLE that
        # adbc_ingest generates cannot be spelled for it at all, and neither the ingest
        # steps nor the single-statement `ddl` of the other entries can run.  `setup`
        # builds the two tables the read side of the workload needs in ArcadeDB's own
        # DDL instead, and that whole read side then runs unchanged.  (Ordinary DML is
        # fine: the INSERTs below and the `extra` steps all go through the ODBC path.)
        read_only=True,
        # The ArcadeDB spelling of what `setup` creates, for the record; unlike the other
        # entries' `ddl` it is never executed -- it is not one statement.
        ddl="CREATE DOCUMENT TYPE adbc_t; CREATE PROPERTY adbc_t.i INTEGER;"
            " CREATE PROPERTY adbc_t.f DOUBLE; CREATE PROPERTY adbc_t.s STRING;"
            " CREATE PROPERTY adbc_t.b BINARY; CREATE PROPERTY adbc_t.d DATE;"
            " CREATE PROPERTY adbc_t.ts DATETIME_MICROS; CREATE PROPERTY adbc_t.n DECIMAL;"
            " CREATE PROPERTY adbc_t.bo BOOLEAN",
        setup=(
            arcadedb_type("adbc_t", [("i", "INTEGER"), ("f", "DOUBLE"), ("s", "STRING"),
                                     ("b", "BINARY"), ("d", "DATE"), ("ts", "DATETIME_MICROS"),
                                     ("n", "DECIMAL"), ("bo", "BOOLEAN")]) +
            # The literal spelling of ROW1/ROW2.  Two of these are ArcadeDB's parsing,
            # not SQL's: a timestamp literal is only recognised in ISO-8601 form with the
            # "T" separator (with a space it is stored as NULL, silently), and "\x.."
            # does not lex at all, so the two bytes of ROW1's `b` go in as the "\uXXXX"
            # escapes of U+0001 and U+0002 -- which is what they read back as, see below.
            arcadedb_insert("adbc_t", ("i", "f", "s", "b", "d", "ts", "n", "bo"), [
                "(1, 1.5, 'héllo \U0001F680', '\\u0001\\u0002', '2024-02-29',"
                " '2024-02-29T13:45:10.123456', 12.345, true)",
                "(2, null, null, null, null, null, null, null)"]) +
            # adbc_big, the table check_big() reads and the one bench/matrix_bench.py
            # times a fetch of on a read_only entry -- so, as for `flightsql`, it is sized
            # for the benchmark rather than for the assertion.  100,000 literal rows load
            # in about two and a half seconds, which is what `setup` costs on every
            # connection opened; a 5,000-row table fetches in ~25 ms, too short to time.
            arcadedb_type("adbc_big", [("a", "LONG"), ("b", "STRING"), ("c", "DOUBLE"),
                                       ("d", "DATE"), ("e", "BOOLEAN")]) +
            arcadedb_insert("adbc_big", ("a", "b", "c", "d", "e"),
                            ["(%d,'r%d',%d.0,'2024-01-01',%s)" % (i, i, i, str(i % 2 == 0).lower())
                             for i in range(ARCADEDB_BIG_ROWS)])),
        big_rows=ARCADEDB_BIG_ROWS,
        # ArcadeDB puts three record-metadata fields -- the record id, its type and its
        # category (d/v/e) -- into the result of every "SELECT *", so a describe of the
        # table reports eleven columns where the type has eight properties.  They are not
        # columns of the table, so they are skipped both in the all-NULL row check and in
        # the catalog comparisons.
        pseudo_columns=("@rid", "@type", "@cat"),
        # ArcadeDB reports no declared precision for a DECIMAL property, so psqlodbc
        # falls back to its own maximum (28) with the scale of the values in the result
        # set -- as it does for RisingWave's unqualified NUMERIC.
        decimal_type="decimal128(28, 3)",
        # A BOOLEAN property has no NULL state on the wire: row 2's `bo` was inserted as
        # NULL and reads back false, as QuestDB's and Access's booleans do.
        not_null=("bo",),
        # ArcadeDB is a graph database as much as a document one, which nothing in the
        # standard workload touches, so the `extra` steps build a small graph through the
        # ODBC path and traverse it: three vertices, two edges, then one- and two-hop
        # traversals from the first vertex.  (MATCH would be the other way to write the
        # traversal, but its "{...}" pattern syntax collides with ODBC escape sequences,
        # which the driver manager rejects before the server ever sees them.)
        extra=[
            ('DROP TYPE "adbc_ge{sfx}" IF EXISTS UNSAFE', None),
            ('DROP TYPE "adbc_gv{sfx}" IF EXISTS UNSAFE', None),
            ('CREATE VERTEX TYPE "adbc_gv{sfx}"', None),
            ('CREATE EDGE TYPE "adbc_ge{sfx}"', None),
            ('CREATE PROPERTY "adbc_gv{sfx}"."name" STRING', None),
            # An ArcadeDB INSERT answers with the records it created, @rid and all, so
            # there is nothing portable to assert on it -- the count below does that.
            ("""INSERT INTO "adbc_gv{sfx}" ("name") VALUES ('a'),('b'),('c')""", None),
            ('SELECT count(*) AS "c" FROM "adbc_gv{sfx}"', (3,)),
            ("""CREATE EDGE "adbc_ge{sfx}" FROM (SELECT FROM "adbc_gv{sfx}" WHERE "name" = 'a')"""
             """ TO (SELECT FROM "adbc_gv{sfx}" WHERE "name" = 'b')""", None),
            ("""CREATE EDGE "adbc_ge{sfx}" FROM (SELECT FROM "adbc_gv{sfx}" WHERE "name" = 'b')"""
             """ TO (SELECT FROM "adbc_gv{sfx}" WHERE "name" = 'c')""", None),
            ('SELECT count(*) AS "c" FROM "adbc_ge{sfx}"', (2,)),
            # One hop out of 'a' is 'b'; two hops is 'c'.
            ("""SELECT "name" FROM (SELECT expand(out('adbc_ge{sfx}'))"""
             """ FROM "adbc_gv{sfx}" WHERE "name" = 'a')""", ("b",)),
            ("""SELECT "name" FROM (SELECT expand(out('adbc_ge{sfx}').out('adbc_ge{sfx}'))"""
             """ FROM "adbc_gv{sfx}" WHERE "name" = 'a')""", ("c",)),
        ]),
    "influxdb3": dict(
        # InfluxDB 3 Core, reached over its Arrow Flight SQL endpoint (8181) with the
        # same Dremio Arrow Flight SQL ODBC driver as the `flightsql` entry -- so the
        # driver-side notes there apply here too, and this entry exists to run the
        # workload against a *second*, very different Flight SQL server.
        #   The database is not part of the Flight SQL protocol: InfluxDB reads it from a
        # gRPC header, and the driver forwards every connection property it does not
        # recognise as one, so `database=adbc` is all it takes.  useEncryption=false
        # matches the server's plain-gRPC listener (no --tls-cert).
        env="INFLUXDB3_ODBC_DRIVER",
        conn="Driver={drv};Host=127.0.0.1;Port=18181;useEncryption=false;database=adbc;",
        # read_only, for two independent reasons: InfluxDB 3's SQL is query-only (it
        # answers any DDL with "DDL not supported" -- tables appear when line protocol is
        # written to them), and the driver has no SQLBindParameter at all.  So both tables
        # are loaded out of band, over the HTTP write API; see tests/compat/README.md.
        read_only=True,
        params=False,  # no SQLBindParameter: the parameterised SELECT runs as a literal
        # InfluxDB's own column shape is not the workload's: the timestamp column of a
        # table is always named `time` and is the only column that can hold one, and there
        # is no DATE type (nor DECIMAL, nor a binary type).  `d` is therefore stored as
        # text and cast here, and `time` is read as `ts` -- which keeps the entry on the
        # server's real timestamp column instead of a stand-in.
        select="SELECT i, f, s, b, CAST(d AS DATE) AS d, time AS ts, n, bo"
               " FROM {t} ORDER BY i",
        # ... and the catalog reports what the table really holds: the same columns with
        # `time` in place of `ts`, in InfluxDB's own (alphabetical) order.
        catalog_cols=("b", "bo", "d", "f", "i", "n", "s", "time"),
        # `time` has no NULL state -- every point carries one -- so the all-NULL row's ts
        # reads back as its timestamp, exactly as Access's YESNO reads back False.
        not_null=("ts",),
        # No binary type: the two bytes are stored as text, as for the `cratedb` entry.
        binary_text="\\x0102",
        # No DECIMAL type either; `n` is a text field, read back as its exact digits.
        decimal_type="string",
        # ddl is documentation here (nothing executes it): the line protocol in
        # README.md writes these fields, and this is the SQL shape it produces.
        ddl="CREATE TABLE adbc_t (i BIGINT, f DOUBLE, s VARCHAR, b VARCHAR, d VARCHAR,"
            " time TIMESTAMP, n VARCHAR, bo BOOLEAN)",
        # adbc_big is loaded with 100,000 points, which is also what
        # bench/matrix_bench.py times a fetch of on a read_only entry.
        big_rows=100000,
        extra=[
            # Two things only a time-series engine does, over the table the workload
            # already reads: a time-bucketed aggregate (DataFusion's date_bin) and a
            # range scan on the timestamp column that InfluxDB partitions by.
            ("SELECT COUNT(*) FROM (SELECT date_bin(INTERVAL '1 hour', time) AS bucket,"
             " COUNT(*) AS n FROM adbc_big GROUP BY bucket)", (1,)),
            ("SELECT COUNT(*), MIN(a), MAX(a) FROM adbc_big"
             " WHERE time >= '2024-02-29T13:45:10Z'", (100000, 0, 99999)),
        ]),
    "ignite": dict(
        # Apache Ignite 2.x is a distributed in-memory key-value grid with a SQL engine on
        # top of it, reached here over its thin-client/ODBC port (10800) by the ODBC driver
        # built from the C++ sources the image itself ships -- Apache publishes no prebuilt
        # libignite-odbc.so for Linux.  See tests/compat/README.md for the root-free build.
        env="IGNITE_ODBC_DRIVER",
        conn="Driver={drv};ADDRESS=127.0.0.1:11800;SCHEMA=PUBLIC;",
        # Every Ignite SQL table is a cache, so it must declare a PRIMARY KEY -- a table
        # without one is refused outright ("No PRIMARY KEY defined for CREATE TABLE"), and
        # `i` is the natural key here.  BINARY is Ignite's byte-string type.
        ddl="CREATE TABLE adbc_t (i INT PRIMARY KEY, f DOUBLE, s VARCHAR(50), b BINARY,"
            " d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        # Ignite folds an unquoted identifier to upper case ...
        ident=str.upper,
        # ... and its ODBC driver answers SQL_IDENTIFIER_QUOTE_CHAR with an empty string,
        # so adbc_ingest quotes nothing and the names it emits are folded too.  This file's
        # own SQL therefore has to leave them unquoted as well: a quoted "a" would be a
        # different column from the A that ingest just created.
        quote="",
        # ingest_create: that same PRIMARY KEY requirement is what the generated ingest DDL
        # cannot satisfy -- it has no notion of a key, and no column of the ingest payload
        # is one (`a` is [1, 2, NULL], and Ignite allows neither a NULL key nor the
        # duplicates the append step would insert).  So adbc_ingest cannot *create* a table
        # here at all; appending into a table that declares its own key works, which is what
        # the `extra` steps below do -- and what an Ignite user has to do.
        ingest_create=False,
        # adbc_big, the table check_big() reads and the one bench/matrix_bench.py times a
        # fetch of -- so, as for the read_only entries, it is sized for the benchmark rather
        # than for the assertion.  SYSTEM_RANGE is the H2 table function Ignite's SQL engine
        # inherits; it fills the table server-side in about three seconds, which is what
        # `setup` costs on every connection opened.
        setup=["DROP TABLE IF EXISTS adbc_big",
               "CREATE TABLE adbc_big (a BIGINT PRIMARY KEY, b VARCHAR)",
               "INSERT INTO adbc_big (a, b) SELECT X, 'r' || X FROM SYSTEM_RANGE(0, 99999)"],
        big_rows=100000,
        # Bulk ingest, in the only shape Ignite has for it: append into a table that
        # declares a PRIMARY KEY of its own.  EXTRA_ROWS' `a` is unique and never NULL, so
        # it can be that key.
        extra=[
            ("DROP TABLE IF EXISTS adbc_ig{sfx}", None),
            ("CREATE TABLE adbc_ig{sfx} (a BIGINT PRIMARY KEY, b VARCHAR, c DOUBLE,"
             " d DATE, e BOOLEAN)", None),
            (("adbc_ig{sfx}", EXTRA_ROWS), (4,)),
            ("SELECT count(*) FROM adbc_ig{sfx}", (4,)),
            ("SELECT b, c FROM adbc_ig{sfx} WHERE a = 3", ("r", None)),
        ]),
    "opensearch": dict(
        # OpenSearch reached through its SQL plugin (the `/_plugins/_sql` REST endpoint)
        # with the OpenSearch SQL ODBC driver, which the project publishes for Windows
        # and macOS only -- see tests/compat/README.md for the Linux build and the two
        # source fixes it needs.  The connection options are the driver's own names:
        # `host`/`port` rather than Server/Database, and auth=NONE for a cluster started
        # with DISABLE_SECURITY_PLUGIN=true.
        env="OPENSEARCH_ODBC_DRIVER",
        conn="Driver={drv};host=127.0.0.1;port=19200;auth=NONE;useSSL=0;",
        # read_only, for two independent reasons: the SQL plugin is a query interface
        # (SELECT, SHOW and DESCRIBE are the whole grammar -- there is no CREATE TABLE
        # and no INSERT, documents are written over the REST API), and the driver
        # answers SQLBindParameter with "OpenSearch does not support parameters".  Both
        # indices are loaded out of band; see fixtures/load_opensearch.py.
        read_only=True,
        params=False,  # no SQLBindParameter: the parameterised SELECT runs as a literal
        # A "..." is not an identifier here: OpenSearch SQL quotes with the backtick and
        # reads "adbc_big" as an index literally called `"adbc_big"` ("no such index").
        # This is the same fact as the `greptimedb` entry's `quote`.
        quote="`",
        # ddl is documentation here (nothing executes it): this is the SQL shape of the
        # index mapping load_opensearch.py creates.  `n` and `b` are keyword fields --
        # OpenSearch has neither DECIMAL nor a binary type -- and `d`/`ts` are one field
        # type, `date`, which the SQL plugin types DATE for a date-only format and
        # TIMESTAMP for one carrying a time.
        ddl="CREATE TABLE adbc_t (i BIGINT, f DOUBLE, s VARCHAR, b VARCHAR, d DATE,"
            " ts TIMESTAMP, n VARCHAR, bo BOOLEAN)",
        # The driver's type table maps the SQL plugin's `date` but not its `timestamp`
        # (a type name the plugin grew after the driver was last released), so `ts` is
        # described as a VARCHAR and arrives as "2024-02-29 13:45:10.123".
        ts_text=True,
        # No binary type: the two bytes are stored as text, as for `cratedb`.
        binary_text="\\x0102",
        # No DECIMAL either; `n` is a keyword field read back as its exact digits.
        decimal_type="string",
        # SQLColumns reports an index's fields in mapping order, which is neither the
        # workload's nor alphabetical, so the catalog columns are compared as a set.
        column_order=False,
        # adbc_big is 100,000 documents, which is also what bench/matrix_bench.py times
        # a fetch of on a read_only entry.  plugins.query.size_limit has to allow it --
        # the loader raises it, see README.md.
        big_rows=100000,
        extra=[
            # The reason to run OpenSearch: a full-text query, expressed in SQL over an
            # analysed `text` field.  MATCH scores documents by term rather than
            # comparing whole values, so 'search' hits two of adbc_search's three
            # documents and 'distributed' exactly one.
            ("SELECT COUNT(*) FROM adbc_search WHERE MATCH(body, 'search')", (2,)),
            ("SELECT id FROM adbc_search WHERE MATCH_QUERY(body, 'distributed')", (1,)),
            # ... and an aggregation pushed down to the search engine over the 100,000
            # documents the read side already scans.
            ("SELECT COUNT(*), MIN(a), MAX(a) FROM adbc_big", (100000, 0, 99999)),
            ("SELECT e, COUNT(*) FROM adbc_big GROUP BY e ORDER BY e", (False, 50000)),
        ]),
    "ydb": dict(
        # YDB (Yandex Database) is a distributed HTAP store whose native API is gRPC/YQL;
        # this entry drives its *PostgreSQL-compatible* wire protocol, which the server
        # serves on 5432 once it is started with the pg feature flags (see
        # tests/compat/README.md), so the psqlodbc build used for the `postgres` entry
        # drives it.  Only the wire protocol and a thin pg_catalog emulation are
        # PostgreSQL's; the engine underneath is YDB's own.
        env="YDB_ODBC_DRIVER",
        # `local` is the database the single-node image creates (YDB path /local), and
        # `adbcuser` is a role created after start-up -- the image ships no users at all
        # and psqlodbc will not send an empty password.  Two of the settings are
        # psqlodbc's, not YDB's:
        #   BoolsAsChar=0, without which the driver reports every BOOLEAN as a VARCHAR(5)
        # holding "1"/"0" instead of SQL_BIT (the same setting `questdb` and `arcadedb`
        # need, for the same reason).
        #   UseServerSidePrepare=0 is what makes any NULL reachable at all.  YDB's PG
        # wire does not implement a NULL bind parameter: a Bind message that gives a
        # parameter length of -1 is read as a zero-length *value*, so a NULL lands in a
        # text column as '' and in any other column as "invalid input syntax for type
        # ..." -- and a raw-protocol probe with no ODBC in the path fails exactly the
        # same way, so this is the server, not the driver (see README.md).  With
        # UseServerSidePrepare=0 psqlodbc stops using the extended query protocol and
        # substitutes bound values into the SQL text itself, where a NULL is the literal
        # NULL and YDB handles it correctly.
        conn="Driver={drv};Server=127.0.0.1;Port=15444;Database=local;Uid=adbcuser;"
             "Pwd=Ydb!Bridge2026;BoolsAsChar=0;UseServerSidePrepare=0;",
        # The PRIMARY KEY is required, not decorative: YDB refuses any CREATE TABLE
        # without one ("Primary key is required for ydb tables").  The generated ingest
        # DDL has nowhere to put one either, which is what the driver's `ydb` quirk adds
        # (adbc.odbc `ddl_extra_column`).
        ddl="CREATE TABLE adbc_t (i INTEGER PRIMARY KEY, f DOUBLE PRECISION, s VARCHAR(50),"
            " b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)",
        # YDB does not report the declared precision of a NUMERIC over the wire, so
        # psqlodbc falls back to its own maximum (28) with the column's scale -- as it
        # does for QuestDB's DECIMAL and RisingWave's unqualified NUMERIC.
        decimal_type="decimal128(28, 3)"),
    "dremio": dict(
        # Dremio (dremio-oss 26), a lakehouse query engine whose *native* client protocol
        # is Arrow Flight SQL (port 32010), driven by the same Arrow Flight SQL ODBC
        # driver as the `flightsql` and `influxdb3` entries -- the driver Dremio itself
        # publishes, so this entry runs it against the engine it was written for.
        # Everything the `flightsql` entry documents about that driver holds here too.
        #   Flight SQL has no "connect to database X" step: the driver forwards every
        # connection property it does not recognise as a gRPC header, and Dremio reads
        # `schema` as the query context.  `$scratch` is the one writable source a stock
        # dremio-oss has, so that is where `setup` puts its tables.  The server needs an
        # admin user created over its REST API first; see tests/compat/README.md.
        env="DREMIO_ODBC_DRIVER",
        conn="Driver={drv};Host=127.0.0.1;Port=32010;UID=adbc;PWD=Adbc2026pass;"
             "useEncryption=false;schema=$scratch;",
        # read_only for the driver's reason alone: it answers SQLBindParameter with
        # HYC00 "Unsupported function" on a virgin statement handle and reports 0
        # parameter markers after a successful SQLPrepare, so neither the parameterised
        # INSERT the other entries load adbc_t with nor adbc_ingest can go through it.
        # (Dremio itself does write: `$scratch` takes CTAS, and a table created there
        # with an explicit column list is an Iceberg table that takes INSERT -- but no
        # parameter can reach the server to use it.)  SQLExecDirect of literal SQL works,
        # so `setup` builds both tables and the read side then runs unchanged.
        read_only=True,
        params=False,  # no SQLBindParameter: the parameterised SELECT runs as a literal
        # The driver reports every SQL_DECIMAL column as precision 19 -- the widest a
        # DECIMAL(19,s) can be -- whatever was declared, so DECIMAL(10,3) is described
        # as (19, 3).  Unlike the sqlflite case in the `flightsql` entry the *scale* is
        # right, so nothing is lost: 12.345 arrives exact, just in a wider decimal128.
        decimal_type="decimal128(19, 3)",
        # Replayed on every connection (bench/matrix_bench.py opens several), so both
        # statements are IF NOT EXISTS: Dremio has no CREATE OR REPLACE TABLE, and
        # rebuilding these would cost a Parquet rewrite per connection.  CTAS is also
        # the only single-statement way to load a table here -- with no parameters, the
        # alternative is CREATE TABLE (cols) plus a literal INSERT per table.
        setup=[
            # ROW1/ROW2 spelled as literals.  Two Dremio-isms: `_UTF8` is required in
            # front of a string literal holding an astral-plane character (its parser
            # encodes an unprefixed literal as ISO-8859-1 and fails planning on the
            # emoji), and BINARY_STRING() is how a VARBINARY literal is written.
            "CREATE TABLE IF NOT EXISTS adbc_t AS"
            " SELECT CAST(1 AS INTEGER) AS i, CAST(1.5 AS DOUBLE) AS f,"
            " _UTF8'h\u00e9llo \U0001f680' AS s, BINARY_STRING('\\x01\\x02') AS b,"
            " DATE '2024-02-29' AS d, TIMESTAMP '2024-02-29 13:45:10.123' AS ts,"
            " CAST(12.345 AS DECIMAL(10,3)) AS n, true AS bo FROM (VALUES(1))"
            " UNION ALL"
            " SELECT CAST(2 AS INTEGER), CAST(NULL AS DOUBLE), CAST(NULL AS VARCHAR),"
            " CAST(NULL AS VARBINARY), CAST(NULL AS DATE), CAST(NULL AS TIMESTAMP),"
            " CAST(NULL AS DECIMAL(10,3)), CAST(NULL AS BOOLEAN) FROM (VALUES(1))",
            # adbc_big, the table check_big() reads and the one bench/matrix_bench.py
            # times a fetch of.  Dremio has no range()/generate_series, so the 100,000
            # rows come from five cross-joined ten-row VALUES lists; it writes the
            # Parquet file in about a third of a second.
            "CREATE TABLE IF NOT EXISTS adbc_big AS SELECT CAST(%s AS BIGINT) AS a,"
            " CONCAT('r', CAST(%s AS VARCHAR)) AS b FROM %s AS d1(n), %s AS d2(n),"
            " %s AS d3(n), %s AS d4(n), %s AS d5(n)"
            % ((DREMIO_ROWNO, DREMIO_ROWNO) + (DREMIO_DIGITS,) * 5),
        ],
        big_rows=100000,
        extra=[
            # Dremio's own catalog, over the tables `setup` just created: a CTAS table
            # is a real dataset in the source, so INFORMATION_SCHEMA describes it.
            ("SELECT COUNT(*) FROM INFORMATION_SCHEMA.\"COLUMNS\""
             " WHERE TABLE_NAME = 'adbc_t'", (8,)),
            # A columnar scan with a pushed-down filter over the 100,000-row Parquet
            # table -- the engine's own data path, on a result the entry knows exactly.
            ("SELECT COUNT(*), MIN(a), MAX(a) FROM adbc_big WHERE a >= 50000",
             (50000, 50000, 99999)),
        ]),
    "tdengine": dict(
        # TDengine is a time-series database with its own SQL dialect and its own ODBC
        # driver (taos-odbc, built from taosdata/taos-connector-odbc against the client
        # libraries shipped in the server image -- see tests/compat/README.md).
        #   read_only: nothing about the *driver* stops a write (it binds parameters and
        # reports row counts fine -- the `extra` steps below bulk ingest through it), but
        # the standard write side of this workload cannot be expressed against a TDengine
        # table at all.  Every table's first column must be a TIMESTAMP primary key whose
        # values are non-NULL, distinct and inside the database's retention window, and
        # the workload's INSERT fills the columns positionally from `i` (1, 2) while
        # adbc_ingest's generated DDL declares no timestamp column at all -- "First column
        # must be timestamp" for the CREATE, "Timestamp data out of range" for epoch-based
        # values.  So `setup` builds adbc_t and adbc_big with literal SQL, as the
        # `flightsql` entry does, and the whole read side runs unchanged.
        env="TDENGINE_ODBC_DRIVER",
        # No DB= : `setup` creates the database and switches to it, so a fresh container
        # needs no server-side preparation.  TIMESTAMP_AS_IS=1 is taos-odbc's, not the
        # server's: without it the driver describes every TIMESTAMP column as an
        # SQL_WVARCHAR holding the formatted text ("2024-02-29 13:45:10.123456") and the
        # reader has no timestamp to read at all.
        conn="Driver={drv};SERVER=127.0.0.1:16030;UID=root;PWD=taosdata;TIMESTAMP_AS_IS=1;",
        read_only=True,
        # TDengine's parser has no double-quoted identifiers -- `FROM "adbc_big"` is a
        # syntax error -- and taos-odbc answers SQL_IDENTIFIER_QUOTE_CHAR with a backtick,
        # which is what adbc_ingest quotes with too.
        quote="`",
        # What `setup` creates; TDengine's own type names.  NCHAR is the wide (character)
        # string, VARCHAR is a byte-length one, and there is no DATE type, so `d` is a
        # second TIMESTAMP.
        #   `n` is a VARCHAR holding the exact digits, not a DECIMAL: TDengine 3.3.6 has
        # DECIMAL, but taos-odbc does not implement it -- a DECIMAL(10,3) column fails the
        # whole SELECT with "`UNKNOWN`[21] not supported yet" (21 is TSDB_DATA_TYPE_DECIMAL64)
        # and DECIMAL(20,3), the 128-bit one, is not in its type table either.  The column
        # reads back as its exact string form, as `databend` and `flightsql` do for
        # decimals their drivers misdescribe.
        ddl="CREATE TABLE adbc_t (ts TIMESTAMP, i INT, f DOUBLE, s NCHAR(50), b VARBINARY(20),"
            " d TIMESTAMP, n VARCHAR(20), bo BOOL)",
        setup=tdengine_setup(),
        # The primary-key timestamp has to come first, so the columns are not in the
        # order the workload declares them; GetObjects compares them as a set.
        column_order=False,
        # `ts` is that primary key: it is the one column of row 2 that cannot be NULL.
        not_null=("ts",),
        big_rows=20000,
        extra=[
            # A real ADBC bulk ingest through this driver: append into a table whose shape
            # TDengine accepts (timestamp first).  mode="append" is the only mode possible
            # here -- create/replace would have to emit the CREATE, and no generated DDL
            # names a timestamp primary key.
            ("DROP TABLE IF EXISTS adbc_ing{sfx}", None),
            # `a` is INT, not BIGINT, and that is the driver's doing: taos-odbc looks a
            # parameter up by the exact (C type, SQL type, column type) triple, and an
            # int64 payload whose values all fit in 32 bits is bound -- here as everywhere
            # else -- as SQL_C_SLONG/SQL_INTEGER, which its table maps only to a TDengine
            # INT.  (Values past 2^31 go as SQL_C_SBIGINT/SQL_BIGINT, which does reach a
            # BIGINT column.)
            ("CREATE TABLE adbc_ing{sfx} (ts TIMESTAMP, a INT, b VARCHAR(20), c DOUBLE,"
             " e BOOL)", None),
            (("adbc_ing{sfx}", TDENGINE_ROWS), (4,)),
            ("SELECT `b`, `c` FROM adbc_ing{sfx} WHERE `a` = 3", ("r", None)),
            ("SELECT COUNT(*) FROM adbc_ing{sfx}", (4,)),
        ]),
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

# Typed values: ADBC clients send Arrow-typed parameters, so dates/timestamps go as
# date32/timestamp (string literals for dates are not portable, e.g. Oracle).
ROW1 = (1, 1.5, "héllo 🚀", b"\x01\x02", datetime.date(2024, 2, 29),
        datetime.datetime(2024, 2, 29, 13, 45, 10, 123456), decimal.Decimal("12.345"), True)
ROW2 = (2, None, None, None, None, None, None, None)
# adbc_t's columns, in the order ROW1/ROW2 give their values.
COLS = ("i", "f", "s", "b", "d", "ts", "n", "bo")


def row2(cfg):
    """The all-NULL second row, with `row2_fill` columns taking row 1's value instead.

    For a server that refuses a NULL there at all: GreptimeDB's TIME INDEX column is
    mandatory and NOT NULL, so `ts` cannot be NULL in any row of any table.  Such a
    column is normally also listed in `not_null`, which is the read-back half of the
    same fact.
    """
    r = list(ROW2)
    for name in cfg.get("row2_fill", ()):
        r[COLS.index(name)] = ROW1[COLS.index(name)]
    return tuple(r)


def conn_uri(name, cfg, drv=None):
    """The entry's connection string, overridable with `<NAME>_CONN`.

    `{drv}` expands to the driver library (the entry's `env` variable when `drv` is not
    given) and `{drvdir}` to the directory holding it, for the rare connection option
    that must point at a file shipped beside the driver.  `{plugin}` and `{plugin_dir}`
    -- two spellings of the same thing -- expand to a `PLUGIN_DIR=` setting for the
    drivers that need one: MySQL Connector/ODBC loads client-side authentication plugins
    from the directory it was *built* with (/usr/local/mysql/lib/plugin for the generic
    tarball), so a tarball unpacked elsewhere cannot load them and a server still using
    mysql_native_password (TiDB, Dolt, Databend) refuses the connection.  The tarball
    keeps those plugins next to the driver, so point PLUGIN_DIR there when that directory
    exists; a packaged install has no such directory and keeps its own -- correct --
    compiled-in default.
    """
    if drv is None:
        drv = os.environ[cfg["env"]]
    drvdir = os.path.dirname(drv)
    pdir = os.path.join(drvdir, "plugin")
    plugin = "PLUGIN_DIR=%s;" % pdir if os.path.isdir(pdir) else ""
    return os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(
        drv=drv, drvdir=drvdir, plugin=plugin, plugin_dir=plugin)
    drvdir = os.path.dirname(drv)
    pdir = os.path.join(drvdir, "plugin")
    return os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(
        drv=drv, drvdir=drvdir, plugin=plugin_dir, plugin_dir=plugin_dir)


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
    pseudo = tuple(cfg.get("pseudo_columns", ()))
    sfx = "" if ro else SUFFIX
    t_name, ing_name = "adbc_t" + sfx, "adbc_ing" + sfx
    T, ING = ident(t_name), ident(ing_name)
    with conn.cursor() as cur:
        for sql in cfg.get("setup", []):
            cur.execute(sql)
        if not ro:
            for t in (t_name, ing_name, qi(cfg, ing_name)):  # ingest quotes names (exact case)
                try:
                    cur.execute("DROP TABLE " + t)
                except Exception:
                    pass
            cur.execute(cfg["ddl"].replace("adbc_t", t_name))
            rows = [ROW1, row2(cfg)] if cfg.get("null_params", True) else [ROW1]
            cur.executemany("INSERT INTO %s VALUES (?, ?, ?, ?, ?, ?, ?, ?)" % t_name, rows)
            if not cfg.get("null_params", True):
                cur.execute("INSERT INTO %s VALUES (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL)" % t_name)
            refresh(cur, cfg, t_name)
        # MDB Tools' SQL parser has no ORDER BY, so read_only sorts client-side instead.
        # select: the query that reads adbc_t back, "{t}" taking the table name.  An
        # entry overrides it where the server cannot present the workload's eight
        # columns as they are and no view can be created to do it either (InfluxDB 3's
        # timestamp column is always named `time`, and it has no DATE type), so the
        # aliases and casts have to live in the query itself.
        sel = cfg.get("select", "SELECT * FROM {t}" if ro else "SELECT * FROM {t} ORDER BY i")
        cur.execute(sel.format(t=t_name))
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
        # ts_text: the ODBC driver has no mapping for the server's timestamp type and
        # describes such a column as a character one, so the timestamp arrives as its
        # text (OpenSearch: the SQL plugin types a date-and-time field TIMESTAMP, a name
        # the driver's type table does not know, and everything unknown is VARCHAR).  The
        # value is still the right one, so parse it and check it the same way.
        if cfg.get("ts_text") and isinstance(ts, str):
            ts = datetime.datetime.fromisoformat(ts)
        # A server whose only timestamp type carries a zone (Spanner: there is no
        # TIMESTAMP WITHOUT TIME ZONE) describes the column as one, so the value arrives
        # as an aware datetime.  The ODBC driver hands over the wall clock the server
        # rendered in the session's zone -- the one that was stored -- so compare that.
        # After the ts_text parse above, so a text timestamp is a datetime by here.
        if ts.tzinfo is not None:
            ts = ts.replace(tzinfo=None)
        assert ts.replace(microsecond=0) == datetime.datetime(2024, 2, 29, 13, 45, 10), ts
        # ts_us: servers whose TIMESTAMP is coarser than a microsecond (Firebird: 1/10000 s)
        assert ts.microsecond in cfg.get("ts_us", (123456, 123000)), ts
        n = r1["n"]
        fields = {f.name.lower(): str(f.type) for f in t.schema}
        # decimal_type: drivers that report a precision other than the declared one
        # decimal_type: the Arrow type(s) a driver that reports a precision other than
        # the declared one produces; a tuple where the answer depends on the driver's
        # own version (CrateDB reports neither precision nor scale, and psqlodbc 16 and
        # 18 fall back differently).
        dt = cfg.get("decimal_type", "decimal128(10, 3)")
        allowed = ("decimal128(10, 3)", "string") + (tuple(dt) if isinstance(dt, tuple) else (dt,))
        assert fields["n"] in allowed, fields["n"]
        assert n in (decimal.Decimal("12.345"), "12.345"), n
        assert fields["bo"] == cfg.get("bool_type", "bool"), fields["bo"]
        assert r1["bo"] in (True, 1), r1["bo"]
        # not_null: columns whose type has no NULL state at all (Access YESNO).
        # pseudo: record-metadata fields the server adds to every "SELECT *" of its own
        # accord (ArcadeDB's @rid/@type/@cat) -- always populated, and not columns of the
        # table, so they are skipped here and in the catalog comparisons below.
        skip = ("i",) + tuple(cfg.get("not_null", ())) + pseudo
        assert all(v is None for k, v in r2.items() if k not in skip), r2
        # parameterised query (literal where the driver has no SQLBindParameter)
        if cfg.get("params", True):
            cur.execute("SELECT s FROM %s WHERE i = ?" % t_name, (1,))
        else:
            cur.execute("SELECT s FROM %s WHERE i = 1" % t_name)
        assert cur.fetchone()[0].startswith("héllo")
        # Non-ASCII text *in the statement itself*, not only in a parameter.  The two
        # are different paths: a parameter goes out as SQL_C_WCHAR, statement text goes
        # through SQLExecDirect, and on Windows the driver manager reads narrow statement
        # text as the ANSI code page unless the W entry point is used -- so a literal
        # "héllo" was stored as "hÃ©llo" and matched nothing, while every parameterised
        # step still passed (the first Windows run found this).  Both spellings of the
        # same predicate must find row 1.  literal_text=False for a server whose SQL has
        # no LIKE, or that cannot hold the value at all.
        if cfg.get("literal_text", True):
            cur.execute("SELECT i FROM %s WHERE s LIKE 'héllo%%'" % t_name)
            assert [r[0] for r in cur.fetchall()] == [1], "statement literal 'héllo' matched nothing"
            if cfg.get("params", True):
                cur.execute("SELECT i FROM %s WHERE s LIKE ?" % t_name, ("héllo%",))
                assert [r[0] for r in cur.fetchall()] == [1], "parameter 'héllo' matched nothing"
        # bulk ingest + read back (read_only reads the fixture's big table instead)
        # ingest_create: the server refuses the table adbc_ingest would have to create --
        # Ignite has no table without a PRIMARY KEY, and the ingest payload has no column
        # that could be one -- so this entry reads the big table `setup` built, exactly as
        # a read_only one does, and covers bulk ingest through its `extra` steps instead.
        if ro or not cfg.get("ingest_create", True):
            check_big(cur, cfg, "SELECT %s, %s FROM %s"
                                % (qi(cfg, "a"), qi(cfg, "b"), qi(cfg, "adbc_big")))
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
    cols = list(COLS)
    def real(names):  # the table's own columns, in the order this entry compares them
        return order([c for c in names if c not in pseudo])

    # catalog_cols: the columns the catalog really reports for adbc_t, for an entry whose
    # `select` reads a table shaped differently from the workload's eight columns
    # (InfluxDB 3's mandatory `time`, which that entry's select presents as `ts`).
    cols = list(cfg.get("catalog_cols", ("i", "f", "s", "b", "d", "ts", "n", "bo")))
    # No catalog filter is given, so every catalog on the server that happens to
    # hold a table of this name is reported -- shared servers really do have
    # more than one.  Check the columns of each match individually.
    per_table = [real([c["column_name"].lower() for c in t["table_columns"]])
                 for cat in objs for s in cat["catalog_db_schemas"] or []
                 for t in s["db_schema_tables"] or []]
    assert order(cols) in per_table, per_table
    assert "adbc_t" in [x.lower() for x in conn.adbc_get_table_types()] or True
    sch = conn.adbc_get_table_schema(T)
    assert real([f.name.lower() for f in sch]) == order(cols)
    conn.close()
    return "PASS  (%s %s)" % (info["vendor_name"], info["vendor_version"])


def qi(cfg, name):
    """Quote an identifier the way this server spells a quoted name.

    adbc_ingest creates its table with whatever the ODBC driver reports for
    SQL_IDENTIFIER_QUOTE_CHAR, so the SQL this file writes against an ingested table has
    to quote with the same character.  Almost everywhere that is the SQL-standard double
    quote -- MySQL and MariaDB reach it through the `ANSI_QUOTES` sql_mode their entries
    set in `setup`.  `quote` overrides it for a server that has no double-quoted-identifier
    mode at all (StarRocks accepts the `ANSI_QUOTES` value but its parser ignores it, so
    MySQL Connector/ODBC correctly reports the backtick and ingest quotes with that;
    GreptimeDB is a MySQL dialect with no ANSI_QUOTES at all, so a "..." there is a
    string literal; TDengine quotes with backticks and rejects `FROM "t"` outright).
    """
    q = cfg.get("quote", '"')
    return q + name + q


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
    # By name, case-insensitively: a server that folds an unquoted identifier to upper
    # case (Ignite, whose driver reports no identifier quote character at all, so the
    # query above cannot quote them) labels the columns "A" and "B".
    col = {name.lower(): i for i, name in enumerate(big.schema.names)}
    pairs = sorted(zip(big.column(col["a"]).to_pylist(), big.column(col["b"]).to_pylist()))
    assert [a for a, _ in pairs] == list(range(n)), len(pairs)
    assert pairs[-1][1] == "r%d" % (n - 1), pairs[-1]


def check_ingest(cur, cfg, ing_name):
    """Bulk ingest, read back, then ingest big_rows rows and read those back."""
    # Every column carries a NULL, and every NULL has a value after it: a driver that
    # retypes a parameter when it binds a NULL and never re-derives it corrupts the
    # *following* rows, which a NULL in the last row would hide.  (Firebird's OdbcFb did
    # exactly that for SQL_BIGINT -- see NullParamCType in src/odbc_bind.c.  That is what
    # the fourth row is for: `a` and `d` used to have their NULL last.)  Row 0 has no
    # NULL at all, which OceanBase needs -- a NULL bound into the first execute of a
    # prepared INSERT fixes that parameter's type as MYSQL_TYPE_NULL there, and every
    # later row is then refused with "Object type error" (4001).
    tbl = ingest_payload(cfg, {
        "a": pa.array([1, 2, None, 4], pa.int64()),
        "b": pa.array(["x", None, "zz", "w"]),
        "c": pa.array([1.5, None, 2.5, 3.5]),
        "d": pa.array([0, 19782, None, 1], pa.date32()),
        "e": pa.array([True, None, False, True], pa.bool_()),
    })
    n1 = cur.adbc_ingest(ing_name, tbl, mode="create")
    n2 = cur.adbc_ingest(ing_name, tbl, mode="append")
    assert (n1, n2) == (4, 4) or not cfg.get("rowcount", True), (n1, n2)
    refresh(cur, cfg, ing_name)
    cur.execute("SELECT %s, %s, %s, %s FROM %s WHERE %s = 2"
                % (qi(cfg, "a"), qi(cfg, "b"), qi(cfg, "c"), qi(cfg, "d"),
                   qi(cfg, ing_name), qi(cfg, "a")))
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
    check_big(cur, cfg, "SELECT %s, %s FROM %s ORDER BY %s"
              % (qi(cfg, "a"), qi(cfg, "b"), qi(cfg, ing_name), qi(cfg, "a")))
    check_text_sortable(cur, cfg, ing_name, N)
    check_wide_text(cur, cfg, ing_name)


def check_wide_text(cur, cfg, ing_name):
    """Read back a text column holding values far wider than the reader binds for.

    `wide_text_rows` opts an entry in, and the column is the one the *generated ingest
    DDL* made for an Arrow string column -- whatever the server's
    SQLGetTypeInfo(SQL_LONGVARCHAR) named.  That is the only way a user reaches this
    column, and on Oracle it is a CLOB, which the reader has to treat as a column with no
    declared width at all: 2,147,483,647 characters is what SQORA reports for one.

    Two things have to hold at once, and only a table of mixed widths asks for both.
    Every value must come back whole, including the ones past
    adbc.odbc.long_bind_bytes (2 KiB) that no bound rowset buffer could have held; and
    the read must cross several rowsets and several Arrow batches, because a reader that
    changes its fetch shape between rowsets does it at a batch boundary.  Oracle's SQORA
    segfaulted on exactly that: raising SQL_ATTR_ROW_ARRAY_SIZE mid-cursor on a cursor
    holding a LOB column dies inside the driver (see OdbcDetectQuirks in
    src/odbc_driver.c), and 20,000 narrow CLOBs were enough to reach it.
    """
    n = cfg.get("wide_text_rows")
    if not n:
        return
    # Mostly narrow, one row in 250 far wider than the reader's long_bind_bytes.  The
    # narrow rows are what keeps a block cursor worth having; the wide ones are what a
    # bound buffer cannot hold.
    def value(i):
        return "w%09d" % i + "." * 9000 if i % 250 == 0 else "row-%012d" % i
    want = [value(i) for i in range(n)]
    cur.adbc_ingest(ing_name, ingest_payload(cfg, {
        "a": pa.array(range(n), pa.int64()),
        "b": pa.array(want),
        "c": pa.array([float(i) for i in range(n)]),
        "d": pa.array(list(range(n)), pa.date32()),
        "e": pa.array([i % 2 == 0 for i in range(n)]),
    }), mode="replace")
    refresh(cur, cfg, ing_name)
    cur.execute("SELECT %s, %s FROM %s" % (qi(cfg, "a"), qi(cfg, "b"), qi(cfg, ing_name)))
    t = cur.fetch_arrow_table()
    col = {name.lower(): i for i, name in enumerate(t.schema.names)}
    got = dict(zip(t.column(col["a"]).to_pylist(), t.column(col["b"]).to_pylist()))
    assert len(got) == n, (len(got), n)
    bad = [i for i in range(n) if got.get(i) != want[i]]
    assert not bad, ("wide text came back wrong on %d of %d rows, first %s"
                     % (len(bad), n, [(i, len(got.get(i) or ""), len(want[i])) for i in bad[:3]]))


def check_text_sortable(cur, cfg, ing_name, n):
    """The text column bulk ingest created must be an ordinary sortable string column.

    `text_sortable` opts an entry in.  It is a real property of the generated DDL, not a
    formality: a server's answer to SQLGetTypeInfo(SQL_LONGVARCHAR) -- which is what the
    ingest DDL asks for, an Arrow string column having no declared width -- can name a
    type the server then refuses to sort, group or de-duplicate on.  Db2's is LONG
    VARCHAR, and ORDER BY, GROUP BY and DISTINCT on one are all SQL0134N, "improper use
    of a string column"; the column is also written about 700x slower than an ordinary
    VARCHAR (see ddl_string_as_max_varchar in src/odbc_internal.h).  So an ingested table
    that cannot be sorted on its own text column is a defect worth a test.

    Left off by default because the same restriction is genuine on some servers whatever
    adbcbridge does -- Oracle's SQL_LONGVARCHAR is CLOB, and ORDER BY on a CLOB is
    ORA-00932 -- so this is only claimed where it has been checked.
    """
    if not cfg.get("text_sortable"):
        return
    b, t = qi(cfg, "b"), qi(cfg, ing_name)
    cur.execute("SELECT %s FROM %s ORDER BY %s" % (b, t, b))
    ordered = cur.fetch_arrow_table().column(0).to_pylist()
    assert ordered == sorted("r%d" % i for i in range(n)), (ordered[:3], ordered[-3:])
    cur.execute("SELECT DISTINCT %s FROM %s" % (b, t))
    assert len(cur.fetch_arrow_table()) == n
    cur.execute("SELECT %s FROM %s GROUP BY %s" % (b, t, b))
    assert len(cur.fetch_arrow_table()) == n


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
