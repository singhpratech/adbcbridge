# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

"""Smoke tests for adbcbridge using the SQLite ODBC driver (no server needed)."""
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


def test_types():
    """Type-coverage checks for everything the SQLite ODBC driver can produce.

    The SQL types this driver also handles but SQLite never reports
    (SQL_SS_TIME2, SQL_SS_TIMESTAMPOFFSET, SQL_GUID, SQL_INTERVAL_*,
    SQL_DECIMAL, SQL_C_WCHAR) are covered by the C unit tests in tests/c/.
    """
    import datetime
    import math

    import pyarrow as pa

    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "types.db")
    with connect(db) as conn:
        with conn.cursor() as cur:
            # --- TIME columns -> time32[s] --------------------------------
            cur.execute("CREATE TABLE tm (t TIME)")
            cur.execute("INSERT INTO tm VALUES ('12:34:56')")
            cur.execute("INSERT INTO tm VALUES ('00:00:00')")
            cur.execute("INSERT INTO tm VALUES ('23:59:59')")
            cur.execute("INSERT INTO tm VALUES (NULL)")
            cur.execute("SELECT t FROM tm ORDER BY rowid")
            tbl = cur.fetch_arrow_table()
            print("time schema:", tbl.schema)
            assert tbl.schema.field("t").type == pa.time32("s"), tbl.schema
            assert tbl.column("t").to_pylist() == [
                datetime.time(12, 34, 56),
                datetime.time(0, 0, 0),
                datetime.time(23, 59, 59),
                None,
            ], tbl.column("t").to_pylist()

            # --- GUID-like text round-trips as a string -------------------
            guid = "6F9619FF-8B86-D011-B42D-00C04FC964FF"
            cur.execute("CREATE TABLE g (id VARCHAR(36), iv VARCHAR(40))")
            cur.execute(f"INSERT INTO g VALUES ('{guid}', '-1 12:34:56.789')")
            cur.execute("SELECT id, iv FROM g")
            tbl = cur.fetch_arrow_table()
            assert tbl.schema.field("id").type == pa.string(), tbl.schema
            assert tbl.column("id").to_pylist() == [guid]
            # ODBC interval strings stay text: Arrow cannot hold every qualifier.
            assert tbl.column("iv").to_pylist() == ["-1 12:34:56.789"]

            # --- wide / unconstrained DECIMAL -> string, losslessly -------
            cur.execute("CREATE TABLE dec (a DECIMAL(50,2), b DECIMAL, c DECIMAL(10,3))")
            wide = "12345678901234567890123456789012345678901234.99"
            cur.execute(f"INSERT INTO dec VALUES ('{wide}', '1.5', '12.345')")
            cur.execute("SELECT a, b, c FROM dec")
            tbl = cur.fetch_arrow_table()
            print("decimal schema:", tbl.schema)
            for name in ("a", "b", "c"):
                assert tbl.schema.field(name).type == pa.string(), tbl.schema
            # SQLite's NUMERIC affinity coerces an out-of-int64-range literal to
            # a float, so compare the wide value numerically, not textually.
            assert math.isclose(float(tbl.column("a").to_pylist()[0]), float(wide),
                                rel_tol=1e-12), tbl.column("a").to_pylist()
            assert tbl.column("b").to_pylist() == ["1.5"]
            assert tbl.column("c").to_pylist() == ["12.345"]

            # --- non-BMP characters survive the text path -----------------
            # SQLite ODBC reports SQL_VARCHAR (not SQL_WVARCHAR), so this is
            # the SQL_C_CHAR path; tests/c/test_utf16.c covers SQL_C_WCHAR.
            emoji = "a😀b🇦🇸 ünïcödé 𝄞"
            cur.execute("CREATE TABLE e (s TEXT)")
            cur.executemany("INSERT INTO e VALUES (?)", [(emoji,), ("",), (None,)])
            cur.execute("SELECT s FROM e ORDER BY rowid")
            got = cur.fetch_arrow_table().column("s").to_pylist()
            print("emoji round-trip:", got)
            assert got == [emoji, "", None], got
            # ...also when the value is long enough to take the SQLGetData path
            long_emoji = "😀" * 40000
            cur.executemany("INSERT INTO e VALUES (?)", [(long_emoji,)])
            cur.execute("SELECT s FROM e WHERE length(s) = 40000")
            assert cur.fetch_arrow_table().column("s").to_pylist() == [long_emoji]

            # --- float specials pass through unchanged --------------------
            cur.execute("SELECT 9e999 AS pinf, -9e999 AS ninf, 1.5 AS ok")
            tbl = cur.fetch_arrow_table()
            print("float schema:", tbl.schema)
            d = tbl.to_pydict()
            assert math.isinf(d["pinf"][0]) and d["pinf"][0] > 0, d
            assert math.isinf(d["ninf"][0]) and d["ninf"][0] < 0, d
            assert d["ok"] == [1.5], d
            # An infinity must not be reported as a null.
            assert tbl.column("pinf").null_count == 0

            # --- timestamps still work alongside the new time handling ----
            cur.execute("CREATE TABLE ts (v TIMESTAMP)")
            cur.execute("INSERT INTO ts VALUES ('2024-02-29 13:45:10.123')")
            cur.execute("SELECT v FROM ts")
            tbl = cur.fetch_arrow_table()
            assert tbl.schema.field("v").type == pa.timestamp("us"), tbl.schema
            assert tbl.column("v").to_pylist() == [
                datetime.datetime(2024, 2, 29, 13, 45, 10, 123000)
            ]
    print("TYPES OK")


if __name__ == "__main__":
    test_types()
