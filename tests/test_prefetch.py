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

"""`adbc.odbc.prefetch`: fetching the next rowset while converting the current one.

Prefetch moves the ODBC handle to a background thread, so the things worth testing are
not the happy path's speed but its edges: that the rows are identical to the synchronous
read, that an error part-way through a result set surfaces the same way and at the same
point, and that abandoning a stream mid-flight stops the fetch thread instead of leaving
it running against a handle that is about to be freed.

SQLite needs no server (SQLITE_ODBC_DRIVER); the mid-stream error test needs PostgreSQL
with a real cursor (PG_URI), and skips otherwise.  Run under pytest, or directly:

    SQLITE_ODBC_DRIVER=/path/libsqlite3odbc.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/test_prefetch.py
"""

import os
import pathlib
import sqlite3
import tempfile
import urllib.parse

import pytest

import adbc_driver_manager as adm
import pyarrow
import pyarrow.compute as pc

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER")
PG_CONN = os.environ.get("PG_URI")

# Enough rows that a 1024-row batch size means many rowsets, so the ring actually cycles.
ROWS = 40000

pytestmark = pytest.mark.skipif(not os.path.exists(DRIVER), reason="adbcbridge is not built")


def open_conn(connstr, **options):
    opts = {"adbc.odbc.connection_string": connstr, "adbc.odbc.delegate": "never"}
    opts.update({k: str(v) for k, v in options.items()})
    db = adm.AdbcDatabase(driver=DRIVER, entrypoint="AdbcDriverInit", **opts)
    return db, adm.AdbcConnection(db)


def reader_for(conn, sql):
    stmt = adm.AdbcStatement(conn)
    stmt.set_sql_query(sql)
    handle, _ = stmt.execute_query()
    return stmt, pyarrow.RecordBatchReader._import_from_c(handle.address)


def read_table(connstr, sql, prefetch, **options):
    db, conn = open_conn(connstr, **{"adbc.odbc.prefetch": prefetch}, **options)
    try:
        stmt, rdr = reader_for(conn, sql)
        table = rdr.read_all()
        stmt.close()
        return table
    finally:
        conn.close()
        db.close()


def fingerprint(table):
    out = {"rows": table.num_rows, "cols": tuple(table.column_names)}
    for name in table.column_names:
        col = table.column(name)
        if pyarrow.types.is_integer(col.type) or pyarrow.types.is_floating(col.type):
            out[name] = (pc.sum(col).as_py(), pc.min(col).as_py(), pc.max(col).as_py())
        elif pyarrow.types.is_date(col.type):
            days = col.cast(pyarrow.int32(), safe=False)
            out[name] = (pc.sum(days).as_py(), pc.min(days).as_py(), pc.max(days).as_py())
        else:
            out[name] = (pc.sum(pc.binary_length(col)).as_py(),
                         col[0].as_py(), col[-1].as_py())
    return out


# --------------------------------------------------------------------------
# SQLite fixture -- no server needed


@pytest.fixture(scope="module")
def sqlite_db():
    tmp = tempfile.mkdtemp(prefix="adbcbridge-prefetch-")
    path = os.path.join(tmp, "prefetch.db")
    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=OFF")
    con.execute("PRAGMA synchronous=OFF")
    con.execute("CREATE TABLE bench (id INTEGER, val DOUBLE, txt VARCHAR(32), dt DATE)")
    # A second table whose text column has no declared width: sqliteodbc describes a bare
    # TEXT column as 65,536 characters, so the reader binds it clipped -- which is exactly
    # the shape prefetch must refuse (a clipped value may need re-reading from a row the
    # fetch thread has already scrolled past).
    con.execute("CREATE TABLE wide (id INTEGER, txt TEXT)")
    con.executemany("INSERT INTO bench VALUES (?,?,?,?)",
                    ((g, g * 1.5, "row-%012d" % g, "2020-01-%02d" % (g % 28 + 1))
                     for g in range(1, ROWS + 1)))
    con.executemany("INSERT INTO wide VALUES (?,?)",
                    ((g, "x" * (g % 97)) for g in range(1, ROWS + 1)))
    con.commit()
    con.close()
    yield "DRIVER=%s;Database=%s;" % (SQLITE_ODBC, path)
    try:
        os.remove(path)
        os.rmdir(tmp)
    except OSError:
        pass


# --------------------------------------------------------------------------
# the rows must not change


@pytest.mark.parametrize("prefetch", [0, 1, 2, 4])
def test_prefetch_reads_the_same_rows(sqlite_db, prefetch):
    sql = "SELECT id, val, txt, dt FROM bench"
    assert fingerprint(read_table(sqlite_db, sql, prefetch)) == \
           fingerprint(read_table(sqlite_db, sql, 0))


@pytest.mark.parametrize("batch_size", [1, 7, 1024, 100000])
def test_prefetch_at_odd_batch_sizes(sqlite_db, batch_size):
    """A batch smaller than a rowset, and a batch far larger than the result set."""
    sql = "SELECT id, val, txt, dt FROM bench"
    opts = {"adbc.odbc.batch_size": batch_size}
    assert fingerprint(read_table(sqlite_db, sql, 2, **opts)) == \
           fingerprint(read_table(sqlite_db, sql, 0, **opts))


def test_prefetch_on_an_empty_result_set(sqlite_db):
    sql = "SELECT id, val, txt, dt FROM bench WHERE id < 0"
    for prefetch in (0, 1, 2):
        assert read_table(sqlite_db, sql, prefetch).num_rows == 0


def test_prefetch_falls_back_for_a_clipped_column(sqlite_db):
    """A column bound narrower than its declared width disables prefetch silently.

    The point is that asking for prefetch on such a query is not an error and does not
    change the answer -- the reader just reads the ordinary way, values wider than the
    bound buffer included.
    """
    sql = "SELECT id, txt FROM wide"
    plain = read_table(sqlite_db, sql, 0)
    assert fingerprint(read_table(sqlite_db, sql, 2)) == fingerprint(plain)
    # ... and the long values really did come back whole.
    assert pc.max(pc.binary_length(plain.column("txt"))).as_py() == 96


# --------------------------------------------------------------------------
# abandoning a stream mid-flight


@pytest.mark.parametrize("prefetch", [0, 1, 2])
def test_abort_partway_through(sqlite_db, prefetch):
    """Release the stream after one batch, with the fetch thread still running.

    Repeated, because a teardown race shows up as a hang or a use-after-free some of the
    time rather than every time.
    """
    sql = "SELECT id, val, txt, dt FROM bench"
    for _ in range(20):
        db, conn = open_conn(sqlite_db, **{"adbc.odbc.prefetch": prefetch,
                                           "adbc.odbc.batch_size": 512})
        stmt, rdr = reader_for(conn, sql)
        batch = rdr.read_next_batch()
        assert batch.num_rows > 0
        del rdr  # releases the ArrowArrayStream: the fetch thread must stop here
        stmt.close()
        conn.close()
        db.close()


@pytest.mark.parametrize("prefetch", [0, 1, 2])
def test_abort_before_reading_anything(sqlite_db, prefetch):
    """The fetch thread starts on the first batch, so this is the never-started case."""
    sql = "SELECT id, val, txt, dt FROM bench"
    for _ in range(20):
        db, conn = open_conn(sqlite_db, **{"adbc.odbc.prefetch": prefetch})
        stmt, rdr = reader_for(conn, sql)
        del rdr
        stmt.close()
        conn.close()
        db.close()


def test_statement_reused_after_an_aborted_prefetch(sqlite_db):
    """The handle has to come back clean enough for the next execute on it."""
    db, conn = open_conn(sqlite_db, **{"adbc.odbc.prefetch": 2, "adbc.odbc.batch_size": 256})
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query("SELECT id, val, txt, dt FROM bench")
        handle, _ = stmt.execute_query()
        rdr = pyarrow.RecordBatchReader._import_from_c(handle.address)
        rdr.read_next_batch()
        del rdr
        # Same statement, executed again.
        handle, _ = stmt.execute_query()
        table = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
        assert table.num_rows == ROWS
        stmt.close()
    finally:
        conn.close()
        db.close()


# --------------------------------------------------------------------------
# options


@pytest.mark.parametrize("value", ["-1", "9", "abc", "1.5", ""])
def test_invalid_prefetch_values_are_rejected(sqlite_db, value):
    with pytest.raises(adm.Error):
        open_conn(sqlite_db, **{"adbc.odbc.prefetch": value})


def test_prefetch_option_round_trips(sqlite_db):
    db, conn = open_conn(sqlite_db)
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_options(**{"adbc.odbc.prefetch": "3"})
        assert stmt.get_option_int("adbc.odbc.prefetch") == 3
        stmt.close()
        conn.set_options(**{"adbc.odbc.prefetch": "2"})
        assert conn.get_option_int("adbc.odbc.prefetch") == 2
    finally:
        conn.close()
        db.close()


def test_prefetch_inherits_from_the_connection(sqlite_db):
    """A statement that says nothing takes the connection's setting."""
    db, conn = open_conn(sqlite_db, **{"adbc.odbc.prefetch": 2})
    try:
        stmt = adm.AdbcStatement(conn)
        assert stmt.get_option_int("adbc.odbc.prefetch") == 2
        stmt.set_sql_query("SELECT id, val, txt, dt FROM bench")
        handle, _ = stmt.execute_query()
        assert pyarrow.RecordBatchReader._import_from_c(
            handle.address).read_all().num_rows == ROWS
        stmt.close()
    finally:
        conn.close()
        db.close()


# --------------------------------------------------------------------------
# an error part-way through the result set


@pytest.mark.skipif(not (PSQLODBC and PG_CONN),
                    reason="needs PG_URI and POSTGRES_ODBC_DRIVER")
@pytest.mark.parametrize("prefetch", [0, 1, 2])
def test_error_midstream(prefetch):
    """A server-side error after N rows must surface identically with prefetch on.

    `UseDeclareFetch=1` is what makes this a *mid-stream* error at all: without it
    psqlodbc drains the whole result set inside SQLExecDirect, and the division by zero
    is raised before a single SQLFetch.  With a cursor the error arrives from the fetch
    thread, which is the path under test.
    """
    u = urllib.parse.urlparse(PG_CONN)
    connstr = ("DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;"
               "UseDeclareFetch=1;Fetch=5000;"
               % (PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
                  (u.path or "/postgres").lstrip("/"), u.username or "", u.password or ""))
    db, conn = open_conn(connstr, **{"adbc.odbc.prefetch": prefetch})
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(
            "CREATE TEMP TABLE prefetch_boom AS "
            "SELECT g AS id FROM generate_series(1, 200000) g")
        stmt.execute_update()
        stmt.set_sql_query("SELECT id, 1 / (id - 100000) AS boom FROM prefetch_boom")
        handle, _ = stmt.execute_query()
        rdr = pyarrow.RecordBatchReader._import_from_c(handle.address)
        seen = 0
        with pytest.raises(Exception) as excinfo:
            for batch in rdr:
                seen += batch.num_rows
        assert "division by zero" in str(excinfo.value)
        # The rows before the bad one were delivered, and the stream stopped short of it.
        assert 0 < seen < 200000
        del rdr
        stmt.close()
    finally:
        conn.close()
        db.close()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
