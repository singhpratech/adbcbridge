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

//! Build script: with the `bundled` feature, compile `libadbc_driver_odbc`
//! from the C sources in `csrc/` into `OUT_DIR` and hand its path to the
//! library as the `ADBCBRIDGE_BUNDLED_DRIVER` compile-time environment
//! variable.  Without the feature this script does nothing.
//!
//! The compile mirrors the top-level `CMakeLists.txt`: C11 (GNU dialect where
//! the compiler has one), `ADBC_EXPORTING`, `NANOARROW_DLL=` (empty), hidden
//! symbol visibility, `-Wall -Wextra -Wno-unused-parameter`, and a link
//! against the ODBC driver manager plus `dl`/`pthread` where those are
//! separate libraries.  `_GNU_SOURCE` and `ADBC_ODBC_HAVE_PREFETCH` are
//! defined by the sources themselves, exactly as in the CMake build.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    #[cfg(feature = "bundled")]
    bundled::build();
}

#[cfg(feature = "bundled")]
mod bundled {
    use std::env;
    use std::ffi::OsString;
    use std::path::{Path, PathBuf};
    use std::process::Command;

    /// Sources of the driver proper, relative to `csrc/`.  Same list as the
    /// `adbc_driver_odbc` target in `CMakeLists.txt`.
    const SOURCES: &[&str] = &[
        "src/odbc_driver.c",
        "src/odbc_delegate.c",
        "src/odbc_reader.c",
        "src/odbc_partition.c",
        "src/odbc_objects.c",
        "src/odbc_bind.c",
        "src/odbc_text.c",
        "src/utils.c",
        "vendor/nanoarrow/nanoarrow.c",
    ];

    /// Environment variables an integrator can set to point the build at a
    /// non-default ODBC driver manager (for example `ADBCBRIDGE_ODBC_LIB=iodbc`).
    const ENV_ODBC_LIB: &str = "ADBCBRIDGE_ODBC_LIB";
    const ENV_ODBC_INCLUDE_DIR: &str = "ADBCBRIDGE_ODBC_INCLUDE_DIR";
    const ENV_ODBC_LIB_DIR: &str = "ADBCBRIDGE_ODBC_LIB_DIR";

    pub fn build() {
        for var in [ENV_ODBC_LIB, ENV_ODBC_INCLUDE_DIR, ENV_ODBC_LIB_DIR] {
            println!("cargo:rerun-if-env-changed={var}");
        }
        let manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
        let csrc = manifest_dir.join("csrc");
        println!("cargo:rerun-if-changed={}", csrc.display());
        let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
        let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
        let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();

        let odbc = OdbcLocation::detect(&target_os);

        let mut build = cc::Build::new();
        build
            .files(SOURCES.iter().map(|s| csrc.join(s)))
            .include(csrc.join("include"))
            .include(csrc.join("vendor"))
            .include(csrc.join("src"))
            .define("ADBC_EXPORTING", None)
            .define("NANOARROW_DLL", Some(""))
            .pic(true)
            .warnings(true)
            .flag_if_supported("-Wno-unused-parameter")
            .flag_if_supported("-fvisibility=hidden")
            // The crate itself never links the driver (it is dlopen()ed by the
            // ADBC driver manager at run time), so keep cc's link lines out of
            // cargo's ears.
            .cargo_metadata(false);
        if let Some(dir) = &odbc.include_dir {
            build.include(dir);
        }
        let tool = build
            .try_get_compiler()
            .unwrap_or_else(|e| panic!("adbcbridge: no C compiler: {e}"));
        if tool.is_like_msvc() {
            build.std("c11");
        } else {
            // CMake's C_STANDARD 11 with the default C_EXTENSIONS ON.
            build.std("gnu11");
        }
        // The CMake build defaults to Release; a driver at -O0 is not useful
        // even in a debug cargo profile, so never go below -O2.
        if env::var("OPT_LEVEL").map(|o| o == "0").unwrap_or(true) {
            build.opt_level(2);
        }

        let objects = match build.try_compile_intermediates() {
            Ok(objects) => objects,
            Err(e) => panic!(
                "adbcbridge: compiling the bundled ODBC driver failed: {e}\n\
                 The ODBC driver-manager headers (sql.h, sqlext.h) are needed: \
                 install unixodbc-dev / unixODBC-devel / unixodbc (brew), or point \
                 {ENV_ODBC_INCLUDE_DIR} at them.  Alternatively build the crate with \
                 `--no-default-features` and provide libadbc_driver_odbc yourself \
                 (see `adbcbridge::driver_path`)."
            ),
        };

        let library = out_dir.join(library_name(&target_os));
        let status = if tool.is_like_msvc() {
            link_msvc(&library, &objects, &odbc)
        } else {
            link_gnu(&tool, &library, &objects, &odbc, &target_os, &target_env)
        };
        match status {
            Ok(()) => {}
            Err(e) => panic!(
                "adbcbridge: linking the bundled ODBC driver failed: {e}\n\
                 The ODBC driver-manager library (lib{}) is needed: install \
                 unixodbc-dev / unixODBC-devel / unixodbc (brew), or set \
                 {ENV_ODBC_LIB} / {ENV_ODBC_LIB_DIR}.",
                odbc.lib
            ),
        }
        println!(
            "cargo:rustc-env=ADBCBRIDGE_BUNDLED_DRIVER={}",
            library.display()
        );
    }

    /// File name the driver manager, the Python package and the CMake build
    /// all agree on (`PREFIX "lib"` on every platform, Windows included).
    fn library_name(target_os: &str) -> &'static str {
        match target_os {
            "windows" => "libadbc_driver_odbc.dll",
            "macos" | "ios" => "libadbc_driver_odbc.dylib",
            _ => "libadbc_driver_odbc.so",
        }
    }

    /// Where the ODBC driver manager lives.
    struct OdbcLocation {
        /// Library base name: `odbc` (unixODBC), `iodbc`, or `odbc32`.
        lib: String,
        include_dir: Option<PathBuf>,
        lib_dir: Option<PathBuf>,
    }

    impl OdbcLocation {
        fn detect(target_os: &str) -> Self {
            let lib = env::var(ENV_ODBC_LIB)
                .ok()
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| {
                    if target_os == "windows" {
                        "odbc32".to_string()
                    } else {
                        "odbc".to_string()
                    }
                });
            let mut include_dir = env::var_os(ENV_ODBC_INCLUDE_DIR).map(PathBuf::from);
            let mut lib_dir = env::var_os(ENV_ODBC_LIB_DIR).map(PathBuf::from);
            if target_os == "macos" && (include_dir.is_none() || lib_dir.is_none()) {
                // Homebrew's unixodbc keg (Apple Silicon and Intel prefixes) and
                // MacPorts; Apple ships no ODBC headers of its own.
                for prefix in ["/opt/homebrew", "/usr/local", "/opt/local"] {
                    let prefix = Path::new(prefix);
                    if prefix.join("include").join("sql.h").is_file() {
                        include_dir.get_or_insert_with(|| prefix.join("include"));
                        lib_dir.get_or_insert_with(|| prefix.join("lib"));
                        break;
                    }
                }
            }
            OdbcLocation {
                lib,
                include_dir,
                lib_dir,
            }
        }
    }

    fn run(mut cmd: Command) -> Result<(), String> {
        let rendered = format!("{cmd:?}");
        match cmd.status() {
            Ok(status) if status.success() => Ok(()),
            Ok(status) => Err(format!("{rendered} exited with {status}")),
            Err(e) => Err(format!("could not run {rendered}: {e}")),
        }
    }

    fn link_gnu(
        tool: &cc::Tool,
        library: &Path,
        objects: &[PathBuf],
        odbc: &OdbcLocation,
        target_os: &str,
        target_env: &str,
    ) -> Result<(), String> {
        let mut cmd = tool.to_command();
        if target_os == "macos" || target_os == "ios" {
            cmd.arg("-dynamiclib");
            cmd.arg("-Wl,-install_name,@rpath/libadbc_driver_odbc.dylib");
        } else {
            cmd.arg("-shared");
        }
        cmd.arg("-o").arg(library);
        cmd.args(objects);
        if let Some(dir) = &odbc.lib_dir {
            let mut flag = OsString::from("-L");
            flag.push(dir);
            cmd.arg(flag);
        }
        cmd.arg(format!("-l{}", odbc.lib));
        if target_os != "windows" {
            // Native delegation dlopen()s the ADBC driver manager and guards it
            // with pthread primitives; both live in libc on modern glibc, musl
            // and the BSDs, but are still separate archives elsewhere.
            if target_os == "linux" || target_os == "android" {
                cmd.arg("-ldl");
            }
            if target_os != "android" {
                cmd.arg("-lpthread");
            }
        } else if target_env == "gnu" {
            // MinGW: the CRT does not pull these in on its own.
            cmd.arg("-static-libgcc");
        }
        run(cmd)
    }

    fn link_msvc(library: &Path, objects: &[PathBuf], odbc: &OdbcLocation) -> Result<(), String> {
        let target = env::var("TARGET").unwrap_or_default();
        let linker = cc::windows_registry::find_tool(&target, "link.exe").ok_or_else(|| {
            "link.exe not found (is the Visual Studio C++ toolchain installed?)".to_string()
        })?;
        let mut cmd = linker.to_command();
        cmd.arg("/NOLOGO").arg("/DLL");
        let mut out = OsString::from("/OUT:");
        out.push(library);
        cmd.arg(out);
        cmd.args(objects);
        if let Some(dir) = &odbc.lib_dir {
            let mut flag = OsString::from("/LIBPATH:");
            flag.push(dir);
            cmd.arg(flag);
        }
        cmd.arg(format!("{}.lib", odbc.lib));
        run(cmd)
    }
}
