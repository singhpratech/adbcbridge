#!/usr/bin/env bash
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
#
# Build adbcbridge and install it for the current user, with no root:
#
#   ~/.local/lib/libadbc_driver_odbc.so   the driver
#   ~/.config/adbc/drivers/odbc.toml      the manifest that names it "odbc"
#
# ~/.config/adbc/drivers is one of the directories the ADBC driver manager
# searches by default, so after this every binding can load the driver as
# driver="odbc" with nothing else set -- no ADBC_DRIVER_PATH, no LD_LIBRARY_PATH.
#
# Re-running is safe: it reconfigures the same build tree and overwrites the
# same two files.
#
# Usage:
#   ./install.sh                    build and install the bridge
#   ./install.sh --drivers          ... and the open-licence ODBC drivers a first
#                                   run needs: SQLite, PostgreSQL, MySQL/MariaDB
#                                   and ClickHouse (see "Driver bootstrap" below)
#   ./install.sh --drivers=sqlite,postgres   only those two
#   ./install.sh --drivers-only     just the drivers, the bridge is already built
#
# Environment overrides:
#   PREFIX        install prefix for the library    (default $HOME/.local)
#   MANIFEST_DIR  directory to write odbc.toml into (default the ADBC user
#                 config dir: ${XDG_CONFIG_HOME:-$HOME/.config}/adbc/drivers,
#                 or ~/Library/Application Support/ADBC/Drivers on macOS)
#   BUILD_DIR     CMake build tree                  (default <repo>/build)
#   BUILD_TYPE    CMake build type                  (default Release)
#   JOBS          parallel build jobs               (default nproc)
#   SUDO          command that runs a package manager as root (default sudo;
#                 set it empty when already root)
#
# Driver bootstrap (--drivers).  The bridge needs an ODBC driver per database,
# and the first ones most people want are open-licence and packaged everywhere:
#
#   SQLite        sqliteodbc                Debian libsqliteodbc, Fedora sqliteodbc,
#                                           Homebrew sqliteodbc
#   PostgreSQL    psqlodbc                  odbc-postgresql / postgresql-odbc / psqlodbc
#   MySQL/MariaDB MariaDB Connector/ODBC    odbc-mariadb / mariadb-connector-odbc /
#                                           mariadb-connector-odbc
#   ClickHouse    clickhouse-odbc           the project's own release tarball, pinned
#                                           below and checked against its SHA-256,
#                                           unpacked under $PREFIX/odbc-drivers
#
# The first three come from the system package manager (apt or dnf, through
# sudo, or Homebrew with no root) because their runtime libraries -- libpq,
# libmariadb -- have to be on the loader path, which only the package manager
# can arrange; the package also registers the driver name in odbcinst.ini.
# Vendor drivers whose licences forbid redistribution (Oracle, Db2, SQL Server,
# Snowflake, ...) stay the user's download; the install guides list them.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-$HOME/.local}"
BUILD_DIR="${BUILD_DIR:-$here/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
SUDO="${SUDO-sudo}"
if [ "$(id -u)" = 0 ]; then SUDO=""; fi

# clickhouse-odbc has no distribution package; this is the release the bridge
# was verified against, with the checksums the project publishes next to the
# tarballs (clickhouse-odbc-<ver>-<OS>.tar.xz.sha256 inside each release zip).
CH_ODBC_TAG="v1.5.5.20260810"
CH_ODBC_VER="1.5.5"
CH_ODBC_SHA256_LINUX="647d9eb31fb44b311f2b2241196ca0920167124627a745a90a0fac55e2cd226a"
CH_ODBC_SHA256_DARWIN="a60f83dc02600df2b8e1a4076ecfdccd7221d4031cea1c9e89c3f3a0118de168"

want_bridge=1
want_drivers=0
drivers="sqlite,postgres,mysql,clickhouse"
for arg in "$@"; do
  case "$arg" in
    --drivers) want_drivers=1 ;;
    --drivers=*) want_drivers=1; drivers="${arg#--drivers=}" ;;
    --drivers-only) want_drivers=1; want_bridge=0 ;;
    -h|--help)
      sed -n '/^# Usage:/,/^# Driver bootstrap/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "install.sh: unknown argument $arg (try --help)" >&2; exit 2 ;;
  esac
done

if [ -z "${MANIFEST_DIR:-}" ]; then
  # Mirror InternalAdbcUserConfigDir() in the driver manager: macOS uses
  # ~/Library/Application Support/ADBC/Drivers, everything else XDG.
  if [ "$(uname -s)" = "Darwin" ]; then
    MANIFEST_DIR="$HOME/Library/Application Support/ADBC/Drivers"
  else
    MANIFEST_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/adbc/drivers"
  fi
fi

if [ -z "${JOBS:-}" ]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 1)"
  else
    JOBS=1
  fi
fi

# ---------------------------------------------------------------------------
# The bridge

install_bridge() {
  command -v cmake >/dev/null 2>&1 || {
    echo "install.sh: cmake not found. Install it first:" >&2
    echo "  Debian/Ubuntu: sudo apt install cmake unixodbc-dev" >&2
    echo "  Fedora/RHEL:   sudo dnf install cmake unixODBC-devel" >&2
    echo "  macOS:         brew install cmake unixodbc" >&2
    exit 1
  }

  echo "==> Configuring   (prefix $PREFIX)"
  # ADBCBRIDGE_MANIFEST_DIR is absolute here, so the manifest lands in the user
  # config dir while the library still goes under $PREFIX.
  cmake -S "$here" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DADBCBRIDGE_MANIFEST_DIR="$MANIFEST_DIR"

  echo "==> Building"
  cmake --build "$BUILD_DIR" -j "$JOBS"

  echo "==> Installing"
  cmake --install "$BUILD_DIR"
}

# The library path, read back out of the manifest so that what we print is what
# the driver manager will actually load.  Match on the library basename rather
# than on "first quoted value": the metadata keys above [Driver.shared] (name,
# version, url, ...) are quoted the same way.
bridge_lib() {
  local manifest="$MANIFEST_DIR/odbc.toml"
  [ -f "$manifest" ] || return 1
  grep -o "'[^']*libadbc_driver_odbc[^']*'" "$manifest" | tr -d "'" | head -n1
}

# ---------------------------------------------------------------------------
# Driver bootstrap

os="$(uname -s)"
pkg_mgr=""
if [ "$os" = "Darwin" ]; then
  command -v brew >/dev/null 2>&1 && pkg_mgr=brew
elif command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then
  pkg_mgr=apt
elif command -v dnf >/dev/null 2>&1; then
  pkg_mgr=dnf
fi

# Package name, per manager, for each driver we know how to fetch.
pkg_for() {  # pkg_for <driver> -> package name, or "" when this manager has none
  case "$pkg_mgr:$1" in
    apt:sqlite) echo libsqliteodbc ;;   apt:postgres) echo odbc-postgresql ;;   apt:mysql) echo odbc-mariadb ;;
    dnf:sqlite) echo sqliteodbc ;;      dnf:postgres) echo postgresql-odbc ;;   dnf:mysql) echo mariadb-connector-odbc ;;
    brew:sqlite) echo sqliteodbc ;;     brew:postgres) echo psqlodbc ;;         brew:mysql) echo mariadb-connector-odbc ;;
    *) echo "" ;;
  esac
}

# The driver library's basename, which is the same on every platform.
lib_for() {
  case "$1" in
    sqlite) echo libsqlite3odbc ;;
    postgres) echo psqlodbcw ;;
    mysql) echo libmaodbc ;;
    clickhouse) echo libclickhouseodbcw ;;
  esac
}

pkg_installed() {  # pkg_installed <package>
  case "$pkg_mgr" in
    apt) dpkg -s "$1" >/dev/null 2>&1 ;;
    dnf) rpm -q "$1" >/dev/null 2>&1 ;;
    brew) brew list --versions "$1" >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}

# Where the package put the driver library.
pkg_lib_path() {  # pkg_lib_path <package> <lib basename>
  local p
  case "$pkg_mgr" in
    apt) p="$(dpkg -L "$1" 2>/dev/null | grep -E "/$2\.so$" | head -n1)" ;;
    dnf) p="$(rpm -ql "$1" 2>/dev/null | grep -E "/$2\.so$" | head -n1)" ;;
    brew) p="$(find "$(brew --prefix "$1" 2>/dev/null)" -name "$2.dylib" -o -name "$2.so" 2>/dev/null | head -n1)" ;;
  esac
  [ -n "${p:-}" ] && [ -e "$p" ] && echo "$p"
}

# Any shared library the driver needs that the loader cannot find.
missing_deps() {  # missing_deps <library path>
  if [ "$os" = "Darwin" ]; then
    return 0  # otool cannot tell "missing" from "found" without loading; skip
  elif command -v ldd >/dev/null 2>&1; then
    ldd "$1" 2>/dev/null | awk '/not found/ {print $1}' | tr '\n' ' '
  fi
}

run_pkg_mgr() {  # run_pkg_mgr <package>...
  case "$pkg_mgr" in
    apt) $SUDO apt-get install -y "$@" ;;
    dnf) $SUDO dnf install -y "$@" ;;
    brew) brew install "$@" ;;
  esac
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}

# clickhouse-odbc: the project publishes one zip per OS holding a tar.xz and its
# .sha256.  Unpack under $PREFIX/odbc-drivers; the library is self-contained
# (its C++ runtime is linked in), so no package manager is involved.
install_clickhouse() {
  local osname sha url zip dir tarball
  case "$os" in
    Linux) osname=Linux; sha="$CH_ODBC_SHA256_LINUX"; zip="clickhouse-odbc-linux-Clang-UnixODBC-Release.zip" ;;
    Darwin) osname=Darwin; sha="$CH_ODBC_SHA256_DARWIN"; zip="clickhouse-odbc-macos-AppleClang-UnixODBC-Release.zip" ;;
    *) echo "    no clickhouse-odbc build for $os"; return 1 ;;
  esac
  if [ "$(uname -m)" != "x86_64" ] && [ "$os" = "Linux" ]; then
    echo "    clickhouse-odbc publishes Linux binaries for x86_64 only (this is $(uname -m)); build it from source instead"
    return 1
  fi
  dir="$PREFIX/odbc-drivers"
  tarball="clickhouse-odbc-$CH_ODBC_VER-$osname.tar.xz"
  local lib="$dir/clickhouse-odbc-$CH_ODBC_VER-$osname/lib/$(lib_for clickhouse)"
  if [ "$os" = "Darwin" ]; then lib="$lib.dylib"; else lib="$lib.so"; fi
  if [ -e "$lib" ]; then echo "    already unpacked: $lib"; echo "$lib" > "$dir/.clickhouse"; return 0; fi
  for tool in curl unzip tar; do
    command -v $tool >/dev/null 2>&1 || { echo "    $tool is needed to fetch clickhouse-odbc"; return 1; }
  done
  url="https://github.com/ClickHouse/clickhouse-odbc/releases/download/$CH_ODBC_TAG/$zip"
  mkdir -p "$dir"
  local tmp; tmp="$(mktemp -d)"
  echo "    downloading $url"
  curl -fsSL -o "$tmp/$zip" "$url" || { echo "    download failed"; rm -rf "$tmp"; return 1; }
  (cd "$tmp" && unzip -oq "$zip" "$tarball") || { echo "    $tarball not in the zip"; rm -rf "$tmp"; return 1; }
  local got; got="$(sha256_of "$tmp/$tarball")"
  if [ "$got" != "$sha" ]; then
    echo "    SHA-256 mismatch for $tarball: expected $sha, got $got -- not installed" >&2
    rm -rf "$tmp"; return 1
  fi
  tar -xJf "$tmp/$tarball" -C "$dir"
  rm -rf "$tmp"
  [ -e "$lib" ] || { echo "    unpacked, but $lib is missing"; return 1; }
  echo "$lib" > "$dir/.clickhouse"
  echo "    unpacked: $lib (SHA-256 verified)"
}

# Results, one line per driver: "<driver>|<status>|<path>|<registered name>|<note>"
results=()

install_drivers() {
  echo "==> Drivers        ($drivers)"
  if [ -z "$pkg_mgr" ]; then
    case "$os" in
      Darwin) echo "    Homebrew is not installed, so sqliteodbc, psqlodbc and mariadb-connector-odbc cannot be fetched: https://brew.sh" ;;
      *) echo "    no apt or dnf here; install libsqliteodbc / odbc-postgresql / odbc-mariadb (or your distribution's names) by hand" ;;
    esac
  fi
  local d pkg lib path name deps
  IFS=, read -r -a wanted <<< "$drivers"
  for d in "${wanted[@]}"; do
    d="$(echo "$d" | tr -d ' ')"
    case "$d" in
      sqlite|postgres|mysql)
        pkg="$(pkg_for "$d")"; lib="$(lib_for "$d")"
        if [ -z "$pkg" ]; then results+=("$d|skipped||| no package manager"); continue; fi
        echo "  - $d ($pkg)"
        if pkg_installed "$pkg"; then
          echo "    already installed"
        else
          if [ -n "$SUDO" ] && [ "$pkg_mgr" != brew ] && ! command -v "$SUDO" >/dev/null 2>&1; then
            echo "    needs root and $SUDO is not available; run as root:  $pkg_mgr install $pkg"
            results+=("$d|skipped||| run: $pkg_mgr install $pkg"); continue
          fi
          if ! run_pkg_mgr "$pkg"; then results+=("$d|failed||| $pkg_mgr install $pkg failed"); continue; fi
        fi
        path="$(pkg_lib_path "$pkg" "$lib" || true)"
        if [ -z "$path" ]; then results+=("$d|installed||| $pkg installed but $lib not found in its file list"); continue; fi
        deps="$(missing_deps "$path")"
        name="$(registered_name "$path")"
        if [ -n "$deps" ]; then results+=("$d|broken|$path|$name| missing: $deps")
        else results+=("$d|ok|$path|$name|"); fi ;;
      clickhouse)
        echo "  - clickhouse (clickhouse-odbc $CH_ODBC_VER)"
        if install_clickhouse; then
          path="$(cat "$PREFIX/odbc-drivers/.clickhouse")"
          deps="$(missing_deps "$path")"
          if [ -n "$deps" ]; then results+=("clickhouse|broken|$path|| missing: $deps")
          else results+=("clickhouse|ok|$path||"); fi
        else
          results+=("clickhouse|failed|||")
        fi ;;
      *) echo "  - $d: not one of sqlite, postgres, mysql, clickhouse"; results+=("$d|skipped||| unknown driver") ;;
    esac
  done
}

# The name odbcinst.ini registered the library under, if any ("Driver=<name>" then
# works as well as the path).  Distribution packages register themselves; Homebrew
# formulae and the ClickHouse tarball do not, and the path is what to use.
registered_name() {  # registered_name <library path>
  command -v odbcinst >/dev/null 2>&1 || return 0
  local n drv base; base="$(basename "$1")"
  for n in $(odbcinst -q -d 2>/dev/null | tr -d '[]' | tr ' ' '\001'); do
    n="$(echo "$n" | tr '\001' ' ')"
    # Debian registers the bare file name (Driver=psqlodbcw.so) and lets the driver
    # manager's search path find it; Fedora writes the full path.  Accept either.
    drv="$(odbcinst -q -d -n "$n" 2>/dev/null | sed -n 's/^Driver\(64\)\{0,1\}=//p' | head -n1)"
    if [ -n "$drv" ] && { [ "$drv" = "$1" ] || [ "$(basename "$drv")" = "$base" ]; }; then echo "$n"; return 0; fi
  done
}

# ---------------------------------------------------------------------------

[ "$want_bridge" = 1 ] && install_bridge
[ "$want_drivers" = 1 ] && install_drivers

manifest="$MANIFEST_DIR/odbc.toml"
lib="$(bridge_lib || true)"
if [ ! -f "$manifest" ] || [ -z "$lib" ] || [ ! -f "$lib" ]; then
  if [ "$want_bridge" = 1 ]; then
    echo "install.sh: install did not produce a usable manifest" >&2; exit 1
  fi
  echo "install.sh: the bridge is not installed yet (no $manifest); run ./install.sh first" >&2
fi

cat <<EOF

adbcbridge installed.

  driver    $lib
  manifest  $manifest

The manifest directory is searched by the ADBC driver manager automatically,
so the driver is now available under the name "odbc":

  Python  dbapi.connect(driver="odbc", db_kwargs={"uri": "Driver=SQLite3;Database=my.db;"})
  R       adbc_database_init(adbc_driver("odbc"), uri = "Driver=SQLite3;Database=my.db;")
  Go      drv.NewDatabase(map[string]string{"driver": "odbc", "uri": "Driver=SQLite3;Database=my.db;"})

Replace the uri with an ODBC connection string for your data source; "Driver="
takes either a registered ODBC driver name or the path to its library.
EOF

if [ "$want_drivers" = 1 ]; then
  echo
  echo "ODBC drivers:"
  echo
  sqlite_drv="SQLite3"
  for r in "${results[@]}"; do
    IFS='|' read -r d st path name note <<< "$r"
    printf "  %-11s %-9s %s%s%s\n" "$d" "$st" "${name:+\"$name\"  }" "$path" "$note"
    [ "$d" = sqlite ] && [ "$st" = ok ] && sqlite_drv="${name:-$path}"
  done
  cat <<EOF

Connection strings (Driver= takes the registered name where one is shown, or the path):

  SQLite      Driver=$sqlite_drv;Database=/tmp/t.db;
  PostgreSQL  Driver=<name or path>;Server=127.0.0.1;Port=5432;Database=postgres;Uid=postgres;Pwd=...;
  MySQL       Driver=<name or path>;Server=127.0.0.1;Port=3306;Database=test;User=root;Password=...;
  ClickHouse  Driver=<path>;Url=http://127.0.0.1:8123;Database=default;Uid=default;Pwd=;

A driver marked "broken" is installed but needs a library the loader cannot find;
the note names it.
EOF
fi

cat <<EOF

Try it:

  pip install adbc-driver-manager pyarrow
  python -c "import adbc_driver_manager.dbapi as d; c=d.connect(driver='odbc', db_kwargs={'uri':'Driver=${sqlite_drv:-SQLite3};Database=/tmp/t.db;'}); print(c.adbc_get_info()['driver_name'])"
EOF
