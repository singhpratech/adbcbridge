#!/bin/sh
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

# Refresh rust/csrc/ from the driver sources at the repository root.
#
# A crates.io package may only contain files under the crate directory, so
# the `bundled` feature compiles from this copy rather than from ../src.  Run
# this after touching src/, include/ or vendor/nanoarrow/; the test in
# tests/csrc_in_sync.rs fails until the copy matches again.

set -eu

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
csrc="$here/csrc"

for dir in src include vendor/nanoarrow; do
    if [ ! -d "$repo/$dir" ]; then
        echo "sync-csrc.sh: $repo/$dir is missing; is $repo a checkout of adbcbridge?" >&2
        exit 1
    fi
done

rm -rf "$csrc"
mkdir -p "$csrc/vendor"
cp -R "$repo/src" "$csrc/src"
cp -R "$repo/include" "$csrc/include"
cp -R "$repo/vendor/nanoarrow" "$csrc/vendor/nanoarrow"
cp "$repo/LICENSE" "$csrc/LICENSE"
cp "$repo/NOTICE" "$csrc/NOTICE"

# Nothing but sources belongs in the copy (editor swap files, object files
# from an in-tree build, ...).
find "$csrc" -type f ! -name '*.c' ! -name '*.h' ! -name LICENSE ! -name NOTICE -delete

echo "sync-csrc.sh: copied src/, include/, vendor/nanoarrow/, LICENSE and NOTICE into $csrc"
