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
"""PostgreSQL through psqlodbc: temporal precision, zones, the current schema and
schema-level GetObjects.

These pin the behaviour the ADBC Driver Foundry validation suite measures on
PostgreSQL (tests/validation/RESULTS.md, findings P1-P4, P7 and P8), so that a
change to the reader, the ingest DDL or the psqlodbc quirk block cannot quietly
undo it:

  * a column declared TIMESTAMP(0) / TIMESTAMPTZ(0) reads as timestamp[s];
    TIMESTAMPTZ(3) as timestamp[ms, UTC]; TIME(0) as time32[s], TIME(3) as
    time32[ms] -- through a query, ExecuteSchema and GetTableSchema alike;
  * bulk ingest creates the column the Arrow type says: a zone-less timestamp
    becomes TIMESTAMP, a zoned one TIMESTAMP WITH TIME ZONE, and both keep the
    unit's precision, as does TIME;
  * adbc.connection.db_schema answers current_schema();
  * GetObjects at depth "db_schemas" names the catalog on every schema.

Needs a PostgreSQL server (PG_URI) and psqlodbc (POSTGRES_ODBC_DRIVER); skips
otherwise.  Run under pytest:

    PG_URI=postgresql://... POSTGRES_ODBC_DRIVER=/path/psqlodbcw.so \\
    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so pytest tests/test_postgres.py
"""

import datetime
import os
import pathlib
import urllib.parse

import pytest

import pyarrow as pa
from adbc_driver_manager import dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get("ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so"))
PSQLODBC = os.environ.get("POSTGRES_ODBC_DRIVER")
PG_CONN = os.environ.get("PG_URI")

pytestmark = pytest.mark.skipif(
    not (PSQLODBC and PG_CONN and os.path.exists(DRIVER)),
    reason="needs PG_URI, POSTGRES_ODBC_DRIVER and a built adbcbridge",
)

UTC = datetime.timezone.utc


def connstr():
    u = urllib.parse.urlparse(PG_CONN)
    return "DRIVER=%s;SERVER=%s;PORT=%d;DATABASE=%s;UID=%s;PWD=%s;" % (
        PSQLODBC, u.hostname or "127.0.0.1", u.port or 5432,
        (u.path or "/postgres").lstrip("/"), u.username or "", u.password or "",
    )


@pytest.fixture
def conn():
    with dbapi.connect(
        driver=DRIVER, entrypoint="AdbcDriverInit",
        db_kwargs={"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never"},
        autocommit=True,
    ) as c:
        yield c


def query_schema(conn, sql):
    with conn.cursor() as cur:
        cur.execute(sql)
        return cur.fetch_arrow_table().schema


def execute_schema(conn, sql):
    with conn.cursor() as cur:
        return cur.adbc_execute_schema(sql)


def column_types(conn, table):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT column_name, data_type, datetime_precision FROM information_schema.columns"
            " WHERE table_schema = current_schema() AND table_name = %s ORDER BY ordinal_position"
            % ("'%s'" % table)
        )
        return {name: (typ, prec) for name, typ, prec in cur.fetchall()}


# --- reading declared precision (P1, P2, P4) -------------------------------------------

DECLARED = [
    # (SQL type, Arrow type the column reads as)
    ("TIMESTAMP(0)", pa.timestamp("s")),
    ("TIMESTAMP(3)", pa.timestamp("ms")),
    ("TIMESTAMP(6)", pa.timestamp("us")),
    ("TIMESTAMP", pa.timestamp("us")),
    ("TIMESTAMP(0) WITH TIME ZONE", pa.timestamp("s", tz="UTC")),
    ("TIMESTAMP(1) WITH TIME ZONE", pa.timestamp("ms", tz="UTC")),
    ("TIMESTAMP(3) WITH TIME ZONE", pa.timestamp("ms", tz="UTC")),
    ("TIMESTAMP(6) WITH TIME ZONE", pa.timestamp("us", tz="UTC")),
    ("TIMESTAMP WITH TIME ZONE", pa.timestamp("us", tz="UTC")),
    ("TIME(0)", pa.time32("s")),
    ("TIME(3)", pa.time32("ms")),
    ("TIME(6)", pa.time64("us")),
    ("TIME", pa.time64("us")),
]


@pytest.mark.parametrize("sql_type,arrow_type", DECLARED, ids=[d[0] for d in DECLARED])
def test_declared_precision(conn, sql_type, arrow_type):
    table = "adbc_precision"
    with conn.cursor() as cur:
        cur.execute("DROP TABLE IF EXISTS %s" % table)
        cur.execute("CREATE TABLE %s (v %s)" % (table, sql_type))
        cur.execute("INSERT INTO %s VALUES ('2024-02-29 13:45:10.123456'::%s)"
                    % (table, sql_type.split(" WITH")[0].split("(")[0]
                       + (" WITH TIME ZONE" if "ZONE" in sql_type else "")))
    sql = "SELECT v FROM %s" % table
    assert query_schema(conn, sql).field("v").type == arrow_type
    assert execute_schema(conn, sql).field("v").type == arrow_type
    assert conn.adbc_get_table_schema(table).field("v").type == arrow_type
    # And the value is the declared precision's: PostgreSQL rounds the literal to the
    # column's digits on the way in, and the Arrow unit hands that back unchanged.
    with conn.cursor() as cur:
        cur.execute(sql)
        (v,) = cur.fetchone()
    digits = int(sql_type.split("(")[1].split(")")[0]) if "(" in sql_type else 6
    micros = round(123456 / 10 ** (6 - digits)) * 10 ** (6 - digits)
    if pa.types.is_time(arrow_type):
        expect = datetime.time(13, 45, 10, micros)
    else:
        expect = datetime.datetime(2024, 2, 29, 13, 45, 10, micros)
    assert v.replace(tzinfo=None) == expect


# --- ingest DDL (P3, P4) ------------------------------------------------------------------

def test_ingest_creates_declared_types(conn):
    ts = datetime.datetime(2024, 2, 29, 13, 45, 10, 123456)
    t = pa.table({
        "ts_s": pa.array([ts.replace(microsecond=0)], pa.timestamp("s")),
        "ts_ms": pa.array([ts.replace(microsecond=123000)], pa.timestamp("ms")),
        "ts_us": pa.array([ts], pa.timestamp("us")),
        "tz_s": pa.array([ts.replace(microsecond=0, tzinfo=UTC)], pa.timestamp("s", tz="UTC")),
        "tz_ms": pa.array([ts.replace(microsecond=123000, tzinfo=UTC)], pa.timestamp("ms", tz="UTC")),
        "tz_us": pa.array([ts.replace(tzinfo=UTC)], pa.timestamp("us", tz="UTC")),
        "t_s": pa.array([datetime.time(13, 45, 10)], pa.time32("s")),
        "t_ms": pa.array([datetime.time(13, 45, 10, 123000)], pa.time32("ms")),
        "t_us": pa.array([datetime.time(13, 45, 10, 123456)], pa.time64("us")),
    })
    with conn.cursor() as cur:
        cur.execute("DROP TABLE IF EXISTS adbc_ingest_types")
        cur.adbc_ingest("adbc_ingest_types", t, mode="create")
    assert column_types(conn, "adbc_ingest_types") == {
        "ts_s": ("timestamp without time zone", 0),
        "ts_ms": ("timestamp without time zone", 3),
        "ts_us": ("timestamp without time zone", 6),
        "tz_s": ("timestamp with time zone", 0),
        "tz_ms": ("timestamp with time zone", 3),
        "tz_us": ("timestamp with time zone", 6),
        "t_s": ("time without time zone", 0),
        "t_ms": ("time without time zone", 3),
        "t_us": ("time without time zone", 6),
    }
    # Round trip: the same schema and the same values come back.
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM adbc_ingest_types")
        back = cur.fetch_arrow_table()
    assert back.schema == t.schema
    assert back.to_pylist() == t.to_pylist()


# --- connection options and GetObjects (P7, P8) ---------------------------------------------

def test_current_db_schema(conn):
    assert conn.adbc_current_db_schema == "public"
    with conn.cursor() as cur:
        cur.execute("SET search_path TO pg_catalog")
    assert conn.adbc_current_db_schema == "pg_catalog"


def test_get_objects_db_schemas_name_the_catalog(conn):
    catalog = conn.adbc_current_catalog
    assert catalog
    objs = conn.adbc_get_objects(depth="db_schemas").read_all().to_pylist()
    pairs = [(o["catalog_name"], s["db_schema_name"])
             for o in objs for s in (o["catalog_db_schemas"] or [])]
    assert (catalog, "public") in pairs
    assert all(c == catalog for c, _ in pairs), pairs


# --- the session time zone (adbc.odbc.utc_session) -------------------------------------------

@pytest.fixture
def new_york_database():
    """Make the database default to a non-UTC zone for the duration of the test.

    A session picks the setting up at connect, so every connection opened inside
    the test sees America/New_York; the fixture's own connection resets it after.
    """
    with dbapi.connect(
        driver=DRIVER, entrypoint="AdbcDriverInit",
        db_kwargs={"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never"},
        autocommit=True,
    ) as admin:
        with admin.cursor() as cur:
            cur.execute("ALTER DATABASE %s SET timezone TO 'America/New_York'"
                        % urllib.parse.urlparse(PG_CONN).path.lstrip("/"))
        try:
            yield
        finally:
            with admin.cursor() as cur:
                cur.execute("ALTER DATABASE %s RESET timezone"
                            % urllib.parse.urlparse(PG_CONN).path.lstrip("/"))


def _connect(utc_session=None):
    kwargs = {"adbc.odbc.connection_string": connstr(), "adbc.odbc.delegate": "never"}
    if utc_session is not None:
        kwargs["adbc.odbc.utc_session"] = "true" if utc_session else "false"
    return dbapi.connect(driver=DRIVER, entrypoint="AdbcDriverInit", db_kwargs=kwargs, autocommit=True)


def test_zoned_values_are_instants_on_a_non_utc_server(new_york_database):
    """psqlodbc hands timestamptz over as session wall-clock time without its offset;
    the driver puts the session on UTC so reads and ingest keep the instant."""
    instant = datetime.datetime(2024, 2, 29, 13, 45, 10, 123000, tzinfo=UTC)
    with _connect() as conn:
        with conn.cursor() as cur:
            cur.execute("SHOW TimeZone")
            assert cur.fetchone()[0] == "UTC"
            cur.execute("DROP TABLE IF EXISTS adbc_tz")
            cur.execute("CREATE TABLE adbc_tz (id int, t timestamptz(3), t0 timestamptz(0))")
            cur.execute("INSERT INTO adbc_tz VALUES (1, '2024-02-29 13:45:10.123+00', '2024-02-29 13:45:10+00')")
            cur.execute("SELECT t, t0 FROM adbc_tz")
            (t, t0) = cur.fetchone()
            assert t == instant
            assert t0 == instant.replace(microsecond=0)
            # Ingest a zoned value and check the instant the server holds.
            cur.adbc_ingest("adbc_tz_in", pa.table({"t": pa.array([instant], pa.timestamp("ms", tz="UTC"))}), mode="replace")
            cur.execute("SELECT t AT TIME ZONE 'UTC' FROM adbc_tz_in")
            assert cur.fetchone()[0] == instant.replace(tzinfo=None)
            cur.execute("DROP TABLE adbc_tz")
            cur.execute("DROP TABLE adbc_tz_in")


def test_utc_session_can_be_switched_off(new_york_database):
    with _connect(utc_session=False) as conn:
        with conn.cursor() as cur:
            cur.execute("SHOW TimeZone")
            assert cur.fetchone()[0] == "America/New_York"
    assert dbapi.connect(driver=DRIVER, entrypoint="AdbcDriverInit",
                         db_kwargs={"adbc.odbc.connection_string": connstr(),
                                    "adbc.odbc.delegate": "never",
                                    "adbc.odbc.utc_session": "false"}).adbc_database.get_option("adbc.odbc.utc_session") == "false"
