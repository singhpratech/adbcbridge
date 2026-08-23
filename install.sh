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
# Environment overrides:
#   PREFIX        install prefix for the library    (default $HOME/.local)
#   MANIFEST_DIR  directory to write odbc.toml into (default the ADBC user
#                 config dir: ${XDG_CONFIG_HOME:-$HOME/.config}/adbc/drivers,
#                 or ~/Library/Application Support/ADBC/Drivers on macOS)
#   BUILD_DIR     CMake build tree                  (default <repo>/build)
#   BUILD_TYPE    CMake build type                  (default Release)
#   JOBS          parallel build jobs               (default nproc)

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

PREFIX="${PREFIX:-$HOME/.local}"
BUILD_DIR="${BUILD_DIR:-$here/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

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

command -v cmake >/dev/null 2>&1 || {
  echo "install.sh: cmake not found. Install it first:" >&2
  echo "  Debian/Ubuntu: sudo apt install cmake unixodbc-dev" >&2
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

manifest="$MANIFEST_DIR/odbc.toml"
# Read the library path back out of the manifest we just wrote, so what we
# print is what the driver manager will actually load.  Match on the library
# basename rather than on "first quoted value": the metadata keys above
# [Driver.shared] (name, version, url, ...) are quoted the same way.
lib=""
if [ -f "$manifest" ]; then
  lib="$(grep -o "'[^']*libadbc_driver_odbc[^']*'" "$manifest" | tr -d "'" | head -n1)"
fi

if [ ! -f "$manifest" ] || [ -z "$lib" ] || [ ! -f "$lib" ]; then
  echo "install.sh: install did not produce a usable manifest" >&2
  exit 1
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
takes either a registered ODBC driver name or the path to its .so.

Try it:

  pip install adbc-driver-manager pyarrow
  python -c "import adbc_driver_manager.dbapi as d; c=d.connect(driver='odbc', db_kwargs={'uri':'Driver=SQLite3;Database=/tmp/t.db;'}); print(c.adbc_get_info()['driver_name'])"
EOF
