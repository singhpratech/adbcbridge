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

"""Tests for the adbcbridge Python package, run against SQLite over ODBC.

    pip install -e python
    SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so pytest python/tests

Set ADBC_ODBC_DRIVER too if the driver library is not in <repo>/build.
"""

import os
import pathlib
import subprocess
import sys

import pyarrow
import pytest

import adbcbridge
from adbcbridge import _locate

SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER")

requires_sqlite = pytest.mark.skipif(
    not SQLITE_ODBC, reason="set SQLITE_ODBC_DRIVER to the SQLite ODBC driver"
)


@pytest.fixture(scope="session")
def driver():
    """The adbcbridge library under test; skips the run if there is none."""
    try:
        return adbcbridge.driver_path()
    except adbcbridge.DriverNotFoundError as exc:
        pytest.skip(str(exc))


@pytest.fixture
def uri(tmp_path):
    return "Driver=%s;Database=%s;" % (SQLITE_ODBC, tmp_path / "t.db")


# --- driver_path() -----------------------------------------------------------


def test_driver_path_returns_an_existing_library(driver):
    assert pathlib.Path(driver).is_file()
    assert pathlib.Path(driver).is_absolute()
    assert "adbc_driver_odbc" in pathlib.Path(driver).name


def test_env_var_wins(driver, monkeypatch):
    monkeypatch.setenv(_locate.ENV_VAR, driver)
    assert adbcbridge.driver_path() == str(pathlib.Path(driver).resolve())


def test_env_var_pointing_nowhere_is_an_error(tmp_path, monkeypatch):
    monkeypatch.setenv(_locate.ENV_VAR, str(tmp_path / "nope.so"))
    with pytest.raises(adbcbridge.DriverNotFoundError):
        adbcbridge.driver_path()


def test_manifest_lookup(driver, tmp_path, monkeypatch):
    """A manifest named odbc.toml in $ADBC_DRIVER_PATH resolves to the library."""
    manifest = tmp_path / "odbc.toml"
    manifest.write_text(
        "manifest_version = 1\n"
        "name = 'adbcbridge (ODBC)'\n"
        "[Driver]\n"
        "entrypoint = 'AdbcDriverInit'\n"
        "[Driver.shared]\n"
        "%s = '%s'\n" % (_locate._platform_tuple(), driver),
        encoding="utf-8",
    )
    monkeypatch.delenv(_locate.ENV_VAR, raising=False)
    monkeypatch.setenv("ADBC_DRIVER_PATH", str(tmp_path))
    assert _locate._from_manifest() == pathlib.Path(driver).resolve()


def test_manifest_accepts_the_uname_arch_spelling(driver):
    """linux_amd64 (what CMake writes) and linux_x86_64 both resolve."""
    keys = _locate._platform_keys()
    assert keys[0] == _locate._platform_tuple()
    for key in keys:
        text = "[Driver.shared]\n%s = '%s'\n" % (key, driver)
        assert _locate._manifest_library(text) == driver
    # A bare path instead of a per-platform table is allowed by the spec.
    assert _locate._manifest_library("[Driver]\nshared = '%s'\n" % driver) == driver


def test_manifest_for_another_platform_is_ignored():
    text = (
        "[Driver.shared]\n"
        "totally_madeup = '/nonexistent/libadbc_driver_odbc.so'\n"
        "other_arch = '/nonexistent/libadbc_driver_odbc.so'\n"
    )
    assert _locate._manifest_library(text) is None


def test_the_installed_manifest_template_parses(driver):
    """The real manifest layout (adbc_driver_odbc.toml.in) is understood."""
    template = pathlib.Path(__file__).resolve().parents[2] / "adbc_driver_odbc.toml.in"
    if not template.is_file():
        pytest.skip("not running from a source checkout")
    text = template.read_text(encoding="utf-8")
    text = text.replace("@ADBCBRIDGE_MANIFEST_PLATFORM@", _locate._platform_tuple())
    text = text.replace("@ADBCBRIDGE_MANIFEST_DRIVER_PATH@", driver)
    text = text.replace("@PROJECT_VERSION@", "0.1.0")
    assert _locate._manifest_library(text) == driver


# --- connect() ---------------------------------------------------------------


@requires_sqlite
def test_connect_and_query(driver, uri):
    with adbcbridge.connect(uri=uri, driver_path=driver) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS one, 'héllo 🚀' AS s")
            table = cur.fetch_arrow_table()
    assert isinstance(table, pyarrow.Table)
    assert table.to_pydict() == {"one": [1], "s": ["héllo 🚀"]}


@requires_sqlite
def test_connect_uses_driver_path_by_default(driver, uri):
    """No driver_path= argument: the package finds the library itself."""
    with adbcbridge.connect(uri=uri) as conn:
        assert conn.adbc_get_info()["driver_name"]


@requires_sqlite
def test_roundtrip_with_parameters_and_options(driver, uri, monkeypatch):
    # This test exercises the ODBC path itself (batch_size is an ODBC-path option);
    # keep the SQLite target from being delegated to adbc_driver_sqlite.
    monkeypatch.setenv("ADBC_ODBC_DELEGATE", "never")
    with adbcbridge.connect(
        uri=uri, driver_path=driver, autocommit=True, batch_size=64
    ) as conn:
        with conn.cursor() as cur:
            cur.execute("CREATE TABLE t (i INTEGER, s TEXT)")
            cur.executemany("INSERT INTO t VALUES (?, ?)", [(1, "a"), (2, "b")])
            cur.execute("SELECT s FROM t WHERE i = ?", (2,))
            assert cur.fetch_arrow_table().to_pydict() == {"s": ["b"]}
            # More rows than one batch, to prove batch_size reached the driver.
            cur.execute(
                "CREATE TABLE big AS WITH RECURSIVE c(x) AS "
                "(SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 300) SELECT x FROM c"
            )
            cur.execute("SELECT x FROM big ORDER BY x")
            table = cur.fetch_arrow_table()
    assert table.num_rows == 300
    assert len(table.to_batches()) > 1


@requires_sqlite
def test_ingest_a_pyarrow_table(driver, uri):
    data = pyarrow.table({"i": [1, 2, 3], "s": ["x", "y", "z"]})
    with adbcbridge.connect(uri=uri, driver_path=driver, autocommit=True) as conn:
        with conn.cursor() as cur:
            cur.adbc_ingest("ingested", data, mode="create")
            cur.execute('SELECT count(*) AS n FROM "ingested"')
            assert cur.fetch_arrow_table().to_pydict() == {"n": [3]}


@requires_sqlite
def test_options_are_translated(driver, uri):
    """Bare names get the adbc.odbc. prefix, bools become true/false."""
    with adbcbridge.connect(
        uri=uri, driver_path=driver, decimal_as_string=True
    ) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT 1")
            assert cur.fetch_arrow_table().num_rows == 1
    with pytest.raises(Exception):  # unknown option -> the driver rejects it
        adbcbridge.connect(uri=uri, driver_path=driver, no_such_option=1)


def test_option_key_and_value_mapping():
    assert adbcbridge._option_key("batch_size") == "adbc.odbc.batch_size"
    assert adbcbridge._option_key("adbc.odbc.batch_size") == "adbc.odbc.batch_size"
    assert adbcbridge._option_key("username") == "username"
    assert adbcbridge._option_value(True) == "true"
    assert adbcbridge._option_value(False) == "false"
    assert adbcbridge._option_value(4096) == "4096"


def test_connect_needs_uri_or_dsn():
    with pytest.raises(ValueError):
        adbcbridge.connect()


# --- odbc driver inventory ---------------------------------------------------


def test_odbc_drivers_listing():
    drivers = adbcbridge.odbc_drivers()
    assert isinstance(drivers, list)
    for entry in drivers:
        assert entry.name
        assert isinstance(entry.attributes, dict)
    ini = adbcbridge.odbcinst_ini()
    assert ini is None or ini.is_file()


# --- command line ------------------------------------------------------------


def run_cli(*args, **kwargs):
    return subprocess.run(
        [sys.executable, "-m", "adbcbridge", *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
        **kwargs,
    )


def test_cli_help_and_version():
    assert run_cli("--version").returncode == 0
    out = run_cli("--help").stdout.decode()
    assert "query" in out and "drivers" in out


def test_console_script_is_installed():
    import shutil

    script = shutil.which("adbcbridge")
    if script is None:
        pytest.skip("console script not on PATH (package not installed?)")
    proc = subprocess.run([script, "--version"], stdout=subprocess.PIPE, timeout=120)
    assert proc.returncode == 0
    assert proc.stdout.decode().strip() == "adbcbridge " + adbcbridge.__version__


def test_cli_drivers():
    proc = run_cli("drivers")
    assert proc.returncode in (0, 1)  # 1 == no drivers registered on this box
    if proc.returncode == 0:
        assert proc.stdout.decode().startswith("#")


@requires_sqlite
def test_cli_driver_path(driver):
    env = dict(os.environ, ADBC_ODBC_DRIVER=driver)
    proc = run_cli("driver-path", env=env)
    assert proc.returncode == 0, proc.stderr.decode()
    assert proc.stdout.decode().strip() == str(pathlib.Path(driver).resolve())


@requires_sqlite
def test_cli_query(driver, uri):
    # "rows affected" is an ODBC-path behaviour; pin the CLI to it.
    env = dict(os.environ, ADBC_ODBC_DRIVER=driver, ADBC_ODBC_DELEGATE="never")
    assert run_cli(uri, "x", env=env).returncode != 0  # not a subcommand

    proc = run_cli("query", uri, "CREATE TABLE t (i INTEGER, s TEXT)", env=env)
    assert proc.returncode == 0, proc.stderr.decode()
    assert "rows affected" in proc.stdout.decode()

    proc = run_cli("query", uri, "INSERT INTO t VALUES (7, 'seven')", env=env)
    assert proc.returncode == 0, proc.stderr.decode()

    proc = run_cli("query", uri, "SELECT * FROM t", env=env)
    assert proc.returncode == 0, proc.stderr.decode()
    out = proc.stdout.decode()
    assert "seven" in out and "1 rows x 2 columns" in out

    proc = run_cli("query", uri, "SELECT * FROM t", "--format", "csv", env=env)
    assert proc.stdout.decode().splitlines() == ["\"i\",\"s\"", "7,\"seven\""]

    proc = run_cli("query", uri, "SELECT * FROM t", "--format", "schema", env=env)
    assert "i:" in proc.stdout.decode()

    proc = run_cli("query", uri, "SELECT * FROM t WHERE s = ?", "-p", "seven", env=env)
    assert proc.returncode == 0, proc.stderr.decode()
    assert "1 rows x 2 columns" in proc.stdout.decode()

    proc = run_cli("query", uri, "SELECT * FROM t", "--limit", "0", env=env)
    assert "0 rows x 2 columns" in proc.stdout.decode()


@requires_sqlite
def test_cli_reports_errors_without_a_traceback(driver, uri):
    env = dict(os.environ, ADBC_ODBC_DRIVER=driver)
    proc = run_cli("query", uri, "SELECT * FROM no_such_table", env=env)
    assert proc.returncode == 1
    assert b"Traceback" not in proc.stderr
    assert proc.stderr.decode().startswith("adbcbridge: ")


def test_cli_missing_driver_library(tmp_path, monkeypatch):
    env = dict(os.environ, ADBC_ODBC_DRIVER=str(tmp_path / "nope.so"))
    proc = run_cli("driver-path", env=env)
    assert proc.returncode == 2
    assert b"nope.so" in proc.stderr
