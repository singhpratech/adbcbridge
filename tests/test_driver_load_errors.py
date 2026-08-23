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

"""What the driver says when the ODBC driver library will not load.

unixODBC reports every failure to load a driver library the same way --
"Can't open lib '<path>' : file not found" -- because libltdl throws the
dlerror() away.  For a file that is not there that is fine; for a file that is
there and was refused it sends people looking for the wrong problem entirely.
adbcbridge opens the same path itself and reports what the loader really said.

Run under pytest, or directly:

    ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so python tests/test_driver_load_errors.py
"""

import ctypes
import os
import pathlib
import sys

import adbc_driver_manager

try:
    import pytest
except ImportError:  # running as a plain script
    pytest = None

HERE = pathlib.Path(__file__).resolve().parent
BUILD = pathlib.Path(
    os.environ.get("ADBC_ODBC_BUILD_DIR", str(HERE.parent / "build"))
).resolve()
DRIVER = os.environ.get(
    "ADBC_ODBC_DRIVER", str(HERE.parent / "build" / "libadbc_driver_odbc.so")
)


def _connect_error(uri):
    """Try to connect with *uri* and return the error text.  Fails if it works."""
    try:
        database = adbc_driver_manager.AdbcDatabase(driver=DRIVER, uri=uri)
        adbc_driver_manager.AdbcConnection(database)
    except Exception as exc:  # noqa: BLE001 - the message is the subject here
        return str(exc)
    raise AssertionError("connecting with %r unexpectedly succeeded" % uri)


def _skip(reason):
    if pytest is not None and "PYTEST_CURRENT_TEST" in os.environ:
        pytest.skip(reason)
    print("SKIP: %s" % reason)
    return True


def test_missing_driver_library_is_still_reported_as_missing():
    """A driver that genuinely is not there must keep saying so."""
    missing = str(HERE / "no-such-odbc-driver.so")
    message = _connect_error("Driver=%s;" % missing)
    assert missing in message
    # unixODBC's own wording, kept...
    assert "file not found" in message
    # ...and the reason from the operating system, which is the same verdict.
    assert "No such file or directory" in message


def test_unreadable_driver_library_says_permission_denied(tmp_path=None):
    """"file not found" for a file that is there but unreadable is misleading."""
    if sys.platform == "win32":
        return _skip("POSIX file modes")
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return _skip("root reads unreadable files")
    if tmp_path is None:  # running as a plain script
        import tempfile

        tmp_path = pathlib.Path(tempfile.mkdtemp())
    library = tmp_path / "libunreadable.so"
    library.write_bytes(b"\x7fELF not really\n")
    library.chmod(0o000)
    try:
        message = _connect_error("Driver=%s;" % library)
    finally:
        library.chmod(0o600)
    assert str(library) in message
    assert "Permission denied" in message


def test_static_tls_exhaustion_is_explained():
    """The failure importing pyarrow leaves behind, reproduced without pyarrow.

    ``libadbc_odbc_tls_dep`` stands in for libstdc++ and
    ``libadbc_odbc_tls_user`` for an ODBC driver that needs its thread-local in
    static TLS.  Touching the first one's thread-local through the dynamic model
    makes the second one permanently unloadable in this process -- which is what
    an ``import pyarrow`` does to every ODBC driver built the same way.
    """
    dep = BUILD / "libadbc_odbc_tls_dep.so"
    user = BUILD / "libadbc_odbc_tls_user.so"
    if not dep.is_file() or not user.is_file():
        return _skip("the static-TLS fixture is not built (%s)" % BUILD)
    handle = ctypes.CDLL(str(dep))
    handle.AdbcOdbcTlsTouch()  # the point of no return; see tests/c/tls_dep.c

    # Nothing else in this process can load `user` any more, fixture or not.
    try:
        ctypes.CDLL(str(user))
    except OSError as exc:
        assert "static TLS" in str(exc)
    else:
        return _skip("this loader does not pin a dlopen()ed library to dynamic TLS")

    message = _connect_error("Driver=%s;" % user)
    assert str(user) in message
    assert "static TLS" in message, message
    # The three things a reader needs: that the file is there, what really
    # happened, and what to do about it.
    assert "the file is there and readable" in message, message
    assert "pyarrow" in message, message
    assert "LD_PRELOAD" in message, message


def main():
    failures = 0
    for name, func in sorted(globals().items()):
        if not name.startswith("test_") or not callable(func):
            continue
        try:
            func()
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print("FAIL %s: %s" % (name, exc))
        else:
            print("ok   %s" % name)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
