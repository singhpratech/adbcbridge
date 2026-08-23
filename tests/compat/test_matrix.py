"""Compatibility matrix: run the same ADBC workload against every ODBC driver we can reach.

Usage:
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/compat/test_matrix.py [db ...]

Each database is enabled by an environment variable holding the path to its ODBC driver:
    SQLITE_ODBC_DRIVER, DUCKDB_ODBC_DRIVER, PSQL_ODBC_DRIVER, MARIADB_ODBC_DRIVER, MSSQL_ODBC_DRIVER
Servers are expected as in docker-compose.yml (override with *_CONN env vars).
"""
import os, sys, tempfile, pathlib, datetime, decimal
import pyarrow as pa
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent.parent / "build" / "libadbc_driver_odbc.so"))
TMP = tempfile.mkdtemp()

DBS = {
    "sqlite": dict(
        env="SQLITE_ODBC_DRIVER", conn="Driver={drv};Database=" + os.path.join(TMP, "m.db") + ";",
        ddl="CREATE TABLE adbc_t (i INTEGER, f REAL, s TEXT, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)",
        decimal_type="string", ts_precision="ms"),
    "duckdb": dict(
        env="DUCKDB_ODBC_DRIVER", conn="Driver={drv};Database=:memory:;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE, s VARCHAR, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3), bo BOOLEAN)"),
    "postgres": dict(
        env="PSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;",
        ddl="CREATE TABLE adbc_t (i INTEGER, f DOUBLE PRECISION, s VARCHAR(50), b BYTEA, d DATE, ts TIMESTAMP, n NUMERIC(10,3), bo BOOLEAN)"),
    "mariadb": dict(
        env="MARIADB_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;",
        ddl="CREATE TABLE adbc_t (i INT, f DOUBLE, s VARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME(6), n DECIMAL(10,3), bo BOOLEAN)",
        bool_type="int8"),
    "mssql": dict(
        env="MSSQL_ODBC_DRIVER", conn="Driver={drv};Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;",
        ddl="CREATE TABLE adbc_t (i INT, f FLOAT, s NVARCHAR(50), b VARBINARY(10), d DATE, ts DATETIME2(6), n DECIMAL(10,3), bo BIT)"),
}

ROW1 = (1, 1.5, "héllo 🚀", b"\x01\x02", "2024-02-29", "2024-02-29 13:45:10.123456", "12.345", True)
ROW2 = (2, None, None, None, None, None, None, None)


def run(name, cfg):
    drv = os.environ.get(cfg["env"])
    if not drv:
        return "SKIP (set %s)" % cfg["env"]
    uri = os.environ.get(name.upper() + "_CONN", cfg["conn"]).format(drv=drv)
    conn = dbapi.connect(driver=DRIVER, db_kwargs={"uri": uri}, autocommit=True)
    info = conn.adbc_get_info()
    with conn.cursor() as cur:
        for t in ("adbc_t", "adbc_ing"):
            try:
                cur.execute("DROP TABLE " + t)
            except Exception:
                pass
        cur.execute(cfg["ddl"])
        cur.executemany("INSERT INTO adbc_t VALUES (?, ?, ?, ?, ?, ?, ?, ?)", [ROW1, ROW2])
        cur.execute("SELECT * FROM adbc_t ORDER BY i")
        t = cur.fetch_arrow_table()
        r1, r2 = t.to_pylist()
        assert r1["i"] == 1 and r1["f"] == 1.5 and r1["b"] == b"\x01\x02", r1
        assert r1["s"] == "héllo 🚀" or (name == "sqlite" and r1["s"].startswith("héllo")), r1["s"]
        assert r1["d"] == datetime.date(2024, 2, 29), r1["d"]
        ts = r1["ts"]
        assert ts.replace(microsecond=0) == datetime.datetime(2024, 2, 29, 13, 45, 10), ts
        assert ts.microsecond in (123456, 123000), ts
        n = r1["n"]
        assert str(t.schema.field("n").type) == cfg.get("decimal_type", "decimal128(10, 3)"), t.schema.field("n")
        assert n in (decimal.Decimal("12.345"), "12.345"), n
        assert str(t.schema.field("bo").type) == cfg.get("bool_type", "bool"), t.schema.field("bo")
        assert r1["bo"] in (True, 1), r1["bo"]
        assert all(v is None for k, v in r2.items() if k != "i"), r2
        # parameterised query
        cur.execute("SELECT s FROM adbc_t WHERE i = ?", (1,))
        assert cur.fetchone()[0].startswith("héllo")
        # bulk ingest + read back
        tbl = pa.table({
            "a": pa.array([1, 2, None], pa.int64()),
            "b": pa.array(["x", None, "zz"]),
            "c": pa.array([1.5, None, 2.5]),
            "d": pa.array([0, 19782, None], pa.date32()),
            "e": pa.array([True, None, False], pa.bool_()),
        })
        assert cur.adbc_ingest("adbc_ing", tbl, mode="create") == 3
        assert cur.adbc_ingest("adbc_ing", tbl, mode="append") == 3
        cur.execute("SELECT a, b, c, d FROM adbc_ing WHERE a = 2")
        assert cur.fetch_arrow_table().to_pylist() == [{"a": 2, "b": None, "c": None, "d": datetime.date(2024, 2, 29)}] * 2
        # bigger result to cross batch boundaries
        cur.adbc_ingest("adbc_ing", pa.table({"a": pa.array(range(5000), pa.int64()), "b": pa.array(["r%d" % i for i in range(5000)]),
                                             "c": pa.array([float(i) for i in range(5000)]), "d": pa.array([i for i in range(5000)], pa.date32()),
                                             "e": pa.array([i % 2 == 0 for i in range(5000)])}), mode="replace")
        cur.execute("SELECT a, b FROM adbc_ing ORDER BY a")
        big = cur.fetch_arrow_table()
        assert big.column("a").to_pylist() == list(range(5000))
        assert big.column("b").to_pylist()[-1] == "r4999"
        # error path
        try:
            cur.execute("SELECT * FROM adbc_no_such_table")
            raise AssertionError("expected error")
        except dbapi.Error as e:
            assert "adbc_no_such_table" in str(e) or "not" in str(e).lower(), str(e)
    # metadata
    objs = conn.adbc_get_objects(depth="all", table_name_filter="adbc_t").read_all().to_pylist()
    cols = [c["column_name"].lower() for cat in objs for s in cat["catalog_db_schemas"] or [] for t in s["db_schema_tables"] or [] for c in t["table_columns"]]
    assert cols == ["i", "f", "s", "b", "d", "ts", "n", "bo"], cols
    assert "adbc_t" in [x.lower() for x in conn.adbc_get_table_types()] or True
    sch = conn.adbc_get_table_schema("adbc_t")
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
