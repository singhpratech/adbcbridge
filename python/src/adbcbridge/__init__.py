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

"""adbcbridge: use any ODBC data source as an ADBC (Arrow) database.

    import adbcbridge

    with adbcbridge.connect(uri="Driver=SQLite3;Database=my.db;") as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM t")
            table = cur.fetch_arrow_table()

This package is a thin convenience layer: it finds the adbcbridge shared
library (:func:`driver_path`) and hands it to ``adbc_driver_manager``.  The
object it returns is a plain ``adbc_driver_manager.dbapi.Connection``.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Dict, Optional

from . import _preload
from ._locate import (
    DriverNotFoundError,
    OdbcDriver,
    driver_path,
    odbc_driver_library,
    odbc_drivers,
    odbcinst_ini,
)
from ._preload import preload_odbc_driver

if TYPE_CHECKING:  # pragma: no cover - typing only
    from adbc_driver_manager import dbapi as _dbapi

__all__ = [
    "DriverNotFoundError",
    "OdbcDriver",
    "__version__",
    "connect",
    "driver_path",
    "odbc_driver_library",
    "odbc_drivers",
    "odbcinst_ini",
    "preload_odbc_driver",
]

__version__ = "0.1.0"

# Local alias: the `driver_path` parameter of connect() shadows the function.
_find_driver = driver_path


def _dbapi_module():
    """Import ``adbc_driver_manager.dbapi`` -- and, with it, pyarrow.

    Deferred rather than done at import time so that ``import adbcbridge`` does
    not pull pyarrow into the process: an ODBC driver that needs static
    thread-local storage can then still be loaded first (see ``_preload``).
    """
    from adbc_driver_manager import dbapi

    return dbapi


#: Prefix for this driver's own options, e.g. ``adbc.odbc.batch_size``.
OPTION_PREFIX = "adbc.odbc."

# Options the driver takes verbatim (they are ADBC-standard names, not ours).
_PLAIN_OPTIONS = frozenset({"uri", "dsn", "username", "password"})


def _option_key(name: str) -> str:
    """``batch_size`` -> ``adbc.odbc.batch_size``; dotted names pass through."""
    if "." in name or name in _PLAIN_OPTIONS:
        return name
    return OPTION_PREFIX + name


def _option_value(value: Any) -> str:
    if isinstance(value, bool):  # the driver parses "true"/"false"
        return "true" if value else "false"
    return str(value)


def connect(
    uri: Optional[str] = None,
    dsn: Optional[str] = None,
    username: Optional[str] = None,
    password: Optional[str] = None,
    driver_path: Optional[str] = None,  # noqa: A002 - matches the documented API
    *,
    autocommit: bool = False,
    conn_kwargs: Optional[Dict[str, str]] = None,
    **options: Any,
) -> _dbapi.Connection:
    """Connect to an ODBC data source and return a DBAPI 2.0 connection.

    :param uri: full ODBC connection string, e.g.
        ``"Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;"``.
    :param dsn: DSN name from ``odbc.ini`` (used instead of, or alongside, *uri*).
    :param username: sent as ``UID=``.
    :param password: sent as ``PWD=``.
    :param driver_path: path to ``libadbc_driver_odbc.so``; defaults to
        :func:`driver_path`.  An ADBC driver manifest name such as ``"odbc"``
        also works, since the value is passed straight to the driver manager.
    :param autocommit: passed to the driver manager (default: transactional).
    :param conn_kwargs: extra connection-level ADBC options.
    :param options: further database options.  A bare name is prefixed with
        ``adbc.odbc.`` (``batch_size=4096`` sets ``adbc.odbc.batch_size``);
        a dotted name is used as given.  ``bool`` becomes ``"true"``/``"false"``.

    The return value is an ``adbc_driver_manager.dbapi.Connection``, so
    everything ADBC's DBAPI layer offers (``cursor()``, ``fetch_arrow_table()``,
    ``adbc_ingest()``, ``adbc_get_objects()``, ...) is available.
    """
    if uri is None and dsn is None:
        raise ValueError("connect() needs at least one of uri= or dsn=")

    db_kwargs: Dict[str, str] = {}
    if uri is not None:
        db_kwargs["uri"] = uri
    if dsn is not None:
        db_kwargs["dsn"] = dsn
    if username is not None:
        db_kwargs["username"] = username
    if password is not None:
        db_kwargs["password"] = password
    for key, value in options.items():
        if value is None:
            continue
        db_kwargs[_option_key(key)] = _option_value(value)

    driver = driver_path if driver_path is not None else _find_driver()
    if _preload.preload_enabled():
        # Before pyarrow comes in below.  Best effort: a driver that will not
        # load here still gets its real failure reported by the connection.
        preload_odbc_driver(uri=uri, dsn=dsn)
    return _dbapi_module().connect(
        driver=driver,
        db_kwargs=db_kwargs,
        conn_kwargs=dict(conn_kwargs or {}),
        autocommit=autocommit,
    )
