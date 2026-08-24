// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

//! `csrc/` must be a byte-for-byte copy of the driver sources at the
//! repository root, so that the `bundled` feature never compiles something
//! other than what `cmake --build` would.  Skipped (with a message) when the
//! crate is built outside a checkout, e.g. from a crates.io package.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

/// (path under the repository root, path under `csrc/`).
const MIRRORED: &[(&str, &str)] = &[
    ("src", "src"),
    ("include", "include"),
    ("vendor/nanoarrow", "vendor/nanoarrow"),
    ("LICENSE", "LICENSE"),
    ("NOTICE", "NOTICE"),
];

/// Files that belong in the copy: what sync-csrc.sh keeps.
fn is_source(path: &Path) -> bool {
    match path.extension().and_then(|e| e.to_str()) {
        Some("c") | Some("h") => true,
        _ => matches!(
            path.file_name().and_then(|n| n.to_str()),
            Some("LICENSE") | Some("NOTICE")
        ),
    }
}

/// Every source file below `root`, keyed by its path relative to `root`.
fn collect(root: &Path, relative: &Path, out: &mut BTreeMap<PathBuf, Vec<u8>>) {
    let full = root.join(relative);
    if full.is_file() {
        if is_source(&full) {
            out.insert(relative.to_path_buf(), fs::read(&full).unwrap());
        }
        return;
    }
    let mut entries: Vec<_> = fs::read_dir(&full)
        .unwrap_or_else(|e| panic!("{}: {e}", full.display()))
        .map(|entry| entry.unwrap().file_name())
        .collect();
    entries.sort();
    for name in entries {
        collect(root, &relative.join(name), out);
    }
}

#[test]
fn csrc_matches_repository_sources() {
    let crate_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let repo = crate_dir.parent().unwrap();
    if !repo.join("CMakeLists.txt").is_file() || !repo.join("src").join("odbc_driver.c").is_file() {
        eprintln!(
            "skipping: {} is not an adbcbridge checkout (no CMakeLists.txt / src/odbc_driver.c)",
            repo.display()
        );
        return;
    }
    let csrc = crate_dir.join("csrc");

    let mut expected = BTreeMap::new();
    let mut actual = BTreeMap::new();
    for (from, to) in MIRRORED {
        let mut files = BTreeMap::new();
        collect(repo, Path::new(from), &mut files);
        for (relative, bytes) in files {
            // Re-root src/foo.c -> csrc/src/foo.c, vendor/nanoarrow/x -> csrc/vendor/nanoarrow/x.
            let under_to = Path::new(to).join(relative.strip_prefix(from).unwrap());
            expected.insert(under_to, bytes);
        }
        if csrc.join(to).exists() {
            collect(&csrc, Path::new(to), &mut actual);
        }
    }

    let mut problems = Vec::new();
    for (path, bytes) in &expected {
        match actual.get(path) {
            None => problems.push(format!("missing from csrc/: {}", path.display())),
            Some(copy) if copy != bytes => {
                problems.push(format!("differs: csrc/{}", path.display()))
            }
            Some(_) => {}
        }
    }
    for path in actual.keys() {
        if !expected.contains_key(path) {
            problems.push(format!(
                "stale, not in the repository: csrc/{}",
                path.display()
            ));
        }
    }
    assert!(
        problems.is_empty(),
        "rust/csrc/ is out of date; run rust/sync-csrc.sh:\n  {}",
        problems.join("\n  ")
    );
    assert!(
        !expected.is_empty(),
        "no sources found under {}",
        repo.display()
    );
}
