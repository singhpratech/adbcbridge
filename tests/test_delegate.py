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

"""Native delegation: adbcbridge hands the driver over to a native ADBC driver.

Run under pytest, or directly:

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/test_delegate.py

Each test skips when its prerequisites (an ODBC driver, a server, a native ADBC
driver package) are missing.
"""

import os
import pathlib
import shutil
import sys
import statistics
import tempfile
import time
import traceback

import adbc_driver_manager.dbapi as dbapi

try:
    import pytest
except ImportError:  # running as a plain script
    pytest = None

HERE = pathlib.Path(__file__).resolve().parent
# The shared-library suffix the build produced on this platform.
SO = {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / ("libadbc_driver_odbc" + SO))
)
PG_URI = os.environ.get("PG_URI", "postgresql://adbc:adbc@127.0.0.1:15432/adbc")
ROWS = int(os.environ.get("DELEGATE_ROWS", "1000000"))


class Skipped(Exception):
    pass


def skip(reason):
    # pytest.skip() raises a BaseException, which main() below cannot catch, and
    # outside a test pytest turns it into an error: only use it under pytest.
    if pytest is not None and "PYTEST_CURRENT_TEST" in os.environ:
        pytest.skip(reason)
    raise Skipped(reason)


def bridge(uri, **options):
    """Connect through adbcbridge."""
    db_kwargs = {"uri": uri}
    db_kwargs.update(options)
    return dbapi.connect(driver=DRIVER, db_kwargs=db_kwargs)


def driver_name(conn):
    return conn.adbc_get_info().get("driver_name", "")


def delegated_to(conn):
    """Who is serving this connection: "odbc", or the native driver's name."""
    try:
        return conn.adbc_connection.get_option("adbc.odbc.delegated_to")
    except Exception:
        return None


FAKE_NATIVE = os.environ.get(
    "ADBC_FAKE_NATIVE_DRIVER",
    str(HERE.parent / "build" / ("libadbc_fake_native_driver" + SO)),
)


def fake_native(family):
    """A copy of the stand-in native driver, named after a database family.

    It records the URI and the options adbcbridge translated for it, so a test
    can assert on exactly what a real native driver would have been handed.
    """
    if not os.path.exists(FAKE_NATIVE):
        skip(f"{FAKE_NATIVE} is not built (cmake --build build)")
    path = os.path.join(tempfile.mkdtemp(), f"libadbc_driver_fake_{family}{SO}")
    shutil.copy(FAKE_NATIVE, path)
    return path


def fake_delegate(conn_str, family="postgres", query="uri", conn_kwargs=None, **options):
    """Delegate `conn_str` to the stand-in driver and run `query` against it.

    "uri" answers with the URI it was given, "options" with every option.
    """
    db_kwargs = {
        "uri": conn_str,
        "adbc.odbc.delegate": "always",
        "adbc.odbc.delegate.allow_paths": "true",
        "adbc.odbc.delegate.driver": fake_native(family),
    }
    db_kwargs.update(options)
    connection = dbapi.connect(
        driver=DRIVER, db_kwargs=db_kwargs, conn_kwargs=conn_kwargs or {}
    )
    with connection as conn, conn.cursor() as cur:
        cur.execute(query)
        return cur.fetch_arrow_table().to_pydict()["value"]


def fake_uri(conn_str, family="postgres", **options):
    return fake_delegate(conn_str, family=family, **options)[0]


def pg_odbc_connection_string():
    drv = os.environ.get("POSTGRES_ODBC_DRIVER") or os.environ.get("PSQL_ODBC_DRIVER")
    if not drv:
        skip("set POSTGRES_ODBC_DRIVER to the psqlodbc shared object")
    return (
        f"Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;"
    )


def require_postgres():
    try:
        import adbc_driver_postgresql.dbapi as pgn
    except ImportError:
        skip("adbc_driver_postgresql is not installed")
    try:
        with pgn.connect(PG_URI) as conn, conn.cursor() as cur:
            cur.execute("SELECT 1")
            cur.fetchall()
    except Exception as e:
        skip(f"no PostgreSQL server at {PG_URI}: {e}")
    return pgn


def require_sqlite_odbc():
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    try:
        import adbc_driver_sqlite  # noqa: F401
    except ImportError:
        skip("adbc_driver_sqlite is not installed")
    return drv


# --- (a) a native URI is served by the native driver ------------------------


def test_postgres_uri_delegates():
    require_postgres()
    with bridge(PG_URI) as conn:
        name = driver_name(conn)
        assert "PostgreSQL" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres uri, delegate=auto -> {name}")


def test_postgres_delegated_fetch_is_native_speed():
    pgn = require_postgres()
    query = (
        f"SELECT i::int AS id, (i*0.5)::float8 AS val, 'row_'||i AS txt, "
        f"DATE '2024-01-01' + (i % 365) AS dt FROM generate_series(1,{ROWS}) i"
    )

    def timed(connect):
        def once():
            with connect() as conn, conn.cursor() as cur:
                start = time.perf_counter()
                cur.execute(query)
                table = cur.fetch_arrow_table()
                assert table.num_rows == ROWS
                return time.perf_counter() - start

        once()  # warm the server's cache
        return statistics.median(once() for _ in range(3))

    native = timed(lambda: pgn.connect(PG_URI))
    delegated = timed(lambda: bridge(PG_URI))
    odbc_conn = pg_odbc_connection_string()
    over_odbc = timed(lambda: bridge(odbc_conn, **{"adbc.odbc.delegate": "never"}))
    print(
        f"{ROWS} rows: native {native:.2f}s, bridge (delegated) {delegated:.2f}s, "
        f"bridge (ODBC) {over_odbc:.2f}s"
    )
    # Delegation is a hand-over, not a wrapper: the bridge should cost nothing
    # measurable on top of the native driver.
    assert delegated < native * 1.5 + 0.25, (native, delegated)
    assert delegated < over_odbc, (delegated, over_odbc)


# --- (b) an ODBC connection string is recognized too ------------------------


def test_postgres_odbc_connection_string_delegates():
    require_postgres()
    conn_str = pg_odbc_connection_string()
    with bridge(conn_str) as conn:
        name = driver_name(conn)
        assert "PostgreSQL" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres ODBC connection string, delegate=auto -> {name}")


def test_postgres_delegate_never_uses_odbc():
    conn_str = pg_odbc_connection_string()
    with bridge(conn_str, **{"adbc.odbc.delegate": "never"}) as conn:
        name = driver_name(conn)
        assert "ADBC ODBC Driver" in name, name
        assert delegated_to(conn) == "odbc"
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres ODBC connection string, delegate=never -> {name}")


def write_dsn(name, entries):
    """Point unixODBC at a throwaway odbc.ini holding one DSN."""
    ini = os.path.join(tempfile.mkdtemp(), "odbc.ini")
    with open(ini, "w") as f:
        f.write(f"[{name}]\n")
        for key, value in entries.items():
            f.write(f"{key} = {value}\n")
    return ini


class odbcini:
    """Context manager for ODBCINI (unixODBC's user odbc.ini)."""

    def __init__(self, path):
        self.path = path

    def __enter__(self):
        self.old = os.environ.get("ODBCINI")
        os.environ["ODBCINI"] = self.path

    def __exit__(self, *exc):
        if self.old is None:
            del os.environ["ODBCINI"]
        else:
            os.environ["ODBCINI"] = self.old


def test_postgres_dsn_delegates():
    require_postgres()
    drv = os.environ.get("POSTGRES_ODBC_DRIVER") or os.environ.get("PSQL_ODBC_DRIVER")
    if not drv:
        skip("set POSTGRES_ODBC_DRIVER to the psqlodbc shared object")
    ini = write_dsn(
        "adbc_pg",
        {
            "Driver": drv,
            "Servername": "127.0.0.1",
            "Port": "15432",
            "Database": "adbc",
            "Username": "adbc",
            "Password": "adbc",
        },
    )
    with odbcini(ini):
        # The native URI is rebuilt out of the odbc.ini section.
        with dbapi.connect(driver=DRIVER, db_kwargs={"dsn": "adbc_pg"}) as conn:
            name = driver_name(conn)
            assert "PostgreSQL" in name and "ODBC" not in name, name
            with conn.cursor() as cur:
                cur.execute("SELECT 1 AS one")
                assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"postgres DSN, delegate=auto -> {name}")


# --- (c) SQLite ------------------------------------------------------------


def test_sqlite_file_delegates():
    sqlite_odbc = require_sqlite_odbc()
    path = os.path.join(tempfile.mkdtemp(), "delegate.db")

    conn_str = f"Driver={sqlite_odbc};Database={path};"
    with bridge(conn_str) as conn:
        name = driver_name(conn)
        assert "SQLite" in name and "ODBC" not in name, name
        with conn.cursor() as cur:
            cur.execute("CREATE TABLE t (i INTEGER)")
            cur.execute("INSERT INTO t VALUES (42)")
        conn.commit()
    print(f"sqlite ODBC connection string, delegate=auto -> {name}")

    # The same file, read back over ODBC: delegation is transparent.
    with bridge(conn_str, **{"adbc.odbc.delegate": "never"}) as conn:
        assert delegated_to(conn) == "odbc"
        with conn.cursor() as cur:
            cur.execute("SELECT i FROM t")
            assert cur.fetch_arrow_table().to_pydict() == {"i": [42]}

    # A native sqlite: URI works as well.
    with bridge(f"sqlite:{path}") as conn:
        assert "SQLite" in driver_name(conn)
        with conn.cursor() as cur:
            cur.execute("SELECT i FROM t")
            assert cur.fetch_arrow_table().to_pydict() == {"i": [42]}


# --- (d) no native driver for this target -> ODBC ---------------------------


def test_dsn_without_native_driver_falls_back_to_odbc():
    drv = os.environ.get("MARIADB_ODBC_DRIVER")
    if not drv:
        skip("set MARIADB_ODBC_DRIVER to the MariaDB Connector/ODBC shared object")
    ini = write_dsn(
        "adbc_mariadb",
        {
            "Driver": drv,
            "Server": "127.0.0.1",
            "Port": "13306",
            "Database": "adbc",
            "User": "adbc",
            "Password": "adbc",
        },
    )
    with odbcini(ini):
        with dbapi.connect(driver=DRIVER, db_kwargs={"dsn": "adbc_mariadb"}) as conn:
            name = driver_name(conn)
            assert "ADBC ODBC Driver" in name, name
            assert delegated_to(conn) == "odbc"
            last_error = conn.adbc_database.get_option("adbc.odbc.delegate.last_error")
            assert "no native ADBC driver" in last_error, last_error
            with conn.cursor() as cur:
                cur.execute("SELECT 1 AS one")
                assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"MariaDB DSN, delegate=auto -> {name} ({last_error})")


# --- policy ----------------------------------------------------------------


def test_delegate_always_errors_without_native_driver():
    try:
        with bridge("Driver=NoSuchDriver;Database=x;", **{"adbc.odbc.delegate": "always"}):
            raise AssertionError("expected delegation to fail")
    except dbapi.Error as e:
        assert "always" in str(e), e
        assert "no native ADBC driver" in str(e), e
    print("delegate=always without a native driver -> error")


def test_env_var_disables_delegation():
    sqlite_odbc = require_sqlite_odbc()
    path = os.path.join(tempfile.mkdtemp(), "env.db")
    old = os.environ.get("ADBC_ODBC_DELEGATE")
    os.environ["ADBC_ODBC_DELEGATE"] = "never"
    try:
        with bridge(f"Driver={sqlite_odbc};Database={path};") as conn:
            assert delegated_to(conn) == "odbc"
            assert "ADBC ODBC Driver" in driver_name(conn)
    finally:
        if old is None:
            del os.environ["ADBC_ODBC_DELEGATE"]
        else:
            os.environ["ADBC_ODBC_DELEGATE"] = old
    print("ADBC_ODBC_DELEGATE=never -> ODBC")


def test_forced_driver_that_cannot_be_loaded_falls_back():
    sqlite_odbc = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "fallback.db")
    with bridge(
        f"Driver={sqlite_odbc};Database={path};",
        **{
            "adbc.odbc.delegate.allow_paths": "true",
            "adbc.odbc.delegate.driver": "/nonexistent/libadbc_driver_sqlite.so",
        },
    ) as conn:
        assert delegated_to(conn) == "odbc"
        last_error = conn.adbc_database.get_option("adbc.odbc.delegate.last_error")
        assert last_error, "expected a diagnostic"
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one")
            assert cur.fetch_arrow_table().to_pydict() == {"one": [1]}
    print(f"unloadable adbc.odbc.delegate.driver -> ODBC ({last_error})")



# --- security: what reaches the native driver -------------------------------


def test_tls_keywords_are_forwarded_not_dropped():
    """psqlodbc's TLS settings must survive the rebuild into a libpq URI."""
    uri = fake_uri(
        "Driver=psqlodbcw.so;Server=db.internal;Database=prod;Uid=app;Pwd=s3cret;"
        "SSLmode=verify-full;sslrootcert=/etc/pki/ca.pem;"
    )
    assert uri.startswith("postgresql://app:s3cret@db.internal/prod?"), uri
    assert "sslmode=verify-full" in uri, uri
    assert "sslrootcert=%2Fetc%2Fpki%2Fca.pem" in uri, uri
    print(f"TLS keywords forwarded -> {uri}")


def test_pqopt_block_is_forwarded():
    uri = fake_uri(
        "Driver=psqlodbcw.so;Server=h;Database=d;Uid=u;Pwd=p;"
        "pqopt={sslrootcert=/etc/ca.pem sslmode=require};"
    )
    assert "sslmode=require" in uri and "sslrootcert=%2Fetc%2Fca.pem" in uri, uri


def test_unrepresentable_keyword_stops_delegation():
    """A keyword we cannot translate must not be dropped behind the user's back."""
    try:
        fake_uri("Driver=psqlodbcw.so;Server=h;Database=d;ReadOnly=1;")
        raise AssertionError("expected delegation to be refused")
    except dbapi.Error as e:
        assert "ReadOnly" in str(e), e
    # ... and in auto mode that means the ODBC driver keeps serving it.
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "fk.db")
    with bridge(f"Driver={drv};Database={path};FKSupport=True;") as conn:
        assert delegated_to(conn) == "odbc"
        last_error = conn.adbc_database.get_option("adbc.odbc.delegate.last_error")
        assert "FKSupport" in last_error, last_error
    print(f"unrepresentable keyword -> ODBC ({last_error})")


def test_connection_string_values_are_uri_escaped():
    """A tenant-controlled Database= must not become libpq query parameters."""
    uri = fake_uri(
        "Driver=psqlodbcw.so;Server=db;Uid=svc;Pwd=S;"
        "Database=x?host=attacker.example&sslmode=disable;"
    )
    assert uri == "postgresql://svc:S@db/x%3Fhost%3Dattacker.example%26sslmode%3Ddisable", uri


def test_unix_socket_and_ipv6_hosts():
    uri = fake_uri("Driver=psqlodbcw.so;Server=/var/run/postgresql;Port=5432;Database=adbc;Uid=adbc;")
    assert uri == "postgresql://adbc@/adbc?host=%2Fvar%2Frun%2Fpostgresql&port=5432", uri
    uri = fake_uri("Driver=psqlodbcw.so;Server=::1;Port=5432;Database=adbc;")
    assert uri == "postgresql://[::1]:5432/adbc", uri


def test_brace_escape_in_connection_string():
    """`}}` is a literal '}' per the ODBC grammar, not the end of the value."""
    uri = fake_uri("Driver=libsqlite3odbc.so;Database={/tmp/a}}b/x.db};", family="sqlite")
    assert uri == "/tmp/a}b/x.db", uri
    uri = fake_uri("Driver=psqlodbcw.so;Server=h;Database=d;Uid=u;Pwd={p}}w};")
    assert uri == "postgresql://u:p%7Dw@h/d", uri


def test_delegate_driver_paths_are_opt_in():
    """A caller-supplied option must not be able to dlopen() anything it likes."""
    evil = os.path.join(tempfile.mkdtemp(), "libadbc_driver_postgresql.so")
    open(evil, "wb").close()
    try:
        bridge("postgresql://localhost/db", **{"adbc.odbc.delegate.driver": evil})
        raise AssertionError("expected the path to be refused")
    except dbapi.Error as e:
        assert "allow_paths" in str(e), e
    try:
        bridge(
            "Driver=psqlodbcw.so;Server=h;Database=d;",
            **{"adbc.odbc.delegate.search_path": os.path.dirname(evil)},
        )
        raise AssertionError("expected the search path to be refused")
    except dbapi.Error as e:
        assert "allow_paths" in str(e), e
    print("adbc.odbc.delegate.driver=/path -> refused unless allow_paths")


def test_nested_delegation_to_the_bridge_is_refused():
    """adbcbridge must not delegate to itself (it used to corrupt the table)."""
    try:
        bridge(
            "postgresql://127.0.0.1:1/db",
            **{
                "adbc.odbc.delegate": "always",
                "adbc.odbc.delegate.allow_paths": "true",
                "adbc.odbc.delegate.driver": DRIVER,
            },
        )
        raise AssertionError("expected self-delegation to be refused")
    except dbapi.Error as e:
        assert "itself" in str(e), e
    print("delegating to adbcbridge itself -> refused")


# --- diagnostics ------------------------------------------------------------


def test_native_uri_with_username_reaches_the_native_driver():
    uri = fake_uri("postgresql://h/db", username="u", password="p")
    assert uri == "postgresql://h/db?user=u&password=p", uri
    # A file-backed database has nowhere to put credentials: say so, do not fall
    # back to ODBC with a URI ODBC cannot parse.
    try:
        fake_uri("sqlite:/tmp/x.db", family="sqlite", username="u")
        raise AssertionError("expected an error")
    except dbapi.Error as e:
        assert "username" in str(e), e


def test_native_connection_error_is_not_masked_by_odbc():
    """A native URI the native driver rejected must report *its* error."""
    require_postgres()
    try:
        bridge("postgresql://adbc:adbc@127.0.0.1:1/adbc")
        raise AssertionError("expected a connection error")
    except dbapi.Error as e:
        message = str(e)
        assert "IM002" not in message, message
        assert "onnect" in message or "refused" in message, message
    print("native connection failure surfaces the native error")


def test_native_uri_over_odbc_is_translated_or_explained():
    """delegate=never with a native URI: ODBC cannot parse it as-is."""
    try:
        with bridge("postgresql://127.0.0.1:15432/adbc", **{"adbc.odbc.delegate": "never"}):
            pass
    except dbapi.Error as e:
        message = str(e)
        assert "IM002" not in message, message
        assert "native ADBC URI" in message or "postgres" in message.lower(), message
        print(f"native URI on the ODBC path -> {message.splitlines()[0]}")


def test_options_after_init_are_rejected():
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "frozen.db")
    with bridge(f"Driver={drv};Database={path};", **{"adbc.odbc.delegate": "never"}) as conn:
        try:
            conn.adbc_database.set_options(**{"adbc.odbc.delegate": "always"})
            raise AssertionError("expected INVALID_STATE")
        except dbapi.Error as e:
            assert "after AdbcDatabaseInit" in str(e), e
    print("adbc.odbc.delegate after init -> INVALID_STATE")


def test_unknown_adbc_option_is_reported_not_dropped():
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "opt.db")
    # delegate=never: nothing can ever consume it.
    try:
        bridge(
            f"Driver={drv};Database={path};",
            **{"adbc.odbc.delegate": "never", "adbc.nonesuch": "1"},
        )
        raise AssertionError("expected NOT_IMPLEMENTED")
    except dbapi.Error as e:
        assert "adbc.nonesuch" in str(e), e
    # auto, but nothing to delegate to: reported at init rather than dropped.
    mariadb = os.environ.get("MARIADB_ODBC_DRIVER")
    if mariadb:
        try:
            bridge(
                f"Driver={mariadb};Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;",
                **{"adbc.nonesuch": "1"},
            )
            raise AssertionError("expected NOT_IMPLEMENTED")
        except dbapi.Error as e:
            assert "adbc.nonesuch" in str(e), e
    print("unknown adbc.* option -> NOT_IMPLEMENTED")


def test_pass_through_option_reaches_the_native_driver():
    options = fake_delegate(
        "Driver=psqlodbcw.so;Server=h;Database=d;",
        query="options",
        **{"adbc.fake.custom": "42"},
    )
    assert "adbc.fake.custom=42" in options, options


def test_connection_options_set_before_init_reach_the_native_driver():
    """conn_kwargs are applied before AdbcConnectionInit, delegated or not."""
    options = fake_delegate(
        "Driver=psqlodbcw.so;Server=h;Database=d;",
        query="options",
        conn_kwargs={"adbc.connection.autocommit": "true"},
    )
    assert "conn:adbc.connection.autocommit=true" in options, options


def test_native_only_connection_option_reaches_the_native_driver():
    """An option ODBC knows nothing about is held until the connection is init'd.

    conn_kwargs are applied before AdbcConnectionInit, where the connection does
    not yet know whether ODBC or a native driver will serve it, so an option like
    Flight SQL's adbc.flight.sql.rpc.call_header.* has to be kept, not refused.
    """
    options = fake_delegate(
        "Driver=psqlodbcw.so;Server=h;Database=d;",
        query="options",
        conn_kwargs={"adbc.fake.x": "1"},
    )
    assert "conn:adbc.fake.x=1" in options, options


def test_typed_connection_options_set_before_init_reach_the_native_driver():
    """The 1.1.0 typed setters hold their options too, and keep the type."""
    options = fake_delegate(
        "Driver=psqlodbcw.so;Server=h;Database=d;",
        query="options",
        conn_kwargs={
            "adbc.fake.count": 7,
            "adbc.fake.seconds": 2.5,
            "adbc.fake.blob": b"\x01\x02\x03",
        },
    )
    assert "conn:adbc.fake.count=int:7" in options, options
    assert "conn:adbc.fake.seconds=double:2.5" in options, options
    assert "conn:adbc.fake.blob=bytes:010203" in options, options


def test_held_connection_option_on_the_odbc_path_is_reported():
    """The same option on a connection ODBC ends up serving is still an error.

    It is raised by AdbcConnectionInit rather than by the setter, which is the
    first moment the connection knows nobody else will take the option.
    """
    drv = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")
    path = os.path.join(tempfile.mkdtemp(), "held.db")
    try:
        dbapi.connect(
            driver=DRIVER,
            db_kwargs={
                "uri": f"Driver={drv};Database={path};",
                "adbc.odbc.delegate": "never",
            },
            conn_kwargs={"adbc.nonesuch": "1"},
        )
        raise AssertionError("expected NOT_IMPLEMENTED")
    except dbapi.Error as e:
        assert "adbc.nonesuch" in str(e), e
        assert "native ADBC driver" in str(e), e
    print("held connection option on the ODBC path -> NOT_IMPLEMENTED")


def test_delegated_to_names_the_native_driver():
    require_postgres()
    with bridge(PG_URI) as conn:
        assert delegated_to(conn) == "postgresql", delegated_to(conn)
        assert conn.adbc_database.get_option("adbc.odbc.delegated_to") == "postgresql"
    print("adbc.odbc.delegated_to -> postgresql")


def test_delegated_connection_keeps_the_native_feature_surface():
    """The bridge forwards; it does not shrink what the native driver can do."""
    require_postgres()
    with bridge(PG_URI) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT $1::int AS a", parameters=(1,))
            assert cur.fetch_arrow_table().to_pydict() == {"a": [1]}
        with conn.cursor() as cur:
            # StatementGetParameterSchema, forwarded to the native driver.
            cur.adbc_prepare("SELECT $1::int")
            assert cur.adbc_prepare("SELECT $1::int") is not None
        assert conn.adbc_get_table_types()


def main():
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"PASS {name}")
        except Skipped as e:
            print(f"SKIP {name}: {e}")
        except Exception:
            failures += 1
            print(f"FAIL {name}")
            traceback.print_exc()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
