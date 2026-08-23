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

"""Finding libadbc_driver_odbc.so, and listing the ODBC drivers on the box."""

from __future__ import annotations

import os
import pathlib
import platform
import re
import subprocess
import sys
from typing import Dict, Iterator, List, NamedTuple, Optional

__all__ = [
    "DriverNotFoundError",
    "OdbcDriver",
    "driver_path",
    "odbc_driver_library",
    "odbc_drivers",
]

#: Environment variable checked first by :func:`driver_path`.
ENV_VAR = "ADBC_ODBC_DRIVER"

#: Name of the ADBC driver manifest installed by ``cmake --install``.
MANIFEST_NAME = "odbc.toml"


class DriverNotFoundError(RuntimeError):
    """Raised when the adbcbridge shared library cannot be located."""


def _library_names() -> List[str]:
    if sys.platform == "win32":
        return ["adbc_driver_odbc.dll", "libadbc_driver_odbc.dll"]
    if sys.platform == "darwin":
        return ["libadbc_driver_odbc.dylib"]
    return ["libadbc_driver_odbc.so"]


#: uname machine names -> the architecture names ADBC manifests use.
_ARCH_ALIASES = {
    "x86_64": "amd64",
    "amd64": "amd64",
    "i386": "x86",
    "i686": "x86",
    "x86": "x86",
    "aarch64": "arm64",
    "arm64": "arm64",
    "armv7l": "arm",
    "armv6l": "arm",
    "ppc64le": "powerpc64le",
    "ppc64": "powerpc64",
    "s390x": "s390x",
    "riscv64": "riscv64",
}


def _platform_tuple() -> str:
    """The ``<os>_<arch>`` key used under ``[Driver.shared]`` in a manifest."""
    if sys.platform == "win32":
        os_name = "windows"
    elif sys.platform == "darwin":
        os_name = "macos"
    elif sys.platform.startswith("freebsd"):
        os_name = "freebsd"
    elif sys.platform.startswith("openbsd"):
        os_name = "openbsd"
    else:
        os_name = "linux"
    machine = platform.machine().lower()
    arch = _ARCH_ALIASES.get(machine, machine)
    return "%s_%s" % (os_name, arch)


def _platform_keys() -> List[str]:
    """Manifest keys accepted for this machine, most canonical first.

    The manifest CMake installs uses the ADBC spelling (``linux_amd64``); a
    hand-written one may well use the uname spelling (``linux_x86_64``), so
    both are accepted.  The OS half always has to match.
    """
    canonical = _platform_tuple()
    keys = [canonical]
    os_name = canonical.rsplit("_", 1)[0]
    machine = platform.machine().lower()
    for alias in {machine} | {a for a, c in _ARCH_ALIASES.items()
                              if c == _ARCH_ALIASES.get(machine, machine)}:
        key = "%s_%s" % (os_name, alias)
        if key not in keys:
            keys.append(key)
    return keys


# --- 1. environment variable ------------------------------------------------


def _from_env() -> Optional[pathlib.Path]:
    value = os.environ.get(ENV_VAR)
    if not value:
        return None
    path = pathlib.Path(value).expanduser()
    if not path.is_file():
        raise DriverNotFoundError(
            "%s=%s does not point at a file" % (ENV_VAR, value)
        )
    return path.resolve()


# --- 2. copy bundled in the wheel -------------------------------------------


def _bundled() -> Optional[pathlib.Path]:
    here = pathlib.Path(__file__).resolve().parent
    for name in _library_names():
        candidate = here / name
        if candidate.is_file():
            return candidate
    return None


# --- 3. ADBC driver manifest named "odbc" -----------------------------------


def manifest_dirs() -> List[pathlib.Path]:
    """Directories the ADBC driver manager searches for ``odbc.toml``."""
    dirs: List[pathlib.Path] = []
    env = os.environ.get("ADBC_DRIVER_PATH")
    if env:
        dirs += [pathlib.Path(p) for p in env.split(os.pathsep) if p]
    prefix = pathlib.Path(sys.prefix)
    dirs += [prefix / "etc" / "adbc" / "drivers", prefix / "share" / "adbc" / "drivers"]
    home = pathlib.Path.home()
    if sys.platform == "darwin":
        dirs += [
            home / "Library" / "Application Support" / "ADBC" / "Drivers",
            pathlib.Path("/Library/Application Support/ADBC/Drivers"),
        ]
    elif sys.platform != "win32":
        config = os.environ.get("XDG_CONFIG_HOME")
        base = pathlib.Path(config) if config else home / ".config"
        dirs.append(base / "adbc" / "drivers")
    dirs += [
        pathlib.Path("/etc/adbc/drivers"),
        pathlib.Path("/usr/local/etc/adbc/drivers"),
        pathlib.Path("/usr/share/adbc/drivers"),
        pathlib.Path("/usr/local/share/adbc/drivers"),
    ]
    return dirs


def _parse_toml(text: str) -> Optional[dict]:
    try:
        import tomllib  # Python >= 3.11
    except ImportError:  # pragma: no cover - only on 3.9/3.10
        try:
            import tomli as tomllib  # type: ignore[no-redef]
        except ImportError:
            return None
    try:
        return tomllib.loads(text)
    except Exception:
        return None


_SHARED_RE = re.compile(
    r"^\s*(?P<key>[A-Za-z0-9_.\"']+)\s*=\s*(?P<quote>['\"])(?P<value>[^'\"]*)(?P=quote)"
)


def _manifest_library(text: str) -> Optional[str]:
    """Extract the ``[Driver.shared]`` entry for this platform from a manifest."""
    keys = _platform_keys()
    parsed = _parse_toml(text)
    if parsed is not None:
        shared = parsed.get("Driver", {}).get("shared")
        if isinstance(shared, str):  # the spec also allows a bare path
            return shared
        if isinstance(shared, dict):
            for key in keys:
                if key in shared:
                    return shared[key]
        return None
    # No TOML parser available (Python < 3.11 without tomli): the manifests we
    # care about are flat enough to scan for the [Driver.shared] table by hand.
    in_shared = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            in_shared = stripped.rstrip().rstrip("]").strip("[]").strip() == "Driver.shared"
            continue
        if not in_shared:
            continue
        match = _SHARED_RE.match(line)
        if match and match.group("key").strip("\"'") in keys:
            return match.group("value")
    return None


def _from_manifest() -> Optional[pathlib.Path]:
    seen = set()
    for directory in manifest_dirs():
        manifest = directory / MANIFEST_NAME
        if manifest in seen:
            continue
        seen.add(manifest)
        try:
            text = manifest.read_text(encoding="utf-8")
        except OSError:
            continue
        library = _manifest_library(text)
        if not library:
            continue
        path = pathlib.Path(library).expanduser()
        if path.is_file():
            return path.resolve()
    return None


# --- 4. common install locations ---------------------------------------------


def _install_dirs() -> Iterator[pathlib.Path]:
    prefix = pathlib.Path(sys.prefix)
    yield prefix / "lib"
    yield prefix / "lib64"
    if sys.platform == "win32":
        yield prefix / "bin"
        yield prefix / "Library" / "bin"
    for base in ("/usr/local", "/usr", "/opt/adbcbridge", "/opt/homebrew"):
        yield pathlib.Path(base) / "lib"
        yield pathlib.Path(base) / "lib64"
    if sys.platform.startswith("linux"):
        machine = platform.machine().lower()  # x86_64-linux-gnu, aarch64-linux-gnu, ...
        yield pathlib.Path("/usr/lib/%s-linux-gnu" % machine)
        yield pathlib.Path("/usr/local/lib/%s-linux-gnu" % machine)
    # A CMake build tree next to a source checkout, so `pip install -e python`
    # works straight after `cmake --build build`.
    repo = pathlib.Path(__file__).resolve().parents[3]
    for sub in ("", "Release", "Debug", "RelWithDebInfo", "MinSizeRel"):
        yield repo / "build" / sub if sub else repo / "build"


def _from_install_dirs() -> Optional[pathlib.Path]:
    names = _library_names()
    for directory in _install_dirs():
        for name in names:
            candidate = directory / name
            if candidate.is_file():
                return candidate.resolve()
    return None


def driver_path() -> str:
    """Return the absolute path of the adbcbridge shared library.

    Looked up in this order:

    1. the ``ADBC_ODBC_DRIVER`` environment variable;
    2. a copy bundled inside this package (see ``python/README.md``);
    3. the ADBC driver manifest named ``odbc`` (``odbc.toml``), in the
       directories the ADBC driver manager searches;
    4. common install locations (``<sys.prefix>/lib``, ``/usr/local/lib``,
       ``/usr/lib``, and a ``build/`` tree next to a source checkout).

    Raises :class:`DriverNotFoundError` if none of those has it.
    """
    for finder in (_from_env, _bundled, _from_manifest, _from_install_dirs):
        found = finder()
        if found is not None:
            return str(found)
    raise DriverNotFoundError(
        "could not find %s.\n"
        "Set %s=/path/to/%s, install the driver "
        "(cmake --install build --prefix \"$VIRTUAL_ENV\"), or build it with "
        "cmake --build build in a source checkout."
        % (_library_names()[0], ENV_VAR, _library_names()[0])
    )


# --- ODBC driver inventory ---------------------------------------------------


class OdbcDriver(NamedTuple):
    """One entry of ``odbcinst.ini``."""

    name: str
    path: str
    description: str
    attributes: Dict[str, str]


def odbcinst_ini() -> Optional[pathlib.Path]:
    """Path of the system ``odbcinst.ini``, as reported by ``odbcinst -j``."""
    try:
        out = subprocess.run(
            ["odbcinst", "-j"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
        ).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        out = ""
    for line in out.splitlines():
        # "DRIVERS............: /etc/odbcinst.ini"
        if line.upper().startswith("DRIVERS") and ":" in line:
            candidate = pathlib.Path(line.split(":", 1)[1].strip())
            if candidate.is_file():
                return candidate
    sysini = os.environ.get("ODBCSYSINI")
    fallbacks = [pathlib.Path(sysini) / "odbcinst.ini"] if sysini else []
    fallbacks += [
        pathlib.Path("/etc/odbcinst.ini"),
        pathlib.Path("/usr/local/etc/odbcinst.ini"),
        pathlib.Path("/opt/homebrew/etc/odbcinst.ini"),
        pathlib.Path.home() / ".odbcinst.ini",
    ]
    for candidate in fallbacks:
        if candidate.is_file():
            return candidate
    return None


def odbc_drivers() -> List[OdbcDriver]:
    """List the ODBC drivers registered in ``odbcinst.ini`` (may be empty)."""
    ini = odbcinst_ini()
    if ini is None:
        return []
    import configparser

    parser = configparser.RawConfigParser(strict=False)
    parser.optionxform = str  # keep the case of "Driver", "Setup", ...
    try:
        parser.read_string(ini.read_text(encoding="utf-8", errors="replace"))
    except (OSError, configparser.Error):
        return []
    drivers = []
    for section in parser.sections():
        if section.upper() == "ODBC DRIVERS":
            continue  # legacy index section, not a driver
        attrs = dict(parser.items(section))
        lowered = {k.lower(): v for k, v in attrs.items()}
        drivers.append(
            OdbcDriver(
                name=section,
                path=lowered.get("driver", ""),
                description=lowered.get("description", ""),
                attributes=attrs,
            )
        )
    return drivers


# --- resolving the *ODBC* driver library behind a connection string ----------


def odbc_ini() -> Optional[pathlib.Path]:
    """Path of the ``odbc.ini`` holding DSNs, as reported by ``odbcinst -j``."""
    try:
        out = subprocess.run(
            ["odbcinst", "-j"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
        ).stdout.decode("utf-8", "replace")
    except (OSError, subprocess.SubprocessError):
        out = ""
    for line in out.splitlines():
        # "SYSTEM DATA SOURCES: /etc/odbc.ini"
        if "DATA SOURCES" in line.upper() and ":" in line:
            candidate = pathlib.Path(line.split(":", 1)[1].strip())
            if candidate.is_file():
                return candidate
    ini = os.environ.get("ODBCINI")
    fallbacks = [pathlib.Path(ini)] if ini else []
    sysini = os.environ.get("ODBCSYSINI")
    if sysini:
        fallbacks.append(pathlib.Path(sysini) / "odbc.ini")
    fallbacks += [
        pathlib.Path.home() / ".odbc.ini",
        pathlib.Path("/etc/odbc.ini"),
        pathlib.Path("/usr/local/etc/odbc.ini"),
        pathlib.Path("/opt/homebrew/etc/odbc.ini"),
    ]
    for candidate in fallbacks:
        if candidate.is_file():
            return candidate
    return None


def _ini_value(path: Optional[pathlib.Path], section: str, key: str) -> Optional[str]:
    """One key of one section of an ODBC ini file, matched case-insensitively."""
    if path is None:
        return None
    import configparser

    parser = configparser.RawConfigParser(strict=False)
    parser.optionxform = str
    try:
        parser.read_string(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, configparser.Error):
        return None
    for name in parser.sections():
        if name.lower() != section.lower():
            continue
        for option, value in parser.items(name):
            if option.lower() == key.lower():
                return value.strip()
    return None


def connection_string_attribute(uri: str, key: str) -> Optional[str]:
    """Read one ``KEY=VALUE`` attribute out of an ODBC connection string.

    Values wrapped in ``{}`` -- the ODBC way of quoting a value that contains a
    semicolon -- are unwrapped.  Returns ``None`` when the key is not there.
    """
    if not uri:
        return None
    field = ""
    fields = []
    braced = False
    for char in uri:
        if char == "{":
            braced = True
        elif char == "}":
            braced = False
        elif char == ";" and not braced:
            fields.append(field)
            field = ""
            continue
        field += char
    fields.append(field)
    for entry in fields:
        name, sep, value = entry.partition("=")
        if not sep or name.strip().lower() != key.lower():
            continue
        value = value.strip()
        if value.startswith("{") and value.endswith("}"):
            value = value[1:-1]
        return value
    return None


def odbc_driver_library(
    driver: Optional[str] = None,
    *,
    uri: Optional[str] = None,
    dsn: Optional[str] = None,
) -> Optional[str]:
    """Absolute path of the ODBC driver library a connection would load.

    *driver* is either that path already or a name registered in
    ``odbcinst.ini``.  When it is not given, the ``Driver=`` attribute of *uri*
    is used, and failing that the ``Driver`` entry of the *dsn* section of
    ``odbc.ini``.  Returns ``None`` when nothing resolves to a file that is
    there -- this is a best-effort lookup, not a validation.
    """
    if driver is None and uri:
        driver = connection_string_attribute(uri, "Driver")
    if driver is None and dsn is None and uri:
        dsn = connection_string_attribute(uri, "DSN")
    if driver is None and dsn:
        driver = _ini_value(odbc_ini(), dsn, "Driver")
    if not driver:
        return None
    candidate = pathlib.Path(driver).expanduser()
    if candidate.is_file():
        return str(candidate.resolve())
    for entry in odbc_drivers():
        if entry.name.lower() != driver.lower() or not entry.path:
            continue
        path = pathlib.Path(entry.path).expanduser()
        if path.is_file():
            return str(path.resolve())
    return None
