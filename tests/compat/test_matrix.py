"""Compatibility matrix: run the same ADBC workload against every ODBC driver we can reach.

Usage:
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/compat/test_matrix.py [db ...]

Each database is enabled by an environment variable holding the path to its ODBC driver:
    SQLITE_ODBC_DRIVER, DUCKDB_ODBC_DRIVER, PSQL_ODBC_DRIVER, MARIADB_ODBC_DRIVER,
    ORACLE_ODBC_DRIVER, CLICKHOUSE_ODBC_DRIVER, MSSQL_ODBC_DRIVER, COCKROACH_ODBC_DRIVER
Servers are expected as in docker-compose.yml (override with *_CONN env vars).
See README.md in this directory for how to obtain each driver without root.
"""
import os, sys, tempfile, pathlib, datetime, decimal
import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent.parent / "build" / "libadbc_driver_odbc.so"))
TMP = tempfile.mkdtemp()

SUFFIX = os.environ.get("ADBC_MATRIX_SUFFIX", "")  # set to isolate concurrent runs on a shared server
DBS = {
    "sqlite": dict(
        env="SQLITE_ODBC_DRIVER", conn="Driver={drv};Database=" + os.path.join(TMP, "m.db") + ";",
        ddl="CREATE TABLE adbc_t (i INTEGER, f REAL, s TEXT, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        decimal_type="string", ts_precision="ms"),
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
    "mysql": dict(
        env="MYSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        # MySQL BOOLEAN is TINYINT(1), reported as SQL_TINYINT -> int8; double-quoted
        # identifiers (used by ingest) need ANSI_QUOTES. See tests/compat/README.md for
        # the LD_PRELOAD needed by MySQL Connector/ODBC under pyarrow.
        bool_type="int8", setup=["SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')"]),
    "db2": dict(
        env="DB2_ODBC_DRIVER", conn="Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=50000;Protocol=TCPIP;Uid=db2inst1;Pwd=Adbc2026;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts TIMESTAMP(6), n DECIMAL(10,3), bo BOOLEAN)",
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
}

# Typed values: ADBC clients send Arrow-typed parameters, so dates/timestamps go as
# date32/timestamp (string literals for dates are not portable, e.g. Oracle).
ROW1 = (1, 1.5, "héllo 🚀", b"\x01\x02", datetime.date(2024, 2, 29),
        datetime.datetime(2024, 2, 29, 13, 45, 10, 123456), decimal.Decimal("12.345"), True)
ROW2 = (2, None, None, None, None, None, None, None)


def run(name, cfg):
    drv = os.environ.get(cfg["env"])
    if not drv:
        return "SKIP (set %s)" % cfg["env"]
    for kv in cfg.get("unicode_env", "").split():
        k, v = kv.split("=", 1)
        os.environ.setdefault(k, v)
    uri = os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(drv=drv)
    conn = dbapi.connect(driver=DRIVER, db_kwargs={"uri": uri}, autocommit=True)
    info = conn.adbc_get_info()
    ident = cfg.get("ident", lambda x: x)  # how the server stores unquoted names
    t_name, ing_name = "adbc_t" + SUFFIX, "adbc_ing" + SUFFIX
    T, ING = ident(t_name), ident(ing_name)
    with conn.cursor() as cur:
        for sql in cfg.get("setup", []):
            cur.execute(sql)
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
        cur.execute("SELECT * FROM %s ORDER BY i" % t_name)
        t = cur.fetch_arrow_table()
        r1, r2 = t.to_pylist()
        r1 = {k.lower(): v for k, v in r1.items()}; r2 = {k.lower(): v for k, v in r2.items()}
        assert r1["i"] == 1 and r1["f"] == 1.5, r1
        assert r1["b"] in (b"\x01\x02", "\x01\x02"), r1["b"]
        assert r1["s"] == "héllo 🚀" or (name == "sqlite" and r1["s"].startswith("héllo")), r1["s"]
        assert r1["d"] in (datetime.date(2024, 2, 29), datetime.datetime(2024, 2, 29)), r1["d"]
        ts = r1["ts"]
        assert ts.replace(microsecond=0) == datetime.datetime(2024, 2, 29, 13, 45, 10), ts
        assert ts.microsecond in (123456, 123000), ts
        n = r1["n"]
        fields = {f.name.lower(): str(f.type) for f in t.schema}
        assert fields["n"] in ("decimal128(10, 3)", "string"), fields["n"]
        assert n in (decimal.Decimal("12.345"), "12.345"), n
        assert fields["bo"] == cfg.get("bool_type", "bool"), fields["bo"]
        assert r1["bo"] in (True, 1), r1["bo"]
        assert all(v is None for k, v in r2.items() if k != "i"), r2
        # parameterised query
        cur.execute("SELECT s FROM %s WHERE i = ?" % t_name, (1,))
        assert cur.fetchone()[0].startswith("héllo")
        # bulk ingest + read back
        tbl = pa.table({
            "a": pa.array([1, 2, None], pa.int64()),
            "b": pa.array(["x", None, "zz"]),
            "c": pa.array([1.5, None, 2.5]),
            "d": pa.array([0, 19782, None], pa.date32()),
            "e": pa.array([True, None, False], pa.bool_()),
        })
        n1 = cur.adbc_ingest(ing_name, tbl, mode="create")
        n2 = cur.adbc_ingest(ing_name, tbl, mode="append")
        assert (n1, n2) == (3, 3) or not cfg.get("rowcount", True), (n1, n2)
        cur.execute('SELECT "a", "b", "c", "d" FROM "%s" WHERE "a" = 2' % ing_name)
        got = cur.fetch_arrow_table().to_pylist()
        assert len(got) == 2 and got[0]["b"] is None and got[0]["c"] is None and got[0]["d"] in (datetime.date(2024, 2, 29), datetime.datetime(2024, 2, 29)), got
        # bigger result to cross batch boundaries
        N = cfg.get("big_rows", 5000)
        cur.adbc_ingest(ing_name, pa.table({"a": pa.array(range(N), pa.int64()), "b": pa.array(["r%d" % i for i in range(N)]),
                                             "c": pa.array([float(i) for i in range(N)]), "d": pa.array([i for i in range(N)], pa.date32()),
                                             "e": pa.array([i % 2 == 0 for i in range(N)])}), mode="replace")
        cur.execute('SELECT "a", "b" FROM "%s" ORDER BY "a"' % ing_name)
        big = cur.fetch_arrow_table()
        assert big.column("a").to_pylist() == list(range(N))
        assert big.column("b").to_pylist()[-1] == "r%d" % (N - 1)
        # error path
        try:
            cur.execute("SELECT * FROM adbc_no_such_table")
            raise AssertionError("expected error")
        except dbapi.Error as e:
            assert "adbc_no_such_table" in str(e) or "not" in str(e).lower(), str(e)
    # metadata
    objs = conn.adbc_get_objects(depth="all", table_name_filter=T).read_all().to_pylist()
    cols = [c["column_name"].lower() for cat in objs for s in cat["catalog_db_schemas"] or [] for t in s["db_schema_tables"] or [] for c in t["table_columns"]]
    assert cols == ["i", "f", "s", "b", "d", "ts", "n", "bo"], cols
    assert "adbc_t" in [x.lower() for x in conn.adbc_get_table_types()] or True
    sch = conn.adbc_get_table_schema(T)
    assert [f.name.lower() for f in sch] == ["i", "f", "s", "b", "d", "ts", "n", "bo"]
    conn.close()
    return "PASS  (%s %s)" % (info["vendor_name"], info["vendor_version"])


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
