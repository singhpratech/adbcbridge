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


def test_phase2():
    import pyarrow as pa
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "p2.db")
    with connect(db) as conn:
        with conn.cursor() as cur:
            cur.execute("CREATE TABLE parent (id INTEGER PRIMARY KEY, name TEXT)")
            cur.execute("CREATE TABLE child (id INTEGER PRIMARY KEY, pid INTEGER REFERENCES parent(id), v REAL)")
            # parameter binding
            cur.executemany("INSERT INTO parent VALUES (?, ?)", [(1, "a"), (2, "b"), (3, None)])
            cur.execute("SELECT COUNT(*) FROM parent")
            assert cur.fetchone()[0] == 3
            cur.execute("SELECT name FROM parent WHERE id = ?", (2,))
            assert cur.fetchone()[0] == "b"
            # bulk ingest
            tbl = pa.table({
                "i": pa.array([1, 2, None], pa.int64()),
                "f": pa.array([1.5, None, 3.25], pa.float64()),
                "s": pa.array(["x", "yy", None], pa.string()),
                "b": pa.array([b"\x00\x01", None, b""], pa.binary()),
                "d": pa.array([0, 19782, None], pa.date32()),
                "ts": pa.array([0, 1709214310123456, None], pa.timestamp("us")),
                "flag": pa.array([True, False, None], pa.bool_()),
            })
            n = cur.adbc_ingest("ingested", tbl, mode="create")
            print("ingested rows:", n)
            assert n == 3
            n = cur.adbc_ingest("ingested", tbl, mode="append")
            assert n == 3
            cur.execute("SELECT * FROM ingested")
            back = cur.fetch_arrow_table()
            print(back.schema)
            print(back.to_pydict())
            assert back.num_rows == 6
            assert back.column("s").to_pylist()[:3] == ["x", "yy", None]
            assert back.column("ts").to_pylist()[1].microsecond == 123000  # sqliteodbc writes ms only
            try:
                cur.adbc_ingest("ingested", tbl, mode="create")
                raise AssertionError("expected already-exists")
            except Exception as e:
                print("create-twice ok:", type(e).__name__)
            n = cur.adbc_ingest("ingested", tbl, mode="replace")
            cur.execute("SELECT COUNT(*) FROM ingested")
            assert cur.fetchone()[0] == 3
        # GetObjects
        objs = conn.adbc_get_objects(depth="all").read_all().to_pylist()
        print("catalogs:", [c["catalog_name"] for c in objs])
        tables = {}
        for c in objs:
            for s in c["catalog_db_schemas"] or []:
                for t in s["db_schema_tables"] or []:
                    tables[t["table_name"]] = t
        print("tables:", sorted(tables))
        assert {"parent", "child", "ingested"} <= set(tables)
        child = tables["child"]
        print("child columns:", [(c["column_name"], c["xdbc_type_name"]) for c in child["table_columns"]])
        print("child constraints:", child["table_constraints"])
        assert [c["column_name"] for c in child["table_columns"]] == ["id", "pid", "v"]
        kinds = {c["constraint_type"] for c in child["table_constraints"]}
        assert "PRIMARY KEY" in kinds, kinds
        assert "FOREIGN KEY" in kinds, kinds
        t_only = conn.adbc_get_objects(depth="tables", table_name_filter="par%").read_all().to_pylist()
        names = [t["table_name"] for c in t_only for s in c["catalog_db_schemas"] for t in s["db_schema_tables"]]
        assert names == ["parent"], names
        print("filtered:", names)
    print("PHASE2 OK")


if __name__ == "__main__":
    test_phase2()
