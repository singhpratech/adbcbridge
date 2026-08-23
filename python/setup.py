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

"""Packaging hook: bundle the driver library into the wheel when one is around.

Everything else about the package is declared in pyproject.toml.  The only job
of this file is the optional bundling step:

    ADBCBRIDGE_LIBRARY=/path/to/libadbc_driver_odbc.so  python -m build --wheel

If ``ADBCBRIDGE_LIBRARY`` is unset we look for a CMake build tree next to the
repository (``build/libadbc_driver_odbc.so`` and the usual multi-config
variants).  When a library is found it is copied into the ``adbcbridge``
package, which makes the resulting wheel self-contained (and platform
specific).  When none is found the wheel is pure Python and
``adbcbridge.driver_path()`` falls back to the driver manifest or to a
system-wide install at run time.
"""

import os
import pathlib
import shutil
import sys

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py

try:  # setuptools >= 70 exposes bdist_wheel; older installs use the wheel package
    from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
except ImportError:  # pragma: no cover - depends on the build environment
    try:
        from wheel.bdist_wheel import bdist_wheel as _bdist_wheel
    except ImportError:  # pragma: no cover
        _bdist_wheel = None

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent

if sys.platform == "win32":
    LIB_NAMES = ("adbc_driver_odbc.dll", "libadbc_driver_odbc.dll")
elif sys.platform == "darwin":
    LIB_NAMES = ("libadbc_driver_odbc.dylib",)
else:
    LIB_NAMES = ("libadbc_driver_odbc.so",)


def find_library():
    """Return the driver library to bundle, or None."""
    explicit = os.environ.get("ADBCBRIDGE_LIBRARY")
    if explicit:
        path = pathlib.Path(explicit)
        if not path.is_file():
            raise SystemExit("ADBCBRIDGE_LIBRARY=%s does not exist" % explicit)
        return path
    build_dir = pathlib.Path(os.environ.get("ADBCBRIDGE_BUILD_DIR", REPO / "build"))
    for sub in ("", "Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
        for name in LIB_NAMES:
            candidate = build_dir / sub / name if sub else build_dir / name
            if candidate.is_file():
                return candidate
    return None


class build_py(_build_py):
    def run(self):
        super().run()
        package_dir = pathlib.Path(self.build_lib) / "adbcbridge"
        lib = find_library()
        if lib is None:
            # Drop a copy left behind by an earlier build in the same tree, so
            # a build without a library really does produce a pure wheel.
            for name in LIB_NAMES:
                stale = package_dir / name
                if stale.exists():
                    stale.unlink()
            return
        target = package_dir / lib.name
        target.parent.mkdir(parents=True, exist_ok=True)
        self.announce("bundling driver library %s" % lib, level=2)
        shutil.copyfile(str(lib), str(target))
        shutil.copymode(str(lib), str(target))


cmdclass = {"build_py": build_py}

if _bdist_wheel is not None:

    class bdist_wheel(_bdist_wheel):
        def finalize_options(self):
            # A bundled .so makes the wheel platform specific, but never
            # Python-ABI specific: there is no extension module in here.
            # Claiming ext modules is what puts the package in platlib (and so
            # at the root of the wheel) instead of purelib.
            if find_library() is not None:
                self.distribution.has_ext_modules = lambda: True
            super().finalize_options()
            if find_library() is not None:
                self.root_is_pure = False

        def get_tag(self):
            python, abi, plat = super().get_tag()
            if find_library() is not None:
                return "py3", "none", plat
            return python, abi, plat

    cmdclass["bdist_wheel"] = bdist_wheel

setup(cmdclass=cmdclass)
