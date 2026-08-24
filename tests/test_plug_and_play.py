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

"""Installation is plug-and-play: install, then load the driver by the name "odbc".

Covers the two ways the driver manager is meant to find the manifest:

  1. ``cmake --install build --prefix DIR`` + ``ADBC_DRIVER_PATH``
  2. ``install.sh`` + the user config directory (``$XDG_CONFIG_HOME/adbc/drivers``),
     with nothing at all set in the environment

Each connection runs in a subprocess with an explicitly built environment, so a
manifest that happens to be installed on the developer's machine cannot make
the test pass by accident.  Test 2 includes a negative control: with the
manifest directory swapped for an empty one the same connection must fail.

Run:  python tests/test_plug_and_play.py
"""

import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
SQLITE_ODBC = os.environ.get("SQLITE_ODBC_DRIVER", "SQLite3")

# Connect by the *name* "odbc" -- the whole point of the manifest.  Printing the
# driver name proves the library that answered is adbcbridge and not, say,
# unixODBC's own libodbc.so picked up by the dlopen fallback.
CONNECT_SNIPPET = """
import adbc_driver_manager.dbapi as dbapi
# This is about resolving the driver by name, not about native delegation
# (which would hand a SQLite target to adbc_driver_sqlite and report its name).
conn = dbapi.connect(
    driver="odbc",
    db_kwargs={{"uri": {uri!r}, "adbc.odbc.delegate": "never"}},
)
info = conn.adbc_get_info()
with conn.cursor() as cur:
    cur.execute("SELECT 42 AS answer")
    assert cur.fetch_arrow_table().to_pydict() == {{"answer": [42]}}
conn.close()
assert "ADBC ODBC Driver" in info["driver_name"], info
print("CONNECTED", info["driver_name"])
"""


def run(cmd, **kw):
    """Run a command, raising with captured output on failure."""
    proc = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if proc.returncode != 0:
        raise AssertionError(
            f"command failed ({proc.returncode}): {' '.join(map(str, cmd))}\n"
            f"--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        )
    return proc


# Where the ADBC driver manager looks for a *user* manifest, relative to the
# directory the test controls: $XDG_CONFIG_HOME/adbc/drivers on Linux, but
# ~/Library/Application Support/ADBC/Drivers on macOS -- the manager does not
# read XDG_CONFIG_HOME there, so the test has to move HOME instead.
DARWIN = sys.platform == "darwin"
USER_MANIFEST_SUBDIR = (
    pathlib.Path("Library/Application Support/ADBC/Drivers") if DARWIN
    else pathlib.Path("adbc/drivers")
)


def child_env(manifest_dir=None, user_config_root=None):
    """A clean environment: only the manifest location under test is visible.

    `user_config_root` is the directory that stands in for XDG_CONFIG_HOME (Linux)
    or HOME (macOS); USER_MANIFEST_SUBDIR below it is where the manifest goes.
    """
    env = dict(os.environ)
    # Never let an ambient manifest location leak into the child.
    env.pop("ADBC_DRIVER_PATH", None)
    env.pop("XDG_CONFIG_HOME", None)
    if manifest_dir is not None:
        env["ADBC_DRIVER_PATH"] = str(manifest_dir)
    if user_config_root is not None:
        if DARWIN:
            env["HOME"] = str(user_config_root)
        else:
            env["XDG_CONFIG_HOME"] = str(user_config_root)
    return env


def connect_by_name(db_path, env):
    """Run the by-name connection in a subprocess. Returns the CompletedProcess."""
    uri = f"Driver={SQLITE_ODBC};Database={db_path};"
    return subprocess.run(
        [sys.executable, "-c", CONNECT_SNIPPET.format(uri=uri)],
        capture_output=True,
        text=True,
        env=env,
    )


def manifest_library(manifest):
    """The library path recorded under [Driver.shared] in a manifest."""
    lines = manifest.read_text().splitlines()
    start = lines.index("[Driver.shared]")
    for line in lines[start + 1 :]:
        if "=" in line:
            return pathlib.Path(line.split("=", 1)[1].strip().strip("'"))
    raise AssertionError(f"no library entry in {manifest}")


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def competing_manifests():
    """Manifest locations, outside our control, that could mask a real failure."""
    found = []
    # The Python driver manager adds sys.prefix/etc/adbc/drivers inside a venv.
    if sys.prefix != sys.base_prefix:
        found.append(pathlib.Path(sys.prefix) / "etc/adbc/drivers/odbc.toml")
    found.append(pathlib.Path("/etc/adbc/drivers/odbc.toml"))
    return [p for p in found if p.exists()]


# Under pytest the two test functions take their `tmp` and `build` from these
# fixtures; run as a script, main() below passes the same values by hand.  The
# import is guarded so the script keeps working where pytest is not installed.
try:
    import pytest
except ImportError:  # pragma: no cover
    pytest = None

if pytest is not None:

    @pytest.fixture(scope="module")
    def tmp(tmp_path_factory):
        stray = competing_manifests()
        if stray:
            pytest.skip("an odbc.toml the test does not control is installed at "
                        + ", ".join(map(str, stray)))
        if not shutil.which("cmake"):
            pytest.skip("cmake not found")
        return tmp_path_factory.mktemp("adbcbridge-pnp-")

    @pytest.fixture(scope="module")
    def build(tmp):
        return build_tree(tmp)


def build_tree(tmp):
    """A configured, built CMake build tree to install from."""
    build = pathlib.Path(os.environ.get("ADBCBRIDGE_BUILD_DIR", tmp / "build"))
    if not (build / "CMakeCache.txt").exists():
        run(["cmake", "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", str(build), "-j", str(os.cpu_count() or 1)])
    return build


def test_install_prefix_flow(tmp, build):
    """cmake --install --prefix DIR, found via ADBC_DRIVER_PATH."""
    prefix = tmp / "prefix-a"
    run(["cmake", "--install", str(build), "--prefix", str(prefix)])

    manifest = prefix / "etc/adbc/drivers/odbc.toml"
    assert manifest.is_file(), f"no manifest at {manifest}"

    # The recorded path must point into *this* prefix and must exist -- a stale
    # configure-time path is the classic way this breaks after relocation.
    lib = manifest_library(manifest)
    assert lib.is_file(), f"manifest points at missing library {lib}"
    assert prefix in lib.parents, f"{lib} is not inside {prefix}"

    env = child_env(manifest_dir=manifest.parent)
    proc = connect_by_name(tmp / "a.db", env)
    assert proc.returncode == 0, f"connect by name failed:\n{proc.stdout}{proc.stderr}"
    assert "CONNECTED" in proc.stdout, proc.stdout
    print("  install --prefix + ADBC_DRIVER_PATH:", proc.stdout.strip())


def test_install_sh_flow(tmp):
    """install.sh, found via the user config dir with an empty environment."""
    prefix = tmp / "prefix-b"
    xdg = tmp / "xdg"  # stands in for XDG_CONFIG_HOME, or for HOME on macOS
    manifest_dir = xdg / USER_MANIFEST_SUBDIR

    env = dict(os.environ)
    env.update(
        PREFIX=str(prefix),
        MANIFEST_DIR=str(manifest_dir),
        BUILD_DIR=str(tmp / "build-sh"),
    )
    run(["bash", str(ROOT / "install.sh")], env=env)

    manifest = manifest_dir / "odbc.toml"
    assert manifest.is_file(), f"install.sh produced no manifest at {manifest}"
    lib = manifest_library(manifest)
    assert lib.is_file(), f"manifest points at missing library {lib}"
    assert prefix in lib.parents, f"{lib} is not inside {prefix}"

    # Nothing set but XDG_CONFIG_HOME (HOME on macOS): the driver manager must
    # find the manifest on its own. This is the "plug and play" claim.
    proc = connect_by_name(tmp / "b.db", child_env(user_config_root=xdg))
    assert proc.returncode == 0, (
        f"connect via user config dir failed:\n{proc.stdout}{proc.stderr}"
    )
    print("  install.sh + user config dir:", proc.stdout.strip())

    # Negative control: same call, empty config dir -> must fail. Without this,
    # a manifest installed elsewhere on the machine could be what actually
    # answered above.
    empty = tmp / "xdg-empty"
    (empty / USER_MANIFEST_SUBDIR).mkdir(parents=True)
    proc = connect_by_name(tmp / "c.db", child_env(user_config_root=empty))
    assert proc.returncode != 0, (
        "control failed: connecting by name succeeded with no manifest "
        f"installed, so the test above proves nothing:\n{proc.stdout}"
    )
    print("  negative control: no manifest -> connect fails, as expected")

    # Idempotence: a second run changes nothing and still succeeds.
    before = digest(manifest), digest(lib)
    run(["bash", str(ROOT / "install.sh")], env=env)
    assert (digest(manifest), digest(lib)) == before, "install.sh is not idempotent"
    print("  install.sh rerun: byte-identical, idempotent")


def main():
    stray = competing_manifests()
    if stray:
        print(
            "SKIP: an odbc.toml the test does not control is installed at "
            f"{', '.join(map(str, stray))}; it would mask real failures. "
            "Remove it and rerun."
        )
        return 77

    if not shutil.which("cmake"):
        print("SKIP: cmake not found")
        return 77

    tmp = pathlib.Path(tempfile.mkdtemp(prefix="adbcbridge-pnp-"))
    try:
        build = build_tree(tmp)
        print("plug-and-play:")
        test_install_prefix_flow(tmp, build)
        test_install_sh_flow(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("PLUG AND PLAY OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
