#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Parallel bulk ingest (``adbc.odbc.ingest_connections``).

The option spreads one ingest over N connections, which makes it N transactions
instead of one.  These tests pin down both halves of that bargain: that the rows
which arrive are exactly the rows a single connection would have written, and
that when something goes wrong the caller is told and the damage is the damage
the option documents -- a subset of the stream, never invented or corrupted rows.

Needs a PostgreSQL server (PG_URI) and psqlodbc (POSTGRES_ODBC_DRIVER); skips
otherwise.  Run under pytest, or directly:

    PG_URI=postgresql://... POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so pytest tests/test_parallel_ingest.py
"""

import os
import pathlib
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

INGEST_CONNECTIONS = "adbc.odbc.ingest_connections"
# Batches are cut into pieces of this many rows before they are queued
# (ADBC_ODBC_INGEST_SLICE_ROWS), so anything longer than this exercises slicing.
SLICE_ROWS = 16384


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


def sql(statement):
    """Run a statement that returns nothing."""
    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(statement)
        stmt.execute_update()
        stmt.close()
    finally:
        conn.close()
        db.close()


def query(statement):
    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query(statement)
        handle, _ = stmt.execute_query()
        table = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
        stmt.close()
        return table
    finally:
        conn.close()
        db.close()


def ingest(source, target, connections, mode="create", autocommit=True):
    """Ingest `source` (anything with __arrow_c_stream__) into `target`."""
    db, conn = connect()
    stmt = None
    try:
        if not autocommit:
            conn.set_autocommit(False)
        stmt = adm.AdbcStatement(conn)
        opts = {
            "adbc.ingest.target_table": target,
            "adbc.ingest.mode": "adbc.ingest.mode." + mode,
        }
        if connections != 1:
            opts[INGEST_CONNECTIONS] = str(connections)
        stmt.set_options(**opts)
        stmt.bind_stream(source.__arrow_c_stream__())
        affected = stmt.execute_update()
        if not autocommit:
            conn.commit()
        return affected
    finally:
        # Close the statement even when the execute raised, or closing the
        # connection raises in its place and hides the error under test.
        if stmt is not None:
            try:
                stmt.close()
            except Exception:
                pass
        conn.close()
        db.close()


def make_table(rows, batch_size=None):
    """Mixed-type rows: int, float, string with nulls, date."""
    tbl = pyarrow.table({
        "id": pyarrow.array(range(rows), pyarrow.int64()),
        "val": pyarrow.array([i * 1.5 for i in range(rows)], pyarrow.float64()),
        "txt": pyarrow.array(
            [None if i % 97 == 0 else "row-%012d" % i for i in range(rows)], pyarrow.string()),
        "dt": pyarrow.array([i % 3000 for i in range(rows)], pyarrow.date32()),
    })
    if batch_size:
        tbl = pyarrow.Table.from_batches(tbl.to_batches(max_chunksize=batch_size), tbl.schema)
    return tbl


def fingerprint(table):
    """An order-independent checksum over every column."""
    txt = table.column("txt")
    return (
        table.num_rows,
        pc.sum(table.column("id")).as_py(),
        round(pc.sum(table.column("val")).as_py(), 3),
        pc.sum(pc.binary_length(txt)).as_py(),
        txt.null_count,
        pc.sum(pc.cast(table.column("dt"), pyarrow.int32())).as_py(),
    )


def read_back(target):
    return query('SELECT "id", "val", "txt", "dt" FROM "%s"' % target)


def drop(*tables):
    for t in tables:
        sql('DROP TABLE IF EXISTS "%s"' % t)


@pytest.fixture
def target():
    name = "t_par_ingest"
    drop(name)
    yield name
    drop(name)


# ---------------------------------------------------------------------------
# The bargain's good half: N connections write what one connection would have


@pytest.mark.parametrize("connections", [2, 4, 8])
def test_matches_the_single_connection_path(connections):
    """Row count and a per-column checksum, against the one-connection ingest."""
    rows = 50_000
    source = make_table(rows, batch_size=7000)
    one, many = "t_par_one", "t_par_many"
    drop(one, many)
    try:
        assert ingest(source, one, 1) == rows
        assert ingest(source, many, connections) == rows
        expected = fingerprint(read_back(one))
        assert expected[0] == rows
        assert fingerprint(read_back(many)) == expected
    finally:
        drop(one, many)


def test_one_huge_batch_is_split_across_workers(target):
    """A stream of a single batch must still reach every worker.

    The driver slices batches longer than ADBC_ODBC_INGEST_SLICE_ROWS, because a
    caller who hands over one big table would otherwise pin the whole ingest to
    one connection.  Slicing re-points column offsets, so it is exactly where a
    wrong row could be read: check every column, not just the count.
    """
    rows = SLICE_ROWS * 4 + 137  # several whole slices and a short last one
    source = make_table(rows).combine_chunks()
    assert len(source.to_batches()) == 1
    assert ingest(source, target, 8) == rows
    assert fingerprint(read_back(target)) == fingerprint(source)


def test_empty_stream(target):
    """No batches at all: an empty table, no error, nothing left running."""
    source = make_table(0)
    assert ingest(source, target, 4) == 0
    assert read_back(target).num_rows == 0


def test_fewer_batches_than_connections(target):
    """Two batches, eight workers: six of them see the end of the queue at once."""
    rows = 100
    source = make_table(rows, batch_size=50)
    assert len(source.to_batches()) == 2
    assert ingest(source, target, 8) == rows
    assert fingerprint(read_back(target)) == fingerprint(source)


def test_single_row_batches(target):
    """Batches too short for the multi-row INSERT still have to arrive."""
    rows = 20
    source = make_table(rows, batch_size=1)
    assert len(source.to_batches()) == rows
    assert ingest(source, target, 4) == rows
    assert fingerprint(read_back(target)) == fingerprint(source)


# ---------------------------------------------------------------------------
# The bargain's price: what a failure leaves behind


def failing_stream(schema, batches, fail_after):
    """A stream that yields `fail_after` batches and then raises."""
    def gen():
        for i, b in enumerate(batches):
            if i == fail_after:
                raise ValueError("injected mid-stream failure")
            yield b
    return pyarrow.RecordBatchReader.from_batches(schema, gen())


def test_failure_mid_stream_is_reported_and_leaves_only_real_rows(target):
    """A stream that breaks half way through.

    The caller must be told, and the table must hold only rows that were really
    in the stream -- a subset, because N connections are N transactions and a
    worker that had already finished has already committed.  That subset is the
    documented price of the option; corrupt or invented rows would not be.
    """
    source = make_table(40_000, batch_size=2000)
    batches = source.to_batches()
    reader = failing_stream(source.schema, batches, fail_after=10)

    with pytest.raises(Exception) as excinfo:
        ingest(reader, target, 4)
    assert "injected mid-stream failure" in str(excinfo.value) or "get_next" in str(excinfo.value)

    # The table exists (the CREATE TABLE committed before any row was sent) and
    # holds a subset of the stream, with every row one that was really sent.
    got = read_back(target)
    assert got.num_rows <= source.num_rows
    sent = set(pyarrow.Table.from_batches(batches[:10]).column("id").to_pylist())
    assert set(got.column("id").to_pylist()) <= sent


def test_constraint_violation_by_one_worker(target):
    """A UNIQUE/NOT NULL constraint one worker's share breaks.

    The failing worker rolls its own share back and trips the queue, so every
    worker still running rolls back too; the caller gets the server's complaint.
    What survives is whatever a worker had already committed -- and, crucially,
    nothing that violates the constraint.
    """
    sql('CREATE TABLE "%s" ("id" bigint PRIMARY KEY, "txt" text NOT NULL)' % target)
    rows = 40_000
    ids = list(range(rows))
    ids[rows - 5] = 0  # a duplicate key, far enough in to land on a later batch
    source = pyarrow.Table.from_batches(
        pyarrow.table({
            "id": pyarrow.array(ids, pyarrow.int64()),
            "txt": pyarrow.array(["r%d" % i for i in range(rows)], pyarrow.string()),
        }).to_batches(max_chunksize=2000))

    with pytest.raises(Exception) as excinfo:
        ingest(source, target, 4, mode="append")
    msg = str(excinfo.value).lower()
    assert "duplicate" in msg or "unique" in msg or "constraint" in msg

    got = read_back_ids(target)
    assert len(got) == len(set(got)), "a committed row broke the primary key"
    assert set(got) <= set(ids)


def test_not_null_violation_by_one_worker(target):
    """The same, for a NOT NULL column."""
    sql('CREATE TABLE "%s" ("id" bigint, "txt" text NOT NULL)' % target)
    rows = 40_000
    txt = ["r%d" % i for i in range(rows)]
    txt[rows - 5] = None
    source = pyarrow.Table.from_batches(
        pyarrow.table({
            "id": pyarrow.array(range(rows), pyarrow.int64()),
            "txt": pyarrow.array(txt, pyarrow.string()),
        }).to_batches(max_chunksize=2000))

    with pytest.raises(Exception) as excinfo:
        ingest(source, target, 4, mode="append")
    msg = str(excinfo.value).lower()
    assert "null" in msg or "constraint" in msg

    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        stmt.set_sql_query('SELECT COUNT(*) FROM "%s" WHERE "txt" IS NULL' % target)
        handle, _ = stmt.execute_query()
        nulls = pyarrow.RecordBatchReader._import_from_c(handle.address).read_all()
        stmt.close()
    finally:
        conn.close()
        db.close()
    assert nulls.column(0)[0].as_py() == 0


def read_back_ids(target):
    return query('SELECT "id" FROM "%s"' % target).column("id").to_pylist()


# ---------------------------------------------------------------------------
# The option itself


def test_defaults_to_one_connection():
    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        assert stmt.get_option_int(INGEST_CONNECTIONS) == 1
        stmt.close()
    finally:
        conn.close()
        db.close()


@pytest.mark.parametrize("bad", ["0", "-1", "65", "many", "4x", ""])
def test_rejects_nonsense(bad):
    db, conn = connect()
    try:
        stmt = adm.AdbcStatement(conn)
        with pytest.raises(Exception):
            stmt.set_options(**{INGEST_CONNECTIONS: bad})
        stmt.close()
    finally:
        conn.close()
        db.close()


def test_falls_back_inside_a_caller_transaction(target):
    """Autocommit off means the CREATE TABLE is uncommitted and invisible to the
    worker connections, so the driver quietly keeps the ingest on one connection.
    The point of the test is that this is still correct and still atomic."""
    rows = 30_000
    source = make_table(rows, batch_size=4000)
    assert ingest(source, target, 8, autocommit=False) == rows
    assert fingerprint(read_back(target)) == fingerprint(source)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
