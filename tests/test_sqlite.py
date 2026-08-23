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
    # These are tests of the ODBC path; keep native delegation (which would hand
    # SQLite over to adbc_driver_sqlite when it is installed) out of the way.
    return dbapi.connect(
        driver=DRIVER, db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"}
    )

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

def test_release_with_open_transaction_drops_locks():
    """Closing a connection mid-transaction must roll back and really disconnect.

    ODBC rejects SQLDisconnect while a transaction is open (SQLSTATE 25000); if the
    driver ignored that, sqliteodbc kept the handle -- and the read lock taken by the
    SELECT -- alive, and the next writer's commit spun in SQLite's busy handler forever.
    """
    import threading
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "lock.db")
    uri = f"Driver={SQLITE_ODBC};Database={db};"
    kw = dict(driver=DRIVER, db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"}, autocommit=False)
    conn = dbapi.connect(**kw)
    with conn.cursor() as cur:
        cur.execute("CREATE TABLE lk (i INTEGER)")
    conn.commit()
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM lk")
        assert cur.fetchone()[0] == 0
    conn.close()  # transaction (the SELECT's read lock) still open here
    conn2 = dbapi.connect(**kw)
    done = threading.Event()
    err = []

    def writer():
        try:
            with conn2.cursor() as cur:
                cur.execute("DROP TABLE lk")
            conn2.commit()
        except Exception as e:  # noqa: BLE001
            err.append(e)
        done.set()

    threading.Thread(target=writer, daemon=True).start()
    assert done.wait(10), "commit after a closed connection hung on the leaked lock"
    assert not err, err
    conn2.close()
    print("RELEASE WITH OPEN TXN OK")


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


def test_bind_types():
    """Arrow parameter/ingest types added for validation findings D10-D13.

    Every case runs twice: once as a two-row batch (which takes the column-wise
    parameter-array path in ExecuteBatchArray) and once row-at-a-time (which
    takes SlotFromArrow), so both binding paths are covered.

    SQLiteODBC reports decimal_digits 0 for every TIME and TIMESTAMP column, so
    what comes back here is always time32[s] / timestamp[us]; the units a
    scale-reporting driver produces are covered by tests/c/test_types.c.
    """
    import datetime

    import pyarrow as pa

    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "bind.db")

    def bind(cur, sql, table, rows_per_batch):
        """Bind `table` through `sql`, rows_per_batch rows per batch."""
        for start in range(0, table.num_rows, rows_per_batch):
            batch = table.slice(start, rows_per_batch).combine_chunks().to_batches()[0]
            cur.adbc_statement.set_sql_query(sql)
            cur.adbc_statement.bind(batch)
            cur.adbc_statement.execute_update()

    with connect(db) as conn:
        with conn.cursor() as cur:
            # --- time32/time64 parameters and ingest (D10) ----------------
            # Whole seconds go across as a TIME_STRUCT, sub-second units as
            # "HH:MM:SS.ffffff" text; SQLite stores the text verbatim, so the
            # string the driver produced is observable.
            cases = [
                (pa.time32("s"), [45296, None], "12:34:56"),
                (pa.time32("ms"), [45296789, None], "12:34:56.789"),
                (pa.time64("us"), [45296789012, None], "12:34:56.789012"),
                (pa.time64("ns"), [45296789012345, None], "12:34:56.7890123"),
            ]
            for i, (arrow_type, values, expected_text) in enumerate(cases):
                name = "tp%d" % i
                cur.execute("CREATE TABLE %s (idx INTEGER, v TIME)" % name)
                data = pa.table({
                    "idx": pa.array([0, 1], pa.int32()),
                    "v": pa.array(values, arrow_type),
                })
                for rows in (2, 1):  # array-bound batch, then row-at-a-time
                    bind(cur, "INSERT INTO %s VALUES (?, ?)" % name, data, rows)
                cur.execute("SELECT v FROM %s ORDER BY rowid" % name)
                got = cur.fetch_arrow_table().column("v").to_pylist()
                assert len(got) == 4, got
                assert got[1] is None and got[3] is None, got
                # Nanosecond times are rendered with 7 digits, the largest
                # fractional scale SQL Server's TIME accepts.
                cur.execute("SELECT CAST(v AS TEXT) FROM %s WHERE v IS NOT NULL" % name)
                texts = cur.fetch_arrow_table().column(0).to_pylist()
                assert texts == [expected_text, expected_text], (arrow_type, texts)
                # ...and ingest picks a TIME column type for the same input.
                assert cur.adbc_ingest("ing%d" % i, data, mode="create") == 2
                cur.execute("SELECT v FROM ing%d ORDER BY idx" % i)
                assert cur.fetch_arrow_table().column("v").to_pylist()[1] is None

            # --- dictionary-encoded parameters (D11) ----------------------
            # What a pandas categorical column becomes: the index is resolved
            # and the dictionary's value bound in its place.
            cur.execute("CREATE TABLE dict_t (s TEXT, i INTEGER)")
            plain = pa.table({
                "s": pa.array(["a", "bb", None, "a"], pa.string()),
                "i": pa.array([10, 20, 30, None], pa.int32()),
            })
            encoded = pa.table(
                [c.combine_chunks().dictionary_encode() for c in plain.columns],
                names=plain.schema.names,
            )
            for rows in (2, 1):
                bind(cur, "INSERT INTO dict_t VALUES (?, ?)", encoded, rows)
            got = None
            cur.execute("SELECT s, i FROM dict_t ORDER BY rowid")
            got = cur.fetch_arrow_table().to_pydict()
            assert got["s"] == ["a", "bb", None, "a"] * 2, got
            assert got["i"] == [10, 20, 30, None] * 2, got
            # Ingest creates the column from the dictionary's value type.
            assert cur.adbc_ingest("dict_ing", encoded, mode="create") == 4
            cur.execute("SELECT s, i FROM dict_ing")
            back = cur.fetch_arrow_table()
            assert back.schema.field("s").type == pa.string(), back.schema
            assert sorted(x for x in back.column("i").to_pylist() if x is not None) == [10, 20, 30]

            # --- string_view / binary_view parameters (D11) ---------------
            # Values above 12 bytes live out of line in a view layout, so both
            # the inline and the out-of-line case are exercised.
            cur.execute("CREATE TABLE view_t (s TEXT, b BLOB)")
            views = pa.table({
                "s": pa.array(["short", "a much longer value than twelve bytes", None],
                              pa.string_view()),
                "b": pa.array([b"\x00\x01", None, b"x" * 40], pa.binary_view()),
            })
            for rows in (2, 1):
                bind(cur, "INSERT INTO view_t VALUES (?, ?)", views, rows)
            cur.execute("SELECT s, b FROM view_t ORDER BY rowid")
            got = cur.fetch_arrow_table().to_pydict()
            assert got["s"] == ["short", "a much longer value than twelve bytes", None] * 2, got
            assert got["b"] == [b"\x00\x01", None, b"x" * 40] * 2, got
            assert cur.adbc_ingest("view_ing", views, mode="create") == 3

            # --- timestamps of every Arrow unit still bind (D13) ----------
            for unit, value in (("s", 1709214310), ("ms", 1709214310123),
                                ("us", 1709214310123456), ("ns", 1709214310123456789)):
                cur.execute("CREATE TABLE ts_%s (v TIMESTAMP)" % unit)
                data = pa.table({"v": pa.array([value, None], pa.timestamp(unit))})
                for rows in (2, 1):
                    bind(cur, "INSERT INTO ts_%s VALUES (?)" % unit, data, rows)
                cur.execute("SELECT v FROM ts_%s WHERE v IS NOT NULL" % unit)
                got = cur.fetch_arrow_table().column("v").to_pylist()
                assert len(got) == 2, got
                assert got[0].replace(microsecond=0) == datetime.datetime(
                    2024, 2, 29, 13, 45, 10
                ), (unit, got)
    print("BIND TYPES OK")


if __name__ == "__main__":
    test_types()
    test_bind_types()


def test_metadata():
    """GetInfo / GetObjects / ingest-conflict contract (validation findings D1-D7)."""
    import pyarrow as pa

    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "meta.db")
    with connect(db) as conn:
        # --- D1: the driver name is adbcbridge's own, stable identity -----
        info = conn.adbc_get_info()
        assert info["driver_name"] == "ADBC ODBC Driver", info["driver_name"]
        # The backing ODBC driver is reported as vendor context and through an
        # adbcbridge-specific connection option, never inside driver_name.
        assert info["vendor_name"] == "SQLite (via ODBC)", info["vendor_name"]
        odbc_driver = conn.adbc_connection.get_option("adbc.odbc.driver_name")
        assert "sqlite" in odbc_driver.lower(), odbc_driver

        # --- D2: the Arrow version is a bare version string ---------------
        av = info["driver_arrow_version"]
        assert av.startswith("v") and av[1:].replace(".", "").isdigit(), av

        with conn.cursor() as cur:
            cur.execute("CREATE TABLE pk1 (a INTEGER NOT NULL PRIMARY KEY)")
            cur.execute("CREATE TABLE fk1 (b INTEGER, FOREIGN KEY (b) REFERENCES pk1 (a))")

        # --- D3: depth=catalogs is never empty ----------------------------
        cats = conn.adbc_get_objects(depth="catalogs").read_all().to_pylist()
        names = [c["catalog_name"] for c in cats]
        assert len(names) >= 1, names
        assert len(set(names)) == len(names), names
        # SQLite has exactly one, unnamed, catalog; it must still be listed.
        assert None in names, names
        schemas = [
            (c["catalog_name"], s["db_schema_name"])
            for c in conn.adbc_get_objects(depth="db_schemas").read_all().to_pylist()
            for s in c["catalog_db_schemas"] or []
        ]
        assert (None, None) in schemas, schemas

        # --- D4: catalog / db_schema filters are honoured ------------------
        def objs(**kw):
            return conn.adbc_get_objects(**kw).read_all().to_pylist()

        def tables_of(rows):
            return [
                t["table_name"]
                for c in rows
                for s in c["catalog_db_schemas"] or []
                for t in s["db_schema_tables"] or []
            ]

        assert objs(depth="catalogs", catalog_filter="nosuchcatalog") == []
        assert tables_of(objs(depth="tables", catalog_filter="nosuchcatalog")) == []
        assert tables_of(objs(depth="tables", db_schema_filter="nosuchschema")) == []
        # A NULL filter means "no filtering"; an empty filter names the unnamed catalog.
        assert "pk1" in tables_of(objs(depth="tables"))
        assert "pk1" in tables_of(objs(depth="tables", catalog_filter=""))
        # LIKE wildcards are applied, not compared literally.
        assert tables_of(objs(depth="tables", catalog_filter="no%catalog")) == []

        # --- D5/D6: constraint usage is NULL where it does not apply -------
        all_objs = objs(depth="all")
        by_name = {
            t["table_name"]: t
            for c in all_objs
            for s in c["catalog_db_schemas"] or []
            for t in s["db_schema_tables"] or []
        }
        pk = [c for c in by_name["pk1"]["table_constraints"]
              if c["constraint_type"] == "PRIMARY KEY"]
        assert len(pk) == 1, pk
        # Not an empty list: a primary key references nothing at all.
        assert pk[0]["constraint_column_usage"] is None, pk[0]

        fk = [c for c in by_name["fk1"]["table_constraints"]
              if c["constraint_type"] == "FOREIGN KEY"]
        assert len(fk) == 1, fk
        usage = fk[0]["constraint_column_usage"]
        assert len(usage) == 1, usage
        # SQLiteODBC reports the absent catalog/schema as "", which must become NULL.
        assert usage[0]["fk_catalog"] is None, usage
        assert usage[0]["fk_db_schema"] is None, usage
        assert usage[0]["fk_table"] == "pk1", usage
        assert usage[0]["fk_column_name"] == "a", usage

        # --- D7: ingest conflicts report ALREADY_EXISTS --------------------
        data = pa.table({"a": pa.array([1, 2], pa.int64()), "b": pa.array(["x", "y"])})
        with conn.cursor() as cur:
            assert cur.adbc_ingest("ing", data, mode="create") == 2
            for mode, payload in (
                ("create", data),
                # create_append cannot append a column the existing table lacks:
                # the table exists as something other than what was asked for.
                ("create_append",
                 data.append_column("extra", pa.array([0, 0], pa.int64()))),
            ):
                try:
                    cur.adbc_ingest("ing", payload, mode=mode)
                    raise AssertionError("expected ALREADY_EXISTS for mode=%s" % mode)
                except adbc_driver_manager.Error as e:
                    assert e.status_code == adbc_driver_manager.AdbcStatusCode.ALREADY_EXISTS, (
                        mode, e.status_code, str(e))
            # A matching create_append still appends.
            assert cur.adbc_ingest("ing", data, mode="create_append") == 2
            cur.execute("SELECT COUNT(*) FROM ing")
            assert cur.fetchone()[0] == 4
    print("METADATA OK")


if __name__ == "__main__":
    test_metadata()


def test_bound_params():
    """Parameter-binding regressions.

    D8  an Arrow `na` (untyped-null) parameter column binds as SQL NULL.
    D9  a multi-row parameter batch on a result-returning query executes once
        per row and exposes the concatenated result sets as one stream.
    D14 StatementGetParameterSchema reports one field per parameter marker.
    """
    import pyarrow as pa
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "bind.db")

    def stmt_of(cur, query, prepare=True):
        s = cur.adbc_statement
        s.set_sql_query(query)
        if prepare:
            s.prepare()
        return s

    def read(handle):
        return pa.RecordBatchReader._import_from_c(handle.address).read_all()

    def param_schema(s):
        # Keep the handle alive: it releases the ArrowSchema when collected.
        handle = s.get_parameter_schema()
        return pa.Schema._import_from_c(handle.address)

    with connect(db) as conn:
        with conn.cursor() as cur:
            cur.execute("CREATE TABLE t (id INTEGER, v TEXT)")

        # --- D8: Arrow `na` binds as a NULL parameter ----------------------
        with conn.cursor() as cur:
            s = stmt_of(cur, "INSERT INTO t (id, v) VALUES (?, ?)")
            s.bind(pa.RecordBatch.from_pydict(
                {"0": pa.array([7001], pa.int64()), "1": pa.nulls(1)}))
            s.execute_update()
        with conn.cursor() as cur:
            cur.execute("SELECT v FROM t WHERE id = 7001")
            assert cur.fetch_arrow_table().column("v").to_pylist() == [None]
        # ...and on a query that returns a result set
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT ? AS v")
            s.bind(pa.RecordBatch.from_pydict({"0": pa.nulls(2)}))
            tbl = read(s.execute_query()[0])
            assert tbl.column("v").to_pylist() == [None, None], tbl

        # --- D9: multi-row bind on a result-returning query ----------------
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1 + ? AS v")
            s.bind(pa.RecordBatch.from_pydict({"0": pa.array([1, 2, 3, 4], pa.int64())}))
            handle, rows = s.execute_query()
            tbl = read(handle)
            # The whole batch is one logical stream carrying the first result's schema.
            assert tbl.column("v").to_pylist() == [2, 3, 4, 5], tbl
            assert rows == -1, rows
        # many rows, so the lazy re-execution loop is driven many times
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT ? AS v")
            s.bind(pa.RecordBatch.from_pydict({"0": pa.array(range(500), pa.int64())}))
            tbl = read(s.execute_query()[0])
            assert tbl.column("v").to_pylist() == list(range(500)), tbl.num_rows
        # a query that yields several rows per execution
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT id FROM t WHERE id = ? OR id = ? ORDER BY id")
            s.bind(pa.RecordBatch.from_pydict(
                {"0": pa.array([7001, 7001], pa.int64()),
                 "1": pa.array([7001, 7001], pa.int64())}))
            tbl = read(s.execute_query()[0])
            assert tbl.column("id").to_pylist() == [7001, 7001], tbl
        # an empty parameter batch executes nothing and yields no rows
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1 + ? AS v")
            s.bind(pa.RecordBatch.from_pydict({"0": pa.array([], pa.int64())}))
            assert read(s.execute_query()[0]).num_rows == 0
        # a bound statement that returns nothing still reports rows affected,
        # even when the caller asked for a result stream
        with conn.cursor() as cur:
            s = stmt_of(cur, "INSERT INTO t (id, v) VALUES (?, ?)")
            s.bind(pa.RecordBatch.from_pydict(
                {"0": pa.array([1, 2, 3], pa.int64()), "1": pa.array(["a", "b", "c"])}))
            handle, rows = s.execute_query()
            assert read(handle).num_rows == 0
            assert rows == 3, rows

        # the stream owns the parameter stream and the statement handle, so it
        # stays readable after the statement that produced it is gone
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1 + ? AS v")
            s.bind(pa.RecordBatch.from_pydict({"0": pa.array([10, 20, 30], pa.int64())}))
            handle, _ = s.execute_query()
            reader = pa.RecordBatchReader._import_from_c(handle.address)
            first = reader.read_next_batch()
        assert first.column(0).to_pylist() == [11], first
        rest = [b.column(0).to_pylist() for b in reader]
        assert rest == [[21], [31]], rest

        # --- D14: StatementGetParameterSchema ------------------------------
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1 + ?")
            sch = param_schema(s)
            assert [f.name for f in sch] == ["0"], sch
            # SQLiteODBC cannot describe parameters, so the fallback applies.
            assert sch.field(0).type == pa.string() and sch.field(0).nullable, sch
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1 + ? + ?")
            sch = param_schema(s)
            assert [f.name for f in sch] == ["0", "1"], sch
        with conn.cursor() as cur:
            s = stmt_of(cur, "SELECT 1")
            sch = param_schema(s)
            assert len(sch) == 0, sch
        # it also works without an explicit prepare
        with conn.cursor() as cur:
            s = stmt_of(cur, "INSERT INTO t (id, v) VALUES (?, ?)", prepare=False)
            sch = param_schema(s)
            assert len(sch) == 2, sch
    print("BOUND PARAMS OK")


if __name__ == "__main__":
    test_bound_params()
    test_release_with_open_transaction_drops_locks()


def test_multirow_ingest():
    """Multi-row INSERT batching for bulk ingest (adbc.odbc.rows_per_insert).

    Ingest packs K rows into one `INSERT ... VALUES (...),(...)` instead of executing a
    one-row INSERT K times.  What has to hold whatever K is: NULLs land in the right
    row-group, a batch that is not a whole multiple of K still ingests exactly once, a
    failure part way through leaves no rows at all, and a K the backend will not prepare
    is narrowed until one is.
    """
    import pyarrow as pa

    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, "mr.db")

    def rows_of(cur, table, cols="*"):
        cur.execute('SELECT %s FROM "%s" ORDER BY "id"' % (cols, table))
        return cur.fetch_arrow_table().to_pydict()

    with connect(db) as conn:
        with conn.cursor() as cur:
            # --- NULLs in every row-group position ------------------------
            # Row i has a NULL in column i % 4 and values everywhere else, so with any K
            # every position inside a row-group is a NULL in some group -- and the row
            # that follows a NULL row must not inherit it.
            n = 41  # deliberately not a multiple of any K used below
            cols = ["a", "b", "c", "d"]
            data = {
                "id": pa.array(range(n), pa.int32()),
                "a": pa.array([None if i % 4 == 0 else i for i in range(n)], pa.int64()),
                "b": pa.array([None if i % 4 == 1 else "s%d" % i for i in range(n)]),
                "c": pa.array([None if i % 4 == 2 else i * 0.5 for i in range(n)], pa.float64()),
                "d": pa.array([None if i % 4 == 3 else i % 20000 for i in range(n)], pa.date32()),
            }
            tbl = pa.table(data)
            for k in (0, 1, 2, 3, 7, 40, 1000):
                name = "nulls_k%d" % k
                cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": str(k)})
                assert cur.adbc_ingest(name, tbl, mode="create") == n
                got = rows_of(cur, name)
                assert got["id"] == list(range(n)), (k, got["id"][:5])
                for j, col in enumerate(cols):
                    want = [None if i % 4 == j else True for i in range(n)]
                    have = [None if v is None else True for v in got[col]]
                    assert have == want, (k, col, have[:8], want[:8])
                assert got["b"][2] == "s2" and got["a"][1] == 1

            # --- a batch that is not a multiple of K, and a single-row batch ---
            cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": "7"})
            for size in (1, 2, 6, 7, 8, 14, 15, 100):
                t = pa.table({"id": pa.array(range(size), pa.int32()),
                              "v": pa.array(["r%d" % i for i in range(size)])})
                name = "size%d" % size
                assert cur.adbc_ingest(name, t, mode="create") == size, size
                got = rows_of(cur, name)
                assert got["id"] == list(range(size)), size
                assert got["v"] == ["r%d" % i for i in range(size)], size

            # An empty batch has nothing to group and must still create the table.
            empty = pa.table({"id": pa.array([], pa.int32()), "v": pa.array([], pa.string())})
            assert cur.adbc_ingest("empty_t", empty, mode="create") == 0
            cur.execute('SELECT COUNT(*) FROM "empty_t"')
            assert cur.fetchone()[0] == 0

            # --- a K the backend will not prepare is narrowed --------------
            # SQLite caps the parameters of one statement (999 or 32766 depending on the
            # build).  1000 row-groups of 120 columns is 120,000 parameters, past either
            # -- the ingest must find a K that prepares rather than fail.
            wide_cols = 120
            wide = pa.table({"id": pa.array(range(30), pa.int32()),
                             **{"c%d" % c: pa.array([c * 1000 + r for r in range(30)], pa.int64())
                                for c in range(wide_cols)}})
            cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": "1000"})
            assert cur.adbc_ingest("wide_t", wide, mode="create") == 30
            got = rows_of(cur, "wide_t")
            assert got["id"] == list(range(30))
            assert got["c0"] == list(range(30))
            assert got["c119"] == [119000 + r for r in range(30)]

            # An out-of-range option value is refused rather than quietly ignored.
            for bad in ("-1", "banana", "12x"):
                try:
                    cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": bad})
                    raise AssertionError("expected a rejection for %r" % bad)
                except AssertionError:
                    raise
                except Exception:
                    pass
            cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": "0"})

    # --- a failure part way through leaves no rows ----------------------
    # The driver batches a whole ingest into one transaction of its own when the caller
    # has not opened one, so a row that the server refuses must roll the lot back.
    uri = f"Driver={SQLITE_ODBC};Database={db};"
    with dbapi.connect(driver=DRIVER, autocommit=True,
                       db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"}) as conn:
        with conn.cursor() as cur:
            cur.execute('CREATE TABLE "uniq" ("id" INTEGER PRIMARY KEY, "v" TEXT)')
            cur.execute('INSERT INTO "uniq" VALUES (700, \'planted\')')
            n = 1000
            clash = pa.table({"id": pa.array(range(n), pa.int32()),
                              "v": pa.array(["r%d" % i for i in range(n)])})
            for k in ("0", "7", "1"):
                cur.adbc_statement.set_options(**{"adbc.odbc.rows_per_insert": k})
                try:
                    cur.adbc_ingest("uniq", clash, mode="append")
                    raise AssertionError("expected the duplicate key to fail (K=%s)" % k)
                except AssertionError:
                    raise
                except Exception:
                    pass
                cur.execute('SELECT COUNT(*) FROM "uniq"')
                assert cur.fetchone()[0] == 1, ("half a table left behind at K=%s" % k)
                cur.execute('SELECT "v" FROM "uniq"')
                assert cur.fetchone()[0] == "planted"

    print("MULTIROW INGEST OK")


if __name__ == "__main__":
    test_multirow_ingest()
