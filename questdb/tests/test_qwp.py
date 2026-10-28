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

"""adbc_driver_questdb against a live QuestDB 10 server, through the ADBC
driver manager.

Environment:
  ADBC_QUESTDB_DRIVER  path to libadbc_driver_questdb.so (default: build-questdb/)
  QUESTDB_URI          ws://host:port (default ws://127.0.0.1:9000)
"""

import datetime
import os
import pathlib
import uuid

import pyarrow as pa
import pytest

import adbc_driver_manager as adm
import adbc_driver_manager.dbapi as dbapi

HERE = pathlib.Path(__file__).resolve().parent
DRIVER = os.environ.get(
    "ADBC_QUESTDB_DRIVER",
    str(HERE.parent.parent / "build-questdb" / "libadbc_driver_questdb.so"),
)
URI = os.environ.get("QUESTDB_URI", "ws://127.0.0.1:9000")


def _connect(**db_kwargs):
    return dbapi.connect(driver=DRIVER, entrypoint="AdbcDriverInit", db_kwargs={"uri": URI, **db_kwargs})


@pytest.fixture(scope="module")
def conn():
    if not os.path.exists(DRIVER):
        pytest.skip(f"driver not built: {DRIVER}")
    try:
        c = _connect()
    except Exception as exc:  # noqa: BLE001
        pytest.skip(f"no QuestDB at {URI}: {exc}")
    yield c
    c.close()


@pytest.fixture
def cur(conn):
    with conn.cursor() as c:
        yield c


def _drop(cur, name):
    cur.execute(f'DROP TABLE IF EXISTS "{name}"')


# --- connection ----------------------------------------------------------------


def test_get_info(conn):
    info = conn.adbc_get_info()
    assert info["vendor_name"] == "QuestDB"
    assert info["vendor_version"].split(".")[0].isdigit()
    assert int(info["vendor_version"].split(".")[0]) >= 10
    assert "QWP" in info["driver_name"]
    assert info["driver_adbc_version"] >= 1001000


def test_bad_uri_fails_at_connect():
    with pytest.raises(adm.Error):
        _connect(uri="ws://127.0.0.1:1")


def test_autocommit_cannot_be_disabled(conn):
    with pytest.raises(adm.NotSupportedError):
        conn.adbc_connection.set_options(**{"adbc.connection.autocommit": "false"})


# --- queries -------------------------------------------------------------------


def test_select_one(cur):
    cur.execute("SELECT 1 AS one")
    tbl = cur.fetch_arrow_table()
    assert tbl.column_names == ["one"]
    assert tbl.column(0).to_pylist() == [1]


def test_many_rows_stream_in_batches(cur):
    cur.execute("SELECT x FROM long_sequence(250000)")
    reader = cur.fetch_record_batch()
    n, batches = 0, 0
    for b in reader:
        n += b.num_rows
        batches += 1
    assert n == 250000
    assert batches > 1


def test_all_scalar_types(cur):
    _drop(cur, "adbc_qwp_types")
    cur.execute(
        "CREATE TABLE adbc_qwp_types ("
        "  b BOOLEAN, y BYTE, s SHORT, c CHAR, i INT, l LONG, f FLOAT, d DOUBLE,"
        "  str STRING, vc VARCHAR, sym SYMBOL, dt DATE, ts TIMESTAMP, u UUID,"
        "  l256 LONG256, ip IPV4, dec DECIMAL(18,3), ts_ns TIMESTAMP_NS, arr DOUBLE[],"
        "  des TIMESTAMP"
        ") TIMESTAMP(des) PARTITION BY DAY WAL"
    )
    cur.execute(
        "INSERT INTO adbc_qwp_types VALUES ("
        "  true, 7, 300, 'q', 70000, 5000000000, 1.5, 2.25,"
        "  'str', 'vc', 'sym', '2024-02-29T00:00:00.000Z', '2024-02-29T08:45:10.123456Z',"
        "  '11111111-2222-3333-4444-555555555555',"
        "  0x7ee65ec7b6e3bc3a422a8855e9d7bfd29199af5c2aa91ba39c022fa261bdede7,"
        "  '192.168.1.20', 1234.567m, '2024-02-29T08:45:10.123456789Z', ARRAY[1.0, 2.5],"
        "  '2024-02-29T08:45:10.000000Z'"
        ")"
    )
    cur.execute("INSERT INTO adbc_qwp_types (des) VALUES ('2024-03-01T00:00:00.000000Z')")
    # WAL visibility: poll briefly.
    for _ in range(300):
        cur.execute("SELECT count() FROM adbc_qwp_types")
        if cur.fetchone()[0] == 2:
            break
        import time

        time.sleep(0.01)
    cur.execute("SELECT * FROM adbc_qwp_types ORDER BY des")
    tbl = cur.fetch_arrow_table()
    assert tbl.num_rows == 2
    row = {name: tbl.column(name)[0].as_py() for name in tbl.column_names}
    nulls = {name: tbl.column(name)[1].as_py() for name in tbl.column_names}
    assert row["b"] is True
    assert row["y"] == 7 and row["s"] == 300 and row["i"] == 70000 and row["l"] == 5000000000
    assert row["f"] == 1.5 and row["d"] == 2.25
    assert row["str"] == "str" and row["vc"] == "vc" and row["sym"] == "sym"
    assert row["c"] == ord("q")  # CHAR arrives as a UTF-16 code unit (uint16)
    assert row["ts"].replace(tzinfo=None) == datetime.datetime(2024, 2, 29, 8, 45, 10, 123456)
    assert row["ts_ns"].to_pydatetime(warn=False).replace(tzinfo=None) == datetime.datetime(2024, 2, 29, 8, 45, 10, 123456)
    assert row["dt"].replace(tzinfo=None) == datetime.datetime(2024, 2, 29)
    assert str(row["u"]) == "11111111-2222-3333-4444-555555555555"
    assert row["ip"] == 0xC0A80114
    assert len(row["l256"]) == 32
    assert row["arr"] == [1.0, 2.5]
    assert float(row["dec"]) == pytest.approx(1234.567)
    # Every nullable column reads back NULL on the second row (QuestDB's BYTE,
    # SHORT, CHAR and BOOLEAN have no NULL and read back as 0 / false).
    assert nulls["y"] == 0 and nulls["s"] == 0 and nulls["b"] is False
    for name in ("i", "l", "f", "d", "str", "vc", "sym", "dt", "ts", "u", "l256", "ip", "dec", "ts_ns", "arr"):
        assert nulls[name] is None, name
    _drop(cur, "adbc_qwp_types")


def test_empty_result(cur):
    cur.execute("SELECT x FROM long_sequence(10) WHERE x > 100")
    tbl = cur.fetch_arrow_table()
    assert tbl.num_rows == 0


def test_sql_error(cur):
    with pytest.raises(adm.Error) as exc:
        cur.execute("SELECT * FROM table_that_does_not_exist_adbc")
    assert "table_that_does_not_exist_adbc" in str(exc.value) or "does not exist" in str(exc.value)


def test_execute_update_rows_affected(cur):
    _drop(cur, "adbc_qwp_upd")
    cur.execute("CREATE TABLE adbc_qwp_upd (id LONG, ts TIMESTAMP) TIMESTAMP(ts) PARTITION BY DAY WAL")
    cur.execute("INSERT INTO adbc_qwp_upd SELECT x, timestamp_sequence(0, 1000000) FROM long_sequence(4)")
    assert cur.rowcount == 4
    cur.adbc_statement.set_sql_query("INSERT INTO adbc_qwp_upd SELECT x, timestamp_sequence(0, 1000000) FROM long_sequence(2)")
    assert cur.adbc_statement.execute_update() == 2
    _drop(cur, "adbc_qwp_upd")


# --- bind parameters -----------------------------------------------------------


def test_bind_scalar_parameters(cur):
    cur.execute("SELECT $1::long * x AS v, $2 AS label FROM long_sequence(3)", parameters=[10, "widgets"])
    tbl = cur.fetch_arrow_table()
    assert tbl.column("v").to_pylist() == [10, 20, 30]
    assert tbl.column("label").to_pylist() == ["widgets"] * 3


def test_bind_typed_parameters(cur):
    params = pa.record_batch(
        [
            pa.array([True]),
            pa.array([7], pa.int32()),
            pa.array([2.5], pa.float64()),
            pa.array([datetime.datetime(2024, 2, 29, 8, 45, 10, 123456)], pa.timestamp("us")),
            pa.array([None], pa.string()),
        ],
        names=["b", "i", "d", "ts", "n"],
    )
    cur.adbc_statement.set_sql_query("SELECT $1 AS b, $2::int AS i, $3 AS d, $4 AS ts, $5 AS n")
    cur.adbc_statement.bind(params)
    stream, _ = cur.adbc_statement.execute_query()
    tbl = pa.RecordBatchReader._import_from_c(stream.address).read_all()
    assert tbl.column("b").to_pylist() == [True]
    assert tbl.column("i").to_pylist() == [7]
    assert tbl.column("d").to_pylist() == [2.5]
    assert tbl.column("ts")[0].as_py().replace(tzinfo=None) == datetime.datetime(2024, 2, 29, 8, 45, 10, 123456)
    assert tbl.column("n").to_pylist() == [None]


def test_executemany_inserts_one_row_per_bound_row(cur):
    _drop(cur, "adbc_qwp_many")
    cur.execute("CREATE TABLE adbc_qwp_many (id LONG, name VARCHAR, ts TIMESTAMP) TIMESTAMP(ts) PARTITION BY DAY WAL")
    rows = [(i, f"n{i}", datetime.datetime(2024, 1, 1, 0, 0, i)) for i in range(5)]
    cur.executemany("INSERT INTO adbc_qwp_many VALUES ($1, $2, $3)", rows)
    import time

    for _ in range(300):
        cur.execute("SELECT count() FROM adbc_qwp_many")
        if cur.fetchone()[0] == 5:
            break
        time.sleep(0.01)
    cur.execute("SELECT id, name FROM adbc_qwp_many ORDER BY id")
    assert cur.fetchall() == [(i, f"n{i}") for i in range(5)]
    _drop(cur, "adbc_qwp_many")


# --- bulk ingest ---------------------------------------------------------------


def _ingest_table(n):
    base = datetime.datetime(2024, 2, 29, 8, 0, 0)
    return pa.table(
        {
            "sym": pa.array([f"s{i % 7}" for i in range(n)]).dictionary_encode(),
            "name": pa.array([f"name-{i}" for i in range(n)]),
            "price": pa.array([i * 0.5 for i in range(n)], pa.float64()),
            "qty": pa.array(list(range(n)), pa.int64()),
            "flag": pa.array([i % 2 == 0 for i in range(n)]),
            "ts": pa.array([base + datetime.timedelta(seconds=i) for i in range(n)], pa.timestamp("us")),
        }
    )


def test_ingest_create_then_append_then_replace(cur):
    _drop(cur, "adbc_qwp_ingest")
    data = _ingest_table(1000)
    assert cur.adbc_ingest("adbc_qwp_ingest", data, mode="create") == 1000
    cur.execute("SELECT count(), min(ts), max(ts), sum(qty) FROM adbc_qwp_ingest")
    count, tmin, tmax, qty = cur.fetchone()
    assert count == 1000
    assert tmin.replace(tzinfo=None) == datetime.datetime(2024, 2, 29, 8, 0, 0)
    assert tmax.replace(tzinfo=None) == datetime.datetime(2024, 2, 29, 8, 16, 39)
    assert qty == sum(range(1000))

    # The designated timestamp came from the single timestamp column.
    cur.execute("SELECT designatedTimestamp FROM tables() WHERE table_name = 'adbc_qwp_ingest'")
    assert cur.fetchone()[0] == "ts"
    cur.execute("SELECT type FROM table_columns('adbc_qwp_ingest') WHERE column = 'sym'")
    assert cur.fetchone()[0] == "SYMBOL"

    with pytest.raises(adm.Error):
        cur.adbc_ingest("adbc_qwp_ingest", data, mode="create")

    assert cur.adbc_ingest("adbc_qwp_ingest", _ingest_table(10), mode="append") == 10
    cur.execute("SELECT count() FROM adbc_qwp_ingest")
    assert cur.fetchone()[0] == 1010

    assert cur.adbc_ingest("adbc_qwp_ingest", _ingest_table(3), mode="replace") == 3
    cur.execute("SELECT count() FROM adbc_qwp_ingest")
    assert cur.fetchone()[0] == 3

    with pytest.raises(adm.Error):
        cur.adbc_ingest("adbc_qwp_missing_table", data, mode="append")
    _drop(cur, "adbc_qwp_ingest")


def test_ingest_explicit_timestamp_column_and_varchar_symbols(cur):
    _drop(cur, "adbc_qwp_ingest2")
    data = pa.table(
        {
            "a": pa.array(["x", "y"]),
            "created": pa.array([datetime.datetime(2024, 1, 1), datetime.datetime(2024, 1, 2)], pa.timestamp("us")),
            "updated": pa.array([datetime.datetime(2024, 1, 3), datetime.datetime(2024, 1, 4)], pa.timestamp("us")),
        }
    )
    cur.adbc_statement.set_options(**{
        "adbc.questdb.ingest.timestamp_column": "updated",
        "adbc.questdb.ingest.symbol_mode": "symbol",
    })
    assert cur.adbc_ingest("adbc_qwp_ingest2", data, mode="create") == 2
    cur.execute("SELECT designatedTimestamp FROM tables() WHERE table_name = 'adbc_qwp_ingest2'")
    assert cur.fetchone()[0] == "updated"
    cur.execute("SELECT type FROM table_columns('adbc_qwp_ingest2') WHERE column = 'a'")
    assert cur.fetchone()[0] == "SYMBOL"
    cur.adbc_statement.set_options(**{
        "adbc.questdb.ingest.timestamp_column": "",
        "adbc.questdb.ingest.symbol_mode": "auto",
    })
    _drop(cur, "adbc_qwp_ingest2")


# --- catalog -------------------------------------------------------------------


def test_catalog(conn, cur):
    _drop(cur, "adbc_qwp_cat")
    cur.execute("CREATE TABLE adbc_qwp_cat (id LONG, name SYMBOL, price DECIMAL(18,3), ts TIMESTAMP) TIMESTAMP(ts) PARTITION BY DAY WAL")

    types = conn.adbc_get_table_types()
    assert "TABLE" in types

    objects = conn.adbc_get_objects(depth="all", table_name_filter="adbc_qwp_cat").read_all().to_pylist()
    assert len(objects) == 1
    assert objects[0]["catalog_name"] == "qdb"
    schemas = objects[0]["catalog_db_schemas"]
    assert schemas[0]["db_schema_name"] == "public"
    tables = schemas[0]["db_schema_tables"]
    assert [t["table_name"] for t in tables] == ["adbc_qwp_cat"]
    assert tables[0]["table_type"] == "TABLE"
    cols = tables[0]["table_columns"]
    assert [c["column_name"] for c in cols] == ["id", "name", "price", "ts"]
    assert [c["ordinal_position"] for c in cols] == [1, 2, 3, 4]
    assert [c["xdbc_type_name"] for c in cols] == ["LONG", "SYMBOL", "DECIMAL(18,3)", "TIMESTAMP"]
    assert cols[2]["xdbc_column_size"] == 18 and cols[2]["xdbc_decimal_digits"] == 3
    assert cols[3]["remarks"] == "designated timestamp"

    only_tables = conn.adbc_get_objects(depth="tables", table_name_filter="adbc_qwp_%").read_all().to_pylist()
    names = [t["table_name"] for t in only_tables[0]["catalog_db_schemas"][0]["db_schema_tables"]]
    assert "adbc_qwp_cat" in names

    schema = conn.adbc_get_table_schema("adbc_qwp_cat")
    assert schema.names == ["id", "name", "price", "ts"]
    assert schema.field("id").type == pa.int64()
    assert pa.types.is_dictionary(schema.field("name").type)
    assert schema.field("price").type == pa.decimal64(18, 3)  # the client reports decimal64 at its full precision
    assert pa.types.is_timestamp(schema.field("ts").type)

    with pytest.raises(adm.Error):
        conn.adbc_get_table_schema("adbc_qwp_no_such_table")
    _drop(cur, "adbc_qwp_cat")


def test_get_table_schema_matches_result_schema(conn, cur):
    _drop(cur, "adbc_qwp_sch")
    cur.execute(
        "CREATE TABLE adbc_qwp_sch (b BOOLEAN, y BYTE, c CHAR, i INT, l LONG, f FLOAT, d DOUBLE, s STRING, v VARCHAR,"
        " sym SYMBOL, dt DATE, ts TIMESTAMP, tn TIMESTAMP_NS, u UUID, l256 LONG256, ip IPV4, dec DECIMAL(10,2),"
        " big DECIMAL(30,4), arr DOUBLE[], arr2 DOUBLE[][], g GEOHASH(8c), bin BINARY) TIMESTAMP(ts) PARTITION BY DAY WAL"
    )
    cur.execute("INSERT INTO adbc_qwp_sch (ts) VALUES ('2024-01-01T00:00:00.000000Z')")
    import time

    for _ in range(300):
        cur.execute("SELECT count() FROM adbc_qwp_sch")
        if cur.fetchone()[0] == 1:
            break
        time.sleep(0.01)
    cur.execute("SELECT * FROM adbc_qwp_sch")
    from_result = cur.fetch_arrow_table().schema
    from_catalog = conn.adbc_get_table_schema("adbc_qwp_sch")
    assert from_result.names == from_catalog.names
    for name in from_result.names:
        a, b = from_result.field(name).type, from_catalog.field(name).type
        if pa.types.is_dictionary(a) or pa.types.is_dictionary(b):
            assert pa.types.is_dictionary(a) and pa.types.is_dictionary(b), name
        elif pa.types.is_list(a) or pa.types.is_list(b):
            # The client emits an all-NULL multi-dimensional array column as a
            # one-level list; the catalog reports the declared dimensions.
            assert pa.types.is_list(a) and pa.types.is_list(b), name
        elif pa.types.is_timestamp(a):
            assert pa.types.is_timestamp(b) and a.unit == b.unit, name
        else:
            assert a == b, name
        assert from_result.field(name).metadata[b"questdb.column_type"] == from_catalog.field(name).metadata[b"questdb.column_type"], name
    _drop(cur, "adbc_qwp_sch")


# --- options -------------------------------------------------------------------


def test_query_timeout_option_roundtrip(cur):
    cur.adbc_statement.set_options(**{"adbc.questdb.query_timeout_ms": 5000})
    assert cur.adbc_statement.get_option_int("adbc.questdb.query_timeout_ms") == 5000
    try:
        cur.execute("SELECT 1")
        assert cur.fetchone() == (1,)
    except adm.NotSupportedError as exc:
        # QuestDB 10.1.0 does not advertise CAP_QUERY_TIMEOUT; the client
        # refuses rather than silently running without the timeout.
        assert "timeout" in str(exc)
    cur.adbc_statement.set_options(**{"adbc.questdb.query_timeout_ms": 0})
    cur.execute("SELECT 1")
    assert cur.fetchone() == (1,)


def test_unknown_option_is_rejected(cur):
    with pytest.raises(adm.NotSupportedError):
        cur.adbc_statement.set_options(**{"adbc.questdb.no_such_option": "1"})
