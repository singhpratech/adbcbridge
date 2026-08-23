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

"""Loading an ODBC driver library before Arrow is imported.

Some ODBC drivers need a library -- in practice libstdc++ -- to live in static
thread-local storage.  glibc can only put a library there while nothing has yet
used its TLS dynamically, and importing pyarrow does exactly that to libstdc++.
Afterwards the driver can never be loaded into that process, and unixODBC
reports the refusal as "file not found" (see ``docs/TROUBLESHOOTING.md``).

Loading the ODBC driver *first* settles the question in its favour and costs
nothing: it is the same library the connection is about to load anyway.  Nothing
in this module may import pyarrow, adbc_driver_manager, or anything that does --
it has to run before them to be worth anything.
"""

from __future__ import annotations

import ctypes
import os
import sys
import threading
from typing import Dict, Optional

from ._locate import odbc_driver_library

__all__ = ["preload_odbc_driver"]

#: Set to 0/false/no to switch the automatic preload in ``connect()`` off.
ENV_VAR = "ADBCBRIDGE_PRELOAD"

_lock = threading.Lock()
_attempted: Dict[str, Optional[str]] = {}  # library path -> error, or None if loaded
# (driver, uri, dsn) -> library path.  Resolving a driver *name* reads
# odbcinst.ini, which means running odbcinst(1): not something to repeat on
# every connect() when the answer cannot change.
_resolved: Dict[tuple, Optional[str]] = {}


def _dlopen(path: str) -> Optional[str]:
    """dlopen *path* the way unixODBC will.  Returns an error string, or None.

    ``ctypes.CDLL`` always adds ``RTLD_NOW``, which would fail on drivers whose
    undefined symbols the driver manager resolves later; libltdl (which unixODBC
    loads drivers through) uses ``RTLD_LAZY``, so call dlopen directly.
    """
    try:
        libc = ctypes.CDLL(None)
        libc.dlopen.restype = ctypes.c_void_p
        libc.dlopen.argtypes = [ctypes.c_char_p, ctypes.c_int]
        libc.dlerror.restype = ctypes.c_char_p
        libc.dlerror.argtypes = []
    except (OSError, AttributeError) as exc:  # pragma: no cover - not glibc
        return str(exc)
    libc.dlerror()  # clear any error left by an earlier caller
    handle = libc.dlopen(os.fsencode(path), os.RTLD_LAZY | os.RTLD_LOCAL)
    if handle:
        return None  # deliberately never dlclose()d: that is the whole point
    message = libc.dlerror()
    return message.decode("utf-8", "replace") if message else "dlopen failed"


def preload_odbc_driver(
    driver: Optional[str] = None,
    *,
    uri: Optional[str] = None,
    dsn: Optional[str] = None,
    strict: bool = False,
) -> Optional[str]:
    """Load the ODBC driver library now, before anything imports pyarrow.

    *driver* is the path of an ODBC driver library or a name from
    ``odbcinst.ini``; it can also be left out and taken from the ``Driver=`` of
    *uri*, or from the ``Driver`` of the *dsn* section of ``odbc.ini``.

    Returns the path that was loaded, or ``None`` if nothing was.  This is a
    best-effort convenience -- a driver that will not preload is left to fail,
    with its real reason, at connection time -- unless *strict* is set, which
    turns both "could not work out which library" and "it would not load" into
    an :class:`OSError`.

    It only ever helps when called before pyarrow is imported (importing
    ``adbcbridge`` alone does not import pyarrow; ``adbcbridge.connect`` does).
    """
    if sys.platform == "win32":
        # Static TLS surplus is a glibc/ELF concept; the Windows loader has no
        # equivalent failure to head off.
        return None
    key = (driver, uri, dsn)
    with _lock:
        if key in _resolved:
            path = _resolved[key]
        else:
            path = odbc_driver_library(driver, uri=uri, dsn=dsn)
            _resolved[key] = path
    if path is None:
        if strict:
            raise OSError(
                "could not work out which ODBC driver library to preload from "
                "driver=%r uri=%r dsn=%r" % (driver, uri, dsn)
            )
        return None
    with _lock:
        if path in _attempted:
            failure = _attempted[path]
        else:
            failure = _dlopen(path)
            _attempted[path] = failure
    if failure is None:
        return path
    if strict:
        raise OSError(
            "%s could not be preloaded: %s%s"
            % (
                path,
                failure,
                (
                    "\npyarrow is already imported in this process, which is too late "
                    "for this to help; see docs/TROUBLESHOOTING.md"
                    if "static TLS" in failure and "pyarrow" in sys.modules
                    else ""
                ),
            )
        )
    return None


def preload_enabled() -> bool:
    """Whether ``connect()`` should preload (``ADBCBRIDGE_PRELOAD=0`` says no)."""
    value = os.environ.get(ENV_VAR)
    if value is None:
        return True
    return value.strip().lower() not in ("0", "false", "no", "off")
