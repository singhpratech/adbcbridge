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

"""Columns bound at a width the driver only guessed at, and the values that outgrow it.

A `bytea` or `text` column has no declared length, so `ApplyBindWidth()` binds it at
`adbc.odbc.long_bind_bytes` (2 KiB) and re-reads whatever overflows that -- and
`AdaptBindWidth()` gives the binding up part-way through the result set when the values
turn out not to fit at all, which drops the rowset back to one row.  Both halves of that
have to return exactly the same bytes as a column that was never bound:

* every value comes back whole, however far past the bound width it is;
* it comes back on the row it belongs to, which is the part a re-read can get wrong
  (SQLGetData reads whichever row the cursor is on, so the re-read has to move it);
* NULL and empty stay NULL and empty on both paths.

The tables are shaped around the decision `AdaptBindWidth()` takes: all-small (it keeps
the binding), all-large (it gives it up), and mixed -- 1% large and 99% small, which is
the case that decides whether the rule is about rows or about bytes.  What the decision
costs and saves is measured in bench/BENCHMARKS.md; timings are not asserted here.

Needs a PostgreSQL server (PG_URI) and psqlodbc (POSTGRES_ODBC_DRIVER); skips otherwise.

    PG_URI=postgresql://adbc:adbc@127.0.0.1:15432/adbc \\
    POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python -m pytest tests/test_long_columns.py
"""

import hashlib
import os
import pathlib
import urllib.parse

import adbc_driver_manager as adm
import pyarrow
import pytest

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER")
PG_CONN = os.environ.get("PG_URI")
SUFFIX = os.environ.get("ADBC_MATRIX_SUFFIX", "")

pytestmark = pytest.mark.skipif(
    not (PSQLODBC and PG_CONN and os.path.exists(DRIVER)),
    reason="needs PG_URI, POSTGRES_ODBC_DRIVER and a built adbcbridge",
)


def connstr():
    u = urllib.parse.urlparse(PG_CONN)
    return "DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;" % (
        PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
        (u.path or "/postgres").lstrip("/"), u.username or "", u.password or "",
    )


def connect(**opts):
    db = adm.AdbcDatabase(
        driver=DRIVER, entrypoint="AdbcDriverInit",
        **{"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never", **opts},
    )
    return db, adm.AdbcConnection(db)


def execute(conn, sql):
    stmt = adm.AdbcStatement(conn)
    stmt.set_sql_query(sql)
    stmt.execute_update()
    stmt.close()


def read_all(conn, sql, **opts):
    stmt = adm.AdbcStatement(conn)
    if opts:
        stmt.set_options(**opts)
    stmt.set_sql_query(sql)
    handle, _ = stmt.execute_query()
    table = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
    stmt.close()
    return table


def payload(i, n):
    """The value row `i` holds: `n` bytes that name the row at both ends.

    A re-read that lands on the neighbouring row fails on the md5, and one that stops at
    the bound width fails on the length and on the tail.
    """
    tag = hashlib.md5(str(i).encode()).hexdigest().encode()
    return tag + b"x" * (n - 2 * len(tag)) + tag


# Rows, and the size of the value each of them holds.  `mixed` is 1% large: the rule
# `AdaptBindWidth` uses has to see that as a column worth giving up on (655 bytes of
# re-read per row) even though 99 rows in 100 never truncate.
SHAPES = {
    "small": lambda i: 64,                                     # never truncates
    "large": lambda i: 3072,                                   # always truncates
    "mixed": lambda i: 65536 if i % 100 == 7 else 64,          # 1% of rows truncate
    "half": lambda i: 4096 if i % 2 == 0 else 64,              # half of them do
}
ROWS = 1200  # more than one probe window (256 rows), so the decision is taken and used


@pytest.fixture(scope="module")
def owner():
    db, conn = connect()
    yield conn
    conn.close()
    db.close()


@pytest.fixture(scope="module")
def tables(owner):
    """One table per shape, plus a NULL, an empty and a one-byte value in every one."""
    names = {}
    for shape, size in SHAPES.items():
        name = '"adbc_long_%s%s"' % (shape, SUFFIX)
        execute(owner, "DROP TABLE IF EXISTS " + name)
        execute(owner, "CREATE TABLE %s (i INTEGER, b BYTEA, t TEXT)" % name)
        # STORAGE EXTERNAL keeps PostgreSQL from compressing the values away, so what
        # crosses the wire really is the size the shape asks for.
        execute(owner, "ALTER TABLE %s ALTER COLUMN b SET STORAGE EXTERNAL" % name)
        rows = []
        for i in range(1, ROWS + 1):
            if i == 3:
                rows.append("(%d, NULL, NULL)" % i)
            elif i == 4:
                rows.append("(%d, ''::bytea, '')" % i)
            else:
                v = payload(i, size(i))
                rows.append("(%d, '\\x%s'::bytea, '%s')" % (i, v.hex(), v.decode()))
        for start in range(0, len(rows), 200):
            execute(owner, "INSERT INTO %s VALUES %s"
                           % (name, ",".join(rows[start:start + 200])))
        names[shape] = name
    yield names
    for name in names.values():
        execute(owner, "DROP TABLE IF EXISTS " + name)


def check(table, shape):
    """Every row of `table` holds exactly what the shape says it should."""
    size = SHAPES[shape]
    assert table.num_rows == ROWS
    ids = table.column("i").to_pylist()
    assert ids == list(range(1, ROWS + 1))
    for name in ("b", "t"):
        got = table.column(name).to_pylist()
        for i, v in zip(ids, got):
            if isinstance(v, str):
                v = v.encode()
            if i == 3:
                assert v is None, (shape, name, i)
            elif i == 4:
                assert v == b"", (shape, name, i)
            else:
                assert v == payload(i, size(i)), (
                    shape, name, i, len(v) if v is not None else None)


@pytest.mark.parametrize("shape", list(SHAPES))
@pytest.mark.parametrize("batch", ["1024", "7"])
def test_values_survive_a_bound_width_they_outgrow(owner, tables, shape, batch):
    """The whole value, on the right row, whatever the reader decided about the column.

    At batch_size 1024 the reader fetches a block cursor and takes the decision after two
    probe rowsets; at 7 the rowset is seven rows and the window spans many of them.  Both
    have to agree with the values that went in.
    """
    sql = "SELECT i, b, t FROM %s ORDER BY i" % tables[shape]
    check(read_all(owner, sql, **{"adbc.odbc.batch_size": batch}), shape)


@pytest.mark.parametrize("shape", list(SHAPES))
def test_the_rowset_the_values_arrived_in_does_not_show(owner, tables, shape):
    """A read that never had a block cursor is the reference for one that adapts.

    `adbc.odbc.batch_size=1` fetches one row at a time from the start, so nothing is ever
    repaired against a block cursor and `AdaptBindWidth()` is never armed (there is no
    rowset to give up).  Whatever the adapting read decides, the two have to agree.
    """
    sql = "SELECT i, b, t FROM %s ORDER BY i" % tables[shape]
    adapting = read_all(owner, sql)
    one_row = read_all(owner, sql, **{"adbc.odbc.batch_size": "1"})
    assert adapting.schema == one_row.schema
    assert adapting.equals(one_row.combine_chunks())
    check(one_row, shape)


def test_a_column_that_never_truncates_keeps_its_block_cursor(owner, tables):
    """The batches of an all-small read are the ones a non-adapting reader produced.

    The probe rowsets the decision is taken in are smaller than the full one, and this
    pins that they are not visible: the batches stay `batch_size` rows.  (A `bytea`
    column is what makes this reader adapt at all; the four-column table next to it has
    no length-less column and never enters the machinery.)
    """
    stmt = adm.AdbcStatement(owner)
    stmt.set_options(**{"adbc.odbc.batch_size": "512"})
    stmt.set_sql_query("SELECT i, b FROM %s ORDER BY i" % tables["small"])
    handle, _ = stmt.execute_query()
    reader = pyarrow.RecordBatchReader._import_from_c(handle.address)
    sizes = [b.num_rows for b in reader]
    stmt.close()
    assert sizes == [512, 512, 176], sizes


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
