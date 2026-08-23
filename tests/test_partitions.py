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

"""Partitioned result sets over ODBC.

The property under test is the one the whole feature rests on: reading every partition
descriptor -- in any order, concurrently, on connections that know nothing about the
statement that produced them -- yields exactly the rows of the unpartitioned query, with
the same schema, no duplicates and no gaps.  A row count alone would not catch a split
that dropped one row and duplicated another, so every check is a checksum over every
column of the whole result set.

Needs a PostgreSQL server (PG_URI) and psqlodbc (POSTGRES_ODBC_DRIVER); skips
otherwise.  Run under pytest, or directly:

    PG_URI=postgresql://... POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/test_partitions.py
"""

import concurrent.futures
import os
import pathlib
import random
import urllib.parse

import pytest

import adbc_driver_manager as adm
import pyarrow
import pyarrow.compute as pc

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER")
PG_CONN = os.environ.get("PG_URI")

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


def connect():
    db = adm.AdbcDatabase(
        driver=DRIVER, entrypoint="AdbcDriverInit",
        **{"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never"},
    )
    return db, adm.AdbcConnection(db)


def to_table(handle):
    return pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()


def execute(conn, sql):
    """Run a statement for its side effect."""
    stmt = adm.AdbcStatement(conn)
    stmt.set_sql_query(sql)
    stmt.execute_update()
    stmt.close()


def read_all(conn, sql):
    stmt = adm.AdbcStatement(conn)
    stmt.set_sql_query(sql)
    handle, _ = stmt.execute_query()
    table = to_table(handle)
    stmt.close()
    return table


def partitions_of(conn, sql, nparts):
    stmt = adm.AdbcStatement(conn)
    stmt.set_options(**{"adbc.odbc.partitions": str(nparts)})
    stmt.set_sql_query(sql)
    descriptors, schema, rows = stmt.execute_partitions()
    stmt.close()
    return [bytes(d) for d in descriptors], schema, rows


def fingerprint(table):
    """Order-independent fingerprint of a whole result set.

    A count would miss a swap of one row for another, and a sum of one column would miss
    a split that mixed up columns, so every column contributes.
    """
    out = {"rows": table.num_rows, "cols": tuple(table.column_names)}
    for name in table.column_names:
        col = table.column(name)
        if col.null_count == table.num_rows and table.num_rows:
            out[name] = "all-null"
        elif pyarrow.types.is_integer(col.type) or pyarrow.types.is_floating(col.type):
            out[name] = (pc.sum(col).as_py(), pc.min(col).as_py(), pc.max(col).as_py())
        elif pyarrow.types.is_date(col.type):
            days = col.cast(pyarrow.int32(), safe=False)
            out[name] = (pc.sum(days).as_py(), pc.min(days).as_py(), pc.max(days).as_py())
        else:
            # Sum of the byte lengths plus the sorted set of distinct values, which
            # catches a duplicated row that a length sum alone would not.
            out[name] = (pc.sum(pc.binary_length(col)).as_py(),
                         tuple(sorted(set(col.to_pylist()))[:16]))
    return out


# --------------------------------------------------------------------------
# fixtures


@pytest.fixture(scope="module")
def owner():
    """A connection that owns the scratch tables for this module."""
    db, conn = connect()
    yield conn
    conn.close()
    db.close()


def make_table(conn, name, rows, dead_tuples=False, vacuum=False):
    execute(conn, "DROP TABLE IF EXISTS %s" % name)
    execute(conn, "CREATE TABLE %s (id int, val double precision, "
                  "txt varchar(32), dt date)" % name)
    if rows:
        execute(conn, "INSERT INTO %s SELECT g, g * 1.5, 'row-' || lpad(g::text, 12, '0'), "
                      "DATE '2020-01-01' + (g %% 3000) FROM generate_series(1, %d) g"
                      % (name, rows))
    if dead_tuples:
        # Rewrite a third of the rows.  Each UPDATE leaves the old tuple dead in place and
        # writes a new one, usually in a different block -- so the heap grows, the live
        # rows move, and any split that assumed a tidy [0, relpages) mapping breaks.
        execute(conn, "UPDATE %s SET val = val + 1 WHERE id %% 3 = 0" % name)
    if vacuum:
        execute(conn, "VACUUM %s" % name)
    return name


# --------------------------------------------------------------------------
# the equivalence property


@pytest.mark.parametrize("nparts", [1, 2, 3, 4, 8, 16])
@pytest.mark.parametrize("rows", [0, 1, 1000, 50000])
def test_partitions_reproduce_the_whole_result(owner, nparts, rows):
    """Every partition count, over every table size, adds up to the same read."""
    name = make_table(owner, "part_eq_%d" % rows, rows)
    sql = "SELECT id, val, txt, dt FROM %s" % name
    expected = fingerprint(read_all(owner, sql))

    descriptors, schema, _ = partitions_of(owner, sql, nparts)
    assert len(descriptors) >= 1
    tables = [to_table(owner.read_partition(d)) for d in descriptors]
    got = pyarrow.concat_tables(tables)
    assert fingerprint(got) == expected

    # The schema ExecutePartitions reports is the unpartitioned query's schema, and every
    # partition's stream carries it too -- otherwise the pieces would not concatenate.
    reported = pyarrow.Schema._import_from_c(schema.address)
    assert reported.names == got.schema.names
    for t in tables:
        assert t.schema.names == got.schema.names


def test_partitions_over_a_table_with_dead_tuples(owner):
    """Dead tuples move live rows between blocks; the ctid split must still cover them.

    Two shapes matter: dead tuples still in place (the heap is full of holes and the
    block count overstates the live rows), and dead tuples reclaimed by VACUUM (the free
    space map fills earlier blocks back in).
    """
    for suffix, vacuum in (("dirty", False), ("vacuumed", True)):
        name = make_table(owner, "part_dead_%s" % suffix, 40000,
                          dead_tuples=True, vacuum=vacuum)
        sql = "SELECT id, val, txt, dt FROM %s" % name
        expected = fingerprint(read_all(owner, sql))
        for nparts in (2, 4, 7):
            descriptors, _, _ = partitions_of(owner, sql, nparts)
            got = pyarrow.concat_tables(
                [to_table(owner.read_partition(d)) for d in descriptors])
            assert fingerprint(got) == expected, "%s at N=%d" % (suffix, nparts)


def test_partitions_of_an_empty_table(owner):
    name = make_table(owner, "part_empty", 0)
    sql = "SELECT id, val, txt, dt FROM %s" % name
    descriptors, _, _ = partitions_of(owner, sql, 8)
    # A table with no heap blocks cannot be sliced into more than one piece.
    assert len(descriptors) == 1
    got = pyarrow.concat_tables([to_table(owner.read_partition(d)) for d in descriptors])
    assert got.num_rows == 0
    assert got.schema.names == ["id", "val", "txt", "dt"]


# --------------------------------------------------------------------------
# descriptors are self-contained


def test_partitions_read_out_of_order_and_on_other_connections(owner):
    """A descriptor carries everything its slice needs and outlives its statement.

    Read in a shuffled order, each on a connection opened after the descriptors were
    produced and after the producing statement was closed.
    """
    name = make_table(owner, "part_order", 30000)
    sql = "SELECT id, val, txt, dt FROM %s" % name
    expected = fingerprint(read_all(owner, sql))

    descriptors, _, _ = partitions_of(owner, sql, 6)
    shuffled = list(descriptors)
    random.Random(20260823).shuffle(shuffled)

    tables = []
    for d in shuffled:
        db, conn = connect()  # a connection that has never seen the statement
        try:
            tables.append(to_table(conn.read_partition(d)))
        finally:
            conn.close()
            db.close()
    assert fingerprint(pyarrow.concat_tables(tables)) == expected


def test_partitions_read_concurrently(owner):
    """N threads, N connections, all reading at once -- the point of the feature."""
    name = make_table(owner, "part_concurrent", 60000)
    sql = "SELECT id, val, txt, dt FROM %s" % name
    expected = fingerprint(read_all(owner, sql))

    descriptors, _, _ = partitions_of(owner, sql, 8)

    def worker(descriptor):
        db, conn = connect()
        try:
            return to_table(conn.read_partition(descriptor))
        finally:
            conn.close()
            db.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(descriptors)) as pool:
        tables = list(pool.map(worker, descriptors))
    assert fingerprint(pyarrow.concat_tables(tables)) == expected


def test_partition_descriptor_from_another_driver_is_rejected(owner):
    with pytest.raises(adm.Error):
        owner.read_partition(b"not-an-adbcbridge-descriptor")
    with pytest.raises(adm.Error):
        owner.read_partition(b"")


# --------------------------------------------------------------------------
# the single-partition fallback


@pytest.mark.parametrize("sql", [
    "SELECT count(*) FROM part_fallback",
    "SELECT id FROM part_fallback WHERE id > 10",
    "SELECT id FROM part_fallback ORDER BY id",
    "SELECT id FROM part_fallback LIMIT 100",
    "SELECT DISTINCT dt FROM part_fallback",
    "SELECT a.id FROM part_fallback a JOIN part_fallback b ON a.id = b.id",
    "SELECT id FROM (SELECT id FROM part_fallback) t",
    "SELECT 'from' AS x FROM part_fallback",
    "SELECT 1",
])
def test_unsplittable_queries_get_one_partition(owner, sql):
    """Anything the driver cannot prove it can slice must come back whole, not wrong."""
    make_table(owner, "part_fallback", 5000)
    descriptors, _, _ = partitions_of(owner, sql, 8)
    assert len(descriptors) == 1
    got = to_table(owner.read_partition(descriptors[0]))
    assert fingerprint(got) == fingerprint(read_all(owner, sql))


def test_partitions_option_of_one_never_splits(owner):
    """`adbc.odbc.partitions=1` is the off switch, even for a splittable query."""
    name = make_table(owner, "part_off", 20000)
    sql = "SELECT id, val, txt, dt FROM %s" % name
    descriptors, _, _ = partitions_of(owner, sql, 1)
    assert len(descriptors) == 1
    # ... and the descriptor is the original query, so the read is byte-for-byte the
    # one ExecuteQuery would have done.
    assert descriptors[0].endswith(sql.encode())


def test_a_view_is_not_split(owner):
    """A relation with no heap of its own has no ctid range to slice."""
    make_table(owner, "part_view_base", 20000)
    execute(owner, "DROP VIEW IF EXISTS part_view")
    execute(owner, "CREATE VIEW part_view AS SELECT * FROM part_view_base")
    sql = "SELECT id, val, txt, dt FROM part_view"
    descriptors, _, _ = partitions_of(owner, sql, 4)
    assert len(descriptors) == 1
    got = to_table(owner.read_partition(descriptors[0]))
    assert got.num_rows == 20000
    execute(owner, "DROP VIEW part_view")


def test_execute_partitions_rejects_bound_parameters(owner):
    """A descriptor is SQL text; there is nowhere to carry parameters."""
    make_table(owner, "part_params", 100)
    stmt = adm.AdbcStatement(owner)
    stmt.set_sql_query("SELECT id FROM part_params WHERE id > ?")
    params = pyarrow.record_batch([pyarrow.array([5])], names=["0"])
    stmt.bind(params)
    with pytest.raises(adm.Error):
        stmt.execute_partitions()
    stmt.close()


# --------------------------------------------------------------------------
# delegation to a native ADBC driver


@pytest.mark.skipif(not PG_CONN, reason="needs PG_URI")
def test_execute_partitions_forwards_to_the_native_driver():
    """A delegated statement's ExecutePartitions is the native driver's, not ours.

    adbc_driver_postgresql does not implement partitioned results, so the observable
    behaviour is its refusal arriving as a clean ADBC error.  That it arrives at all is
    the point: the native driver declines the call *without touching the AdbcError it
    was handed*, and a proxy that then asks that driver for the error's details -- as
    ADBC_ERROR_INIT's vendor_code invites it to -- makes the driver dereference state it
    never created, which used to segfault the process here.
    """
    db = adm.AdbcDatabase(driver=DRIVER, entrypoint="AdbcDriverInit", uri=PG_CONN)
    conn = adm.AdbcConnection(db)
    try:
        assert conn.get_option("adbc.odbc.delegated_to") == "postgresql"
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query("SELECT 1")
        with pytest.raises(adm.Error) as excinfo:
            stmt.execute_partitions()
        assert "postgresql" in str(excinfo.value)
        stmt.close()
        # The connection is still usable afterwards -- the refusal was an error, not a
        # broken proxy.
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query("SELECT 1 AS one")
        handle, _ = stmt.execute_query()
        assert to_table(handle).num_rows == 1
        stmt.close()
    finally:
        conn.close()
        db.close()


@pytest.mark.skipif(not PG_CONN, reason="needs PG_URI")
def test_read_partition_forwards_to_the_native_driver():
    """Likewise for the other half of the contract."""
    db = adm.AdbcDatabase(driver=DRIVER, entrypoint="AdbcDriverInit", uri=PG_CONN)
    conn = adm.AdbcConnection(db)
    try:
        with pytest.raises(adm.Error):
            conn.read_partition(b"whatever")
    finally:
        conn.close()
        db.close()


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
