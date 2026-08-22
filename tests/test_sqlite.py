"""Smoke tests for adbc-odbc using the SQLite ODBC driver (no server needed)."""
import os, sys, pathlib, tempfile
import adbc_driver_manager.dbapi as dbapi
import adbc_driver_manager

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

def connect(path):
    uri = f"Driver={SQLITE_ODBC};Database={path};"
    return dbapi.connect(driver=DRIVER, db_kwargs={"uri": uri})

def main():
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "t.db")
    with connect(db) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            tbl = cur.fetch_arrow_table()
            print("SELECT 1 ->", tbl.schema, tbl.to_pydict())
            assert tbl.to_pydict()["one"] == [1]

            cur.execute("CREATE TABLE t (i INTEGER, f REAL, s TEXT, b BLOB, d DATE, ts TIMESTAMP, n DECIMAL(10,3))")
            cur.execute("INSERT INTO t VALUES (1, 1.5, 'héllo', x'0102', '2024-02-29', '2024-02-29 13:45:10.123', '12.345')")
            cur.execute("INSERT INTO t VALUES (NULL, NULL, NULL, NULL, NULL, NULL, NULL)")
            cur.execute("INSERT INTO t VALUES (-7, -0.25, '', x'', '1970-01-01', '1970-01-01 00:00:00', '-0.5')")
            cur.execute("SELECT * FROM t ORDER BY rowid")
            tbl = cur.fetch_arrow_table()
            print(tbl.schema)
            print(tbl.to_pydict())
            d = tbl.to_pydict()
            assert d["i"] == [1, None, -7]
            assert d["s"] == ["héllo", None, ""]
            assert d["b"] == [b"\x01\x02", None, b""]

            # Big result: more rows than one batch
            cur.execute("CREATE TABLE big AS WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 5000) SELECT x, 'row'||x AS name FROM c")
            cur.execute("SELECT x, name FROM big ORDER BY x")
            tbl = cur.fetch_arrow_table()
            assert tbl.num_rows == 5000, tbl.num_rows
            assert tbl.column("x").to_pylist()[-1] == 5000
            print("big table ok, batches:", len(tbl.to_batches()))

            # rows_affected for DML
            n = cur.execute("DELETE FROM big WHERE x <= 10")
            print("rowcount:", cur.rowcount)
            assert cur.rowcount == 10

        print("table types:", conn.adbc_get_table_types())
        print("info:", conn.adbc_get_info())
        sch = conn.adbc_get_table_schema("t")
        print("table schema:", sch)
        # Error path
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT * FROM nope")
            raise AssertionError("expected error")
        except Exception as e:
            print("error ok:", type(e).__name__, str(e)[:120])
    print("ALL OK")

if __name__ == "__main__":
    main()
