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

//! Find and load **adbcbridge**, the [ADBC](https://arrow.apache.org/adbc/)
//! driver that talks to any ODBC data source, from Rust.
//!
//! The driver itself is a C shared library (`libadbc_driver_odbc.so`,
//! `.dylib` or `.dll`).  This crate does two small things:
//!
//! * [`driver_path()`] finds a copy of that library, and
//! * [`load()`] hands it to [`adbc_driver_manager`] as a [`ManagedDriver`],
//!   after which the ordinary [`adbc_core`] traits ([`Driver`], [`Database`],
//!   [`Connection`], [`Statement`]) do the rest.
//!
//! With the default `bundled` feature the library is compiled from the C
//! sources shipped inside the crate when the crate is built, so nothing has to
//! be installed beyond a C compiler and the ODBC driver-manager development
//! files (unixODBC or iODBC on Unix; the SDK on Windows).  Without the feature
//! the crate only locates a driver that is already on the machine.
//!
//! # Example: `SELECT 1` through the SQLite ODBC driver
//!
//! The ODBC connection string goes in as the ADBC `uri`.  `SQLITE_ODBC_DRIVER`
//! holds the path (or registered name) of the SQLite ODBC driver; the example
//! simply returns when it is not set.
//!
//! ```
//! use adbc_core::options::OptionDatabase;
//! use adbc_core::{Connection, Database, Driver, Statement};
//!
//! fn main() -> Result<(), Box<dyn std::error::Error>> {
//!     let Some(sqlite) = std::env::var_os("SQLITE_ODBC_DRIVER") else {
//!         return Ok(());
//!     };
//!     let mut driver = adbcbridge::load()?;
//!     let uri = format!("Driver={};Database=:memory:;", sqlite.to_string_lossy());
//!     let database = driver.new_database_with_opts([(OptionDatabase::Uri, uri.into())])?;
//!     let mut connection = database.new_connection()?;
//!     let mut statement = connection.new_statement()?;
//!     statement.set_sql_query("SELECT 1 AS one")?;
//!     let mut rows = 0;
//!     for batch in statement.execute()? {
//!         rows += batch?.num_rows();
//!     }
//!     assert_eq!(rows, 1);
//!     Ok(())
//! }
//! ```
//!
//! [`Driver`]: adbc_core::Driver
//! [`Database`]: adbc_core::Database
//! [`Connection`]: adbc_core::Connection
//! [`Statement`]: adbc_core::Statement

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::env;
use std::ffi::OsString;
use std::fmt;
use std::path::{Path, PathBuf};

use adbc_core::options::AdbcVersion;
pub use adbc_driver_manager::ManagedDriver;

// The driver manager types this crate hands out come from these exact crate
// versions; re-exporting them lets a program name them without having to pin
// the same versions itself.
pub use adbc_core;
pub use adbc_driver_manager;

/// Environment variable naming the driver library, checked first.
const ENV_LIBRARY: &str = "ADBCBRIDGE_LIBRARY";
/// Environment variable naming the driver library, shared with the Python
/// package, the benchmarks and the C test suite; checked second.
const ENV_DRIVER: &str = "ADBC_ODBC_DRIVER";
/// Environment variable naming extra ADBC driver-manifest directories.
const ENV_ADBC_DRIVER_PATH: &str = "ADBC_DRIVER_PATH";
/// The ADBC driver manifest installed by `cmake --install`.
const MANIFEST_NAME: &str = "odbc.toml";
/// Symbol the driver manager calls to fill in the ADBC vtable.
const ENTRYPOINT: &[u8] = b"AdbcDriverInit";

/// Why [`driver_path`] or [`load`] failed.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// `ADBCBRIDGE_LIBRARY` or `ADBC_ODBC_DRIVER` is set but does not name a file.
    BadEnvironment {
        /// The variable's name.
        variable: &'static str,
        /// Its value.
        value: OsString,
    },
    /// No copy of the driver library was found anywhere it is looked for.
    NotFound {
        /// Every place that was checked, in order.
        searched: Vec<PathBuf>,
    },
    /// The library was found but the ADBC driver manager could not load it.
    Adbc(adbc_core::error::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::BadEnvironment { variable, value } => write!(
                f,
                "{variable}={} does not point at a file",
                Path::new(value).display()
            ),
            Error::NotFound { searched } => {
                write!(
                    f,
                    "could not find {}: set {ENV_LIBRARY} or {ENV_DRIVER} to its path, \
                     build the crate with the `bundled` feature, or install the driver \
                     (cmake --install build); searched",
                    library_names()[0]
                )?;
                for path in searched {
                    write!(f, "\n  {}", path.display())?;
                }
                Ok(())
            }
            Error::Adbc(e) => write!(f, "could not load the adbcbridge driver: {e}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Adbc(e) => Some(e),
            _ => None,
        }
    }
}

impl From<adbc_core::error::Error> for Error {
    fn from(e: adbc_core::error::Error) -> Self {
        Error::Adbc(e)
    }
}

/// Absolute path of the adbcbridge shared library.
///
/// Looked up in this order, the first hit winning:
///
/// 1. the `ADBCBRIDGE_LIBRARY` environment variable, then `ADBC_ODBC_DRIVER`
///    (either is an error rather than a miss if set to something that is not a
///    file);
/// 2. the copy compiled into this crate's build directory by the `bundled`
///    feature;
/// 3. the ADBC driver manifest named `odbc` (`odbc.toml`) in the directories
///    the ADBC driver manager searches: `ADBC_DRIVER_PATH`, the active
///    `VIRTUAL_ENV`/`CONDA_PREFIX`, the per-user directory
///    (`$XDG_CONFIG_HOME/adbc/drivers`, or `~/Library/Application
///    Support/ADBC/Drivers` on macOS) and the system ones under `/etc` and
///    `/usr`;
/// 4. common install directories (`$VIRTUAL_ENV/lib`, `/usr/local/lib`,
///    `/usr/lib`, `/opt/adbcbridge/lib`, `/opt/homebrew/lib`, the multiarch
///    `lib` directories, and a CMake `build/` tree next to a source checkout).
pub fn driver_path() -> Result<PathBuf, Error> {
    let mut searched = Vec::new();
    for variable in [ENV_LIBRARY, ENV_DRIVER] {
        let Some(value) = env::var_os(variable).filter(|v| !v.is_empty()) else {
            continue;
        };
        let path = PathBuf::from(&value);
        if path.is_file() {
            return Ok(absolute(path));
        }
        return Err(Error::BadEnvironment { variable, value });
    }
    if let Some(path) = bundled_driver() {
        if path.is_file() {
            return Ok(path.to_path_buf());
        }
        searched.push(path.to_path_buf());
    }
    for dir in manifest_dirs() {
        let manifest = dir.join(MANIFEST_NAME);
        if searched.contains(&manifest) {
            continue;
        }
        let Ok(text) = std::fs::read_to_string(&manifest) else {
            searched.push(manifest);
            continue;
        };
        searched.push(manifest);
        if let Some(library) = manifest_library(&text, &platform_keys()) {
            let path = PathBuf::from(library);
            if path.is_file() {
                return Ok(absolute(path));
            }
            searched.push(path);
        }
    }
    for dir in install_dirs() {
        for name in library_names() {
            let candidate = dir.join(name);
            if candidate.is_file() {
                return Ok(absolute(candidate));
            }
            searched.push(candidate);
        }
    }
    Err(Error::NotFound { searched })
}

/// Load the adbcbridge driver found by [`driver_path`] into the ADBC driver
/// manager (entry point `AdbcDriverInit`, ADBC version 1.1.0).
pub fn load() -> Result<ManagedDriver, Error> {
    load_from(driver_path()?)
}

/// Load the adbcbridge driver library at `path`, skipping the lookup that
/// [`load`] does (entry point `AdbcDriverInit`, ADBC version 1.1.0).
pub fn load_from(path: impl AsRef<Path>) -> Result<ManagedDriver, Error> {
    ManagedDriver::load_dynamic_from_filename(path.as_ref(), Some(ENTRYPOINT), AdbcVersion::V110)
        .map_err(Error::Adbc)
}

/// The copy of the driver compiled by `build.rs` under the `bundled` feature,
/// if this crate was built with it.  The path is baked in at compile time.
fn bundled_driver() -> Option<&'static Path> {
    option_env!("ADBCBRIDGE_BUNDLED_DRIVER").map(Path::new)
}

fn absolute(path: PathBuf) -> PathBuf {
    std::fs::canonicalize(&path).unwrap_or(path)
}

/// Library file names to look for, most likely first.
fn library_names() -> &'static [&'static str] {
    if cfg!(windows) {
        &["adbc_driver_odbc.dll", "libadbc_driver_odbc.dll"]
    } else if cfg!(target_os = "macos") {
        &["libadbc_driver_odbc.dylib"]
    } else {
        &["libadbc_driver_odbc.so"]
    }
}

fn home_dir() -> Option<PathBuf> {
    let var = if cfg!(windows) { "USERPROFILE" } else { "HOME" };
    env::var_os(var)
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

/// Prefixes an installer is likely to have been pointed at
/// (`cmake --install build --prefix "$VIRTUAL_ENV"`).
fn env_prefixes() -> Vec<PathBuf> {
    ["VIRTUAL_ENV", "CONDA_PREFIX"]
        .iter()
        .filter_map(env::var_os)
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .collect()
}

/// Directories the ADBC driver manager searches for `odbc.toml`.
fn manifest_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    if let Some(list) = env::var_os(ENV_ADBC_DRIVER_PATH) {
        dirs.extend(env::split_paths(&list).filter(|p| !p.as_os_str().is_empty()));
    }
    for prefix in env_prefixes() {
        dirs.push(prefix.join("etc").join("adbc").join("drivers"));
        dirs.push(prefix.join("share").join("adbc").join("drivers"));
    }
    if cfg!(target_os = "macos") {
        if let Some(home) = home_dir() {
            dirs.push(home.join("Library/Application Support/ADBC/Drivers"));
        }
        dirs.push(PathBuf::from("/Library/Application Support/ADBC/Drivers"));
    } else if !cfg!(windows) {
        let config = env::var_os("XDG_CONFIG_HOME")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .or_else(|| home_dir().map(|h| h.join(".config")));
        if let Some(config) = config {
            dirs.push(config.join("adbc").join("drivers"));
        }
    }
    if !cfg!(windows) {
        dirs.extend(
            [
                "/etc/adbc/drivers",
                "/usr/local/etc/adbc/drivers",
                "/usr/share/adbc/drivers",
                "/usr/local/share/adbc/drivers",
            ]
            .iter()
            .map(PathBuf::from),
        );
    }
    dirs
}

/// Keys accepted under `[Driver.shared]` for this machine, most canonical
/// first: the ADBC spelling (`linux_amd64`), the `uname -m` spelling a
/// hand-written manifest may use (`linux_x86_64`), and, where the driver
/// manager adds one, the libc suffix (`linux_amd64_musl`, `windows_amd64_mingw`).
fn platform_keys() -> Vec<String> {
    let os = match env::consts::OS {
        "macos" | "ios" => "macos",
        "windows" => "windows",
        "freebsd" => "freebsd",
        "openbsd" => "openbsd",
        _ => "linux",
    };
    let arches: &[&str] = match env::consts::ARCH {
        "x86_64" => &["amd64", "x86_64"],
        "x86" => &["x86", "i386", "i686"],
        "aarch64" => &["arm64", "aarch64"],
        "arm" => &["arm", "armv7l", "armv6l"],
        "powerpc64" if cfg!(target_endian = "little") => &["powerpc64le", "ppc64le"],
        "powerpc64" => &["powerpc64", "ppc64"],
        "powerpc" => &["powerpc", "ppc"],
        "riscv64" => &["riscv64"],
        "s390x" => &["s390x"],
        other => return vec![format!("{os}_{other}")],
    };
    let suffix = if cfg!(target_env = "musl") {
        "_musl"
    } else if cfg!(all(windows, target_env = "gnu")) {
        "_mingw"
    } else {
        ""
    };
    let mut keys = Vec::new();
    for arch in arches {
        if !suffix.is_empty() {
            keys.push(format!("{os}_{arch}{suffix}"));
        }
        keys.push(format!("{os}_{arch}"));
    }
    keys
}

/// Strip the quotes off a TOML basic or literal string; `None` for anything
/// that is not a plain one-line string.
fn toml_string(value: &str) -> Option<String> {
    let value = value.trim();
    let quote = value.chars().next()?;
    if quote != '"' && quote != '\'' {
        return None;
    }
    let rest = &value[1..];
    let end = rest.find(quote)?;
    let body = &rest[..end];
    if quote == '"' && body.contains('\\') {
        // Basic strings may escape backslashes (Windows paths) and quotes.
        let mut out = String::new();
        let mut chars = body.chars();
        while let Some(c) = chars.next() {
            if c == '\\' {
                match chars.next() {
                    Some('n') => out.push('\n'),
                    Some('t') => out.push('\t'),
                    Some(other) => out.push(other),
                    None => break,
                }
            } else {
                out.push(c);
            }
        }
        return Some(out);
    }
    Some(body.to_string())
}

/// The `[Driver.shared]` entry for this platform in a driver manifest, or the
/// bare `shared = "path"` form the spec also allows.  The manifests written by
/// `cmake --install` are flat, so a line scanner is enough; an inline table
/// (`shared = { linux_amd64 = "..." }`) is handled too.
fn manifest_library(text: &str, keys: &[String]) -> Option<String> {
    let mut section = String::new();
    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(rest) = line.strip_prefix('[') {
            section = rest
                .split(']')
                .next()
                .unwrap_or("")
                .trim()
                .trim_matches('"')
                .to_string();
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim().trim_matches('"').trim_matches('\'');
        let value = value.trim();
        match section.as_str() {
            "Driver.shared" if keys.iter().any(|k| k == key) => {
                if let Some(path) = toml_string(value) {
                    return Some(path);
                }
            }
            "Driver" if key == "shared" => {
                if let Some(path) = toml_string(value) {
                    return Some(path);
                }
                if let Some(table) = value.strip_prefix('{') {
                    // `{ linux_amd64 = "/a", macos_arm64 = "/b" }`
                    for entry in table.trim_end_matches('}').split(',') {
                        let Some((k, v)) = entry.split_once('=') else {
                            continue;
                        };
                        let k = k.trim().trim_matches('"').trim_matches('\'');
                        if keys.iter().any(|key| key == k) {
                            if let Some(path) = toml_string(v) {
                                return Some(path);
                            }
                        }
                    }
                }
            }
            _ => {}
        }
    }
    None
}

/// Common install locations, plus a CMake build tree next to a source checkout.
fn install_dirs() -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    for prefix in env_prefixes() {
        dirs.push(prefix.join("lib"));
        dirs.push(prefix.join("lib64"));
        if cfg!(windows) {
            dirs.push(prefix.join("bin"));
            dirs.push(prefix.join("Library").join("bin"));
        }
    }
    if !cfg!(windows) {
        for base in ["/usr/local", "/usr", "/opt/adbcbridge", "/opt/homebrew"] {
            dirs.push(Path::new(base).join("lib"));
            dirs.push(Path::new(base).join("lib64"));
        }
    }
    if cfg!(target_os = "linux") {
        // Debian multiarch: x86_64-linux-gnu, aarch64-linux-gnu, ...
        let machine = match env::consts::ARCH {
            "x86" => "i386",
            "arm" => "arm",
            other => other,
        };
        let triplet = if machine == "arm" {
            "arm-linux-gnueabihf".to_string()
        } else {
            format!("{machine}-linux-gnu")
        };
        dirs.push(Path::new("/usr/lib").join(&triplet));
        dirs.push(Path::new("/usr/local/lib").join(&triplet));
    }
    // `cmake --build build` in a checkout of the repository this crate lives
    // in: the crate sits at <repo>/rust, so the build tree is a sibling.
    if let Some(repo) = Path::new(env!("CARGO_MANIFEST_DIR")).parent() {
        let build = repo.join("build");
        dirs.push(build.clone());
        for sub in ["Release", "Debug", "RelWithDebInfo", "MinSizeRel"] {
            dirs.push(build.join(sub));
        }
    }
    dirs
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_flat_table() {
        let text = "name = 'odbc'\n[Driver]\n[Driver.shared]\nlinux_amd64 = \"/opt/x/libadbc_driver_odbc.so\"\nmacos_arm64 = '/opt/y.dylib'\n";
        let keys = vec!["linux_amd64".to_string()];
        assert_eq!(
            manifest_library(text, &keys).as_deref(),
            Some("/opt/x/libadbc_driver_odbc.so")
        );
        let keys = vec!["macos_arm64".to_string()];
        assert_eq!(
            manifest_library(text, &keys).as_deref(),
            Some("/opt/y.dylib")
        );
        let keys = vec!["windows_amd64".to_string()];
        assert_eq!(manifest_library(text, &keys), None);
    }

    #[test]
    fn manifest_bare_and_inline() {
        let keys = vec!["linux_amd64".to_string()];
        assert_eq!(
            manifest_library("[Driver]\nshared = \"/a/b.so\"  # comment\n", &keys).as_deref(),
            Some("/a/b.so")
        );
        assert_eq!(
            manifest_library(
                "[Driver]\nshared = { macos_arm64 = '/m', linux_amd64 = \"/l\" }\n",
                &keys
            )
            .as_deref(),
            Some("/l")
        );
        assert_eq!(
            manifest_library(
                "[Driver.shared]\nwindows_amd64 = \"C:\\\\d\\\\x.dll\"\n",
                &["windows_amd64".to_string()]
            )
            .as_deref(),
            Some("C:\\d\\x.dll")
        );
    }

    #[test]
    fn platform_keys_are_well_formed() {
        let keys = platform_keys();
        assert!(!keys.is_empty());
        for key in &keys {
            assert!(key.contains('_'), "{key}");
        }
        if cfg!(all(
            target_os = "linux",
            target_arch = "x86_64",
            target_env = "gnu"
        )) {
            assert_eq!(keys[0], "linux_amd64");
            assert!(keys.contains(&"linux_x86_64".to_string()));
        }
    }

    #[test]
    fn bad_environment_is_an_error() {
        // No other unit test reads the environment, so mutating it here is
        // safe even though tests share a process.
        env::set_var(ENV_LIBRARY, "/nonexistent/libadbc_driver_odbc.so");
        let result = driver_path();
        env::remove_var(ENV_LIBRARY);
        match result {
            Err(Error::BadEnvironment { variable, .. }) => assert_eq!(variable, ENV_LIBRARY),
            other => panic!("expected BadEnvironment, got {other:?}"),
        }
    }

    #[test]
    fn not_found_error_lists_locations() {
        let e = Error::NotFound {
            searched: vec![PathBuf::from("/nowhere/libadbc_driver_odbc.so")],
        };
        let text = e.to_string();
        assert!(text.contains("ADBCBRIDGE_LIBRARY"), "{text}");
        assert!(text.contains("/nowhere/"), "{text}");
    }
}
