<!-- SPDX-License-Identifier: Apache-2.0 -->
# Building from source

adbcBridge is a single plain-C11 shared library, `libadbc_driver_odbc.so`
(`.dylib` on macOS, `.dll` on Windows), built with CMake. It links only an ODBC
(Open Database Connectivity) driver manager and the C standard library; the Arrow
support it needs is vendored (nanoarrow, under `vendor/`). This page covers the
prerequisites, the build and install commands, every CMake option, the test
suite, the release workflow, and how each language binding is packaged from
source.

---

## Prerequisites

You need a C11 compiler, CMake 3.16 or newer, and an ODBC driver manager with its
development headers (`sql.h`, `sqlext.h`).

| Platform | Install |
|---|---|
| Debian / Ubuntu | `sudo apt install cmake unixodbc-dev` |
| Red Hat / Fedora | `sudo dnf install cmake unixODBC-devel` |
| macOS | `brew install cmake unixodbc` |
| Windows | CMake and a C toolchain (MSVC or MinGW); the OS ships the ODBC driver manager (`odbc32`), no separate install needed |

The driver manager can be unixODBC or iODBC on POSIX, and Windows' own on
Windows. CMake finds it via `find_package(ODBC)`, falling back to searching for
`sql.h` and a library named `odbc`, `odbc32`, or `iodbc`. On macOS, point CMake at
Homebrew's unixODBC with `-DCMAKE_PREFIX_PATH="$(brew --prefix unixodbc)"`.

To *use* the driver you also need at least one vendor ODBC driver for the database
you are connecting to; that is separate from building adbcBridge itself.

---

## Configure, build, install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
cmake --install build --prefix /your/prefix
```

The default build type is `Release` if none is given. The install lays out:

| Component | Destination |
|---|---|
| the shared library | `<prefix>/lib` (`<prefix>/bin` on Windows) |
| the ADBC driver manifest `odbc.toml` | `<prefix>/etc/adbc/drivers` (configurable) |

### Quick user install

For a no-root install into your home directory, `install.sh` wraps the three
commands above and writes the manifest into the ADBC per-user config directory so
the driver is immediately loadable by the name `odbc`:

```sh
./install.sh
```

It installs the library under `~/.local/lib` and the manifest under
`~/.config/adbc/drivers` (or `~/Library/Application Support/ADBC/Drivers` on
macOS). Its behaviour is tunable through environment variables: `PREFIX`,
`MANIFEST_DIR`, `BUILD_DIR`, `BUILD_TYPE`, and `JOBS`.

### The driver manifest

`ADBCBRIDGE_INSTALL_MANIFEST` (on by default) installs a TOML manifest named
`odbc.toml`. The ADBC driver manager discovers a driver by finding
`<name>.toml` in one of its search directories, so installing it as `odbc.toml`
lets any ADBC binding load adbcBridge simply as `driver="odbc"` — no
`ADBC_DRIVER_PATH` or `LD_LIBRARY_PATH` needed.

The manifest is expanded from `adbc_driver_odbc.toml.in` at **install** time (not
configure time), because the absolute library path it must contain is only known
once `cmake --install --prefix` has chosen the final prefix. It carries a
platform tuple key such as `linux_amd64` under `[Driver.shared]`; CMake derives
the OS, architecture, and a libc suffix (`_musl`, `_mingw`) so the key matches
exactly what the driver manager looks up — a mismatch would make discovery
silently fall through to some other library called "odbc".

---

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `ADBC_ODBC_BUILD_SHARED` | `ON` | Build the shared library. |
| `ADBC_ODBC_BUILD_TESTS` | `ON` | Build the C smoke test and the C unit tests. |
| `ADBC_ODBC_ASAN` | `OFF` | Build with AddressSanitizer and UndefinedBehaviorSanitizer (requires GCC or Clang). |
| `ADBCBRIDGE_BUILD_SHARED` | `ON` | Build the shared library (a second shared-build toggle alongside `ADBC_ODBC_BUILD_SHARED`). |
| `ADBCBRIDGE_INSTALL_MANIFEST` | `ON` | Install the `odbc.toml` ADBC driver manifest. |
| `ADBCBRIDGE_MANIFEST_DIR` | `etc/adbc/drivers` | Where to install the manifest, relative to the install prefix (or an absolute path, as `install.sh` uses to place it in the user config dir). |

Standard CMake variables also apply: `CMAKE_BUILD_TYPE`, `CMAKE_INSTALL_PREFIX`,
`CMAKE_PREFIX_PATH` (to find unixODBC), and the install-time
`cmake --install --prefix`.

---

## Test suite

Tests are built when `ADBC_ODBC_BUILD_TESTS` is `ON` and run with `ctest`:

```sh
ctest --test-dir build --output-on-failure
```

### C unit tests

These compile the driver's own sources and exercise pieces of it in isolation,
with no database or ODBC driver required. Each is both a `ctest` target and a
source file under `tests/c/`:

| Target | Source | Exercises |
|---|---|---|
| `test_utf16` | `test_utf16.c` | The UTF-8 ↔ SQLWCHAR (wide-character) codecs used on the parameter and connect paths. |
| `test_types` | `test_types.c` | The type-mapping logic (see [Type mapping](types.md)). |
| `test_sqllen32` | `test_sqllen32.c` | The 32-bit-`SQLLEN` accessors that read a narrow driver's lengths and indicators. |
| `test_objects` | `test_objects.c` | The `GetObjects` catalog/schema/table metadata assembly. |
| `test_errors` | `test_errors.c` | Mapping ODBC diagnostic records to ADBC errors and status codes. |
| `test_multirow` | `test_multirow.c` | The multi-row `INSERT` batching used by bulk ingest. |
| `test_partition` | `test_partition.c` | Splitting a query into partitions for `ExecutePartitions`. |

When the ODBC headers define `SQL_WCHART_CONVERT` (a four-byte SQLWCHAR, as iODBC
always has), two extra targets — `test_utf16_wchar32` and `test_multirow_wchar32`
— rebuild those two tests against the four-byte width, so the wide-character
codecs are proven for an iODBC-style build without needing iODBC present.

### C smoke test

`adbc_odbc_c_smoke` (`tests/c/test_driver.c`) is a dependency-free test that
`dlopen`s the built driver and drives the ADBC 1.1.0 vtable directly. It is
POSIX-only (it uses `dlopen`/`mkdtemp`) and is skipped on Windows. It needs a
SQLite ODBC driver, named by the `SQLITE_ODBC_DRIVER` environment variable (a
path or a registered driver name); with none set it reports "skipped".

```sh
SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so ctest --test-dir build
```

The test build also produces two helper libraries never installed:
`adbc_fake_native_driver` (a stand-in native ADBC driver for the delegation
tests) and, on ELF/glibc, a pair of TLS libraries that reproduce a specific
loader failure for `tests/test_driver_load_errors.py`.

### Python integration tests and the compatibility matrix

The end-to-end tests under `tests/` (`test_sqlite.py`, `test_delegate.py`,
`test_long_columns.py`, `test_partitions.py`, `test_prefetch.py`,
`test_pg_array_ingest.py`, `test_parallel_ingest.py`, `test_windows_text.py`,
`test_plug_and_play.py`, `test_driver_load_errors.py`) drive the built library
through the ADBC Python driver manager. They read:

- `ADBC_ODBC_DRIVER` — path to the built `libadbc_driver_odbc.so`;
- `SQLITE_ODBC_DRIVER` / `POSTGRES_ODBC_DRIVER` / … — the vendor ODBC driver for
  the database under test.

The compatibility matrix runner, `tests/compat/test_matrix.py`, exercises all 46
databases (see [Connection strings](connection-strings.md)). It reads:

- `ADBC_ODBC_DRIVER` — the driver under test (default
  `<repo>/build/libadbc_driver_odbc.so`);
- `<NAME>_ODBC_DRIVER` — each database's vendor ODBC driver, which also gates
  whether that entry runs;
- `<NAME>_CONN` — an optional per-entry connection-string override;
- `ADBC_MATRIX_SUFFIX` — a table-name suffix to isolate concurrent runs.

The servers themselves are brought up by `tests/compat/docker-compose.yml`, which
defines one service per database with the ports the matrix templates connect to.
See `tests/compat/README.md` for how the fleet is started and which vendor ODBC
drivers each entry expects.

---

## Release workflow

`.github/workflows/ci.yml` builds and tests on every push and pull request across
Ubuntu, macOS, and Windows (x64 and Win32), runs the C unit tests everywhere, and
on Linux additionally installs a SQLite ODBC driver to run the Python tests and
verify manifest discovery.

`.github/workflows/release.yml` runs on a `v*` tag (or as a dry run via
`workflow_dispatch`) and produces, for each platform, on a GitHub Release:

| Job | Artifact |
|---|---|
| `linux` (x86_64, aarch64, manylinux_2_28) | the `.so`, an auditwheel-repaired Python wheel, and `adbcbridge-<tag>-<rid>.tar.gz` |
| `macos` (arm64) | the `.dylib`, a delocated wheel, and `…-osx-arm64.tar.gz` |
| `windows` (x64) | the `.dll`, a wheel, and `…-win-x64.tar.gz` |
| `sdist` | a Python source distribution |
| `crate` | the Rust crate (`cargo package`) |
| `nuget` | a NuGet package bundling the four native libraries |
| `maven` | a Maven jar bundling the four native libraries |

The wheels bundle the driver library but deliberately **exclude** the OS ODBC
driver manager (`libodbc`), so the user's own `odbcinst.ini` still governs which
vendor drivers are visible. Native libraries move between jobs through the Release
itself; a dry run builds everything against a draft release that the final job
deletes. PyPI publishing is a separate workflow (`publish-pypi.yml`) using trusted
publishing.

---

## Packaging each language binding from source

Every binding wraps the same C library. They obtain it in one of two ways: some
compile it from a bundled copy of the C sources, others bundle a prebuilt native
library. Each binding locates the library at run time via `ADBCBRIDGE_LIBRARY` /
`ADBC_ODBC_DRIVER`, the ADBC manifest, or a bundled copy (see the environment
variables in [Options](options.md)).

### Rust (`rust/`)

The crate's default `bundled` feature compiles the driver from `rust/csrc/` at
build time via a `build.rs` that mirrors the CMake build (C11, `ADBC_EXPORTING`,
hidden visibility, linking the ODBC driver manager plus `dl`/`pthread`). Because a
crates.io package may only contain files under the crate directory, `csrc/` is a
copy of the repository's `src/`, `include/`, and `vendor/nanoarrow/`; refresh it
with `rust/sync-csrc.sh` after touching those, and a test (`tests/csrc_in_sync.rs`)
fails until the copy matches. Package with:

```sh
cd rust && cargo package
```

### Python (`python/`)

A thin wrapper that ships the prebuilt shared library inside the wheel. `setup.py`
takes the library path from `ADBCBRIDGE_LIBRARY` (or finds it under
`ADBCBRIDGE_BUILD_DIR`, default `<repo>/build`). Build with:

```sh
ADBCBRIDGE_LIBRARY=$PWD/build/libadbc_driver_odbc.so python -m build --wheel python
```

The release repairs the wheel with auditwheel (Linux) or delocate (macOS),
excluding `libodbc`.

### .NET (`csharp/`)

`dotnet pack` builds the NuGet package; a `NativeRoot` property points it at a
directory of per-runtime native libraries to bundle under `runtimes/`:

```sh
dotnet pack csharp/AdbcBridge -c Release -p:NativeRoot=/path/to/natives
```

### Java (`java/`)

`mvn package` builds the jar; the `adbcbridge.natives` property points it at a
directory of native libraries to bundle under `native/`:

```sh
mvn -f java/pom.xml package -Dadbcbridge.natives=/path/to/natives
```

### Go (`go/`)

An ordinary `go build` loads the driver from an external path or the ADBC
manifest at run time. Building with `-tags adbcbridge_embed` instead embeds the
native libraries staged under `go/internal/native/<goos>_<goarch>/` into the
binary and extracts the right one at run time:

```sh
go build -tags adbcbridge_embed ./...
```

Without the tag, or with no library staged, the build still compiles and simply
falls back to locating the driver via the manifest and install directories.
