<!-- SPDX-License-Identifier: Apache-2.0 -->
# Installing on macOS

This page mirrors [Installing on Linux](install-linux.md) for macOS. The shape
is the same — a driver manager, then vendor drivers, then adbcBridge — with one
extra wrinkle that is unique to macOS: some vendor drivers ship built against a
*different* ODBC driver manager (iODBC) than the default one (unixODBC), and the
two are not interchangeable. That is explained in full below.

> **Note:** The prebuilt macOS release library is **Apple Silicon (arm64) only**.
> There is no Intel (x86-64) build. On an Intel Mac you must build from source
> (`cmake`), and against whichever driver manager your vendor drivers use.

## 1. The ODBC driver manager

macOS has two ODBC driver managers, and which one you need depends on your
vendor drivers.

### 🍎 unixODBC (the default)

Most drivers work through **unixODBC**, the same driver manager Linux uses.
Install it with Homebrew:

```sh
brew install unixodbc
```

Confirm it and find its config paths:

```sh
odbcinst -j
```

`odbcinst -j` prints where `odbcinst.ini` (installed drivers) and `odbc.ini`
(named data sources / DSNs) live. On a Homebrew install these are under the
Homebrew prefix.

### When you need a bridge built against iODBC instead

A handful of vendor drivers for macOS are compiled against **iODBC**, the *other*
ODBC driver manager, and only ship in that form. The reason is a low-level
incompatibility: unixODBC's wide-character type (`SQLWCHAR`) is two bytes
(UTF-16), while iODBC's is four bytes (`wchar_t`). A driver built for one
**cannot** be loaded through the other — the wide entry points exchange text in
the wrong unit size, and diagnostics come back unreadable. Relinking such a
driver is not a valid workaround.

The vendor drivers known to be iODBC-only on macOS are:

| Driver | Fronts |
|---|---|
| MySQL Connector/ODBC for macOS | MySQL and every MySQL-wire database |
| OpenLink Virtuoso | Virtuoso |
| Arrow Flight SQL ODBC (Dremio's build) | Flight SQL, InfluxDB 3, Dremio |

If you need one of these, build **a second copy of adbcBridge against iODBC** and
use the iODBC drivers through it, keeping the unixODBC-built bridge for
everything else. One bridge build per driver manager.

```sh
# Build iODBC from source first (e.g. from the openlink/iODBC tag:
#   ./configure --disable-gui && make install), then:
cmake -S . -B build-iodbc -DCMAKE_BUILD_TYPE=Release \
  -DODBC_INCLUDE_DIR=<iodbc>/include -DODBC_LIBRARY=<iodbc>/lib/libiodbc.dylib
cmake --build build-iodbc
ctest --test-dir build-iodbc
```

Then set `ADBC_ODBC_DRIVER` to `build-iodbc/libadbc_driver_odbc.dylib` when you
use an iODBC driver. The exact per-driver preparation steps (clearing the
download quarantine flag, adding rpaths, re-signing) and the reasons behind them
are in [TROUBLESHOOTING.md](../TROUBLESHOOTING.md), under the two macOS iODBC
sections, and the measured facts are in
[bench/BENCHMARKS-macos.md](../../bench/BENCHMARKS-macos.md).

> **Troubleshooting:** Two symptoms mean you have an iODBC driver loaded through
> a unixODBC-built bridge. First, every call fails with an *empty* diagnostic
> like `[H000] [ (0) (SQLDriverConnect)`. Second, the process dies with SIGABRT
> — no message — on the first SQL statement that errors on the server (a `DROP`
> of a missing table, say). unixODBC's own `isql` dies the same way. The fix is
> the iODBC-built bridge above.

## 2. Example ODBC drivers

Install vendor drivers per database. For the ones that are UTF-16 (unixODBC)
drivers, Homebrew is the easiest route:

```sh
brew install sqliteodbc      # SQLite, through unixODBC
brew install psqlodbc        # PostgreSQL, through unixODBC
```

Microsoft's msodbcsql18 for SQL Server ships as an arm64 tarball you relink to
your driver manager. The MySQL, Virtuoso and Flight SQL drivers are the
iODBC-only cases from the table above. After installing a driver, confirm the
manager registered it:

```sh
odbcinst -q -d      # driver names in odbcinst.ini
```

## 3. adbcBridge

### From the GitHub Release (prebuilt library)

The macOS release asset is `adbcbridge-<tag>-osx-arm64.tar.gz`, containing
`osx-arm64/libadbc_driver_odbc.dylib`. It is built against unixODBC, for Apple
Silicon.

```sh
shasum -a 256 -c SHA256SUMS --ignore-missing   # SHA256SUMS is on the same release page
tar xzf adbcbridge-v0.1.0-osx-arm64.tar.gz
# → osx-arm64/libadbc_driver_odbc.dylib
```

### From the Python wheel (bundles the library)

The macOS wheel is tagged **`macosx_14_0_arm64`** — that is, it targets macOS
14.0 or later on Apple Silicon (`MACOSX_DEPLOYMENT_TARGET` is `14.0`; the
release build passes `_PYTHON_HOST_PLATFORM=macosx-14.0-arm64`, which the wheel
tag normalises to).

```sh
pip install adbcbridge          # from PyPI
# or a downloaded release wheel:
pip install adbcbridge-0.1.0-py3-none-macosx_14_0_arm64.whl
pip install adbc-driver-manager pyarrow
```

> **Note (the universal caveat):** the wheel is **arm64 only**, not a
> universal2 (fat) wheel. Python interpreters from python.org are often
> universal2 builds; the release build tags the wheel `macosx_14_0_arm64`
> explicitly so packaging tools do not demand a nonexistent x86-64 slice. On an
> Intel Mac, or under an x86-64 (Rosetta) Python, this wheel will not match —
> build from source instead.

### From source (cmake)

```sh
brew install cmake unixodbc
git clone https://github.com/singhpratech/adbcbridge && cd adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DADBCBRIDGE_MANIFEST_DIR="$HOME/Library/Application Support/ADBC/Drivers"
cmake --build build -j
cmake --install build --prefix "$HOME/.local"
```

`ADBCBRIDGE_MANIFEST_DIR` is given as an absolute path because its default
(`etc/adbc/drivers`, relative to the prefix) would put the manifest in
`~/.local/etc/adbc/drivers`, a directory the ADBC driver manager does not
search, so `driver="odbc"` would not resolve.

To build against iODBC instead, pass `-DODBC_INCLUDE_DIR` and `-DODBC_LIBRARY`
as shown in [the iODBC section](#when-you-need-a-bridge-built-against-iodbc-instead).

### Via install.sh

```sh
./install.sh
```

On macOS `install.sh` writes the library under `~/.local/lib` and the manifest
into the ADBC user config directory, which on macOS is
`~/Library/Application Support/ADBC/Drivers/odbc.toml`. The `PREFIX`,
`MANIFEST_DIR`, `BUILD_DIR`, `BUILD_TYPE` and `JOBS` overrides from the
[Linux page](install-linux.md#via-installsh) apply unchanged.

## The ADBC driver manifest

The manifest works exactly as on [Linux](install-linux.md#the-adbc-driver-manifest),
with two macOS specifics:

- The user config directory is `~/Library/Application Support/ADBC/Drivers`
  (not `~/.config`). `install.sh` and the driver manager both use it.
- The `[Driver.shared]` platform key is **`macos_arm64`**:

```toml
[Driver.shared]
macos_arm64 = '/Users/you/.local/lib/libadbc_driver_odbc.dylib'
```

Once the manifest is in place, every ADBC client loads the bridge as
`driver="odbc"` with no path and no environment variable.

## Environment variables

The same variables the [Linux page lists](install-linux.md#environment-variables-the-library-and-bindings-honour)
apply on macOS. `ADBC_ODBC_DRIVER` is especially useful here: point it at the
unixODBC-built `.dylib` for most work, and at the `build-iodbc` one when you use
an iODBC driver.

## Verifying the install

```sh
odbcinst -j                 # driver manager present, config paths
isql -v <your-DSN>          # exercise the vendor driver + server directly
pip install adbc-driver-manager pyarrow
```

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(driver="odbc",
                   db_kwargs={"uri": "Driver=SQLite3;Database=/tmp/t.db;"}) as conn:
    print(conn.adbc_get_info()["driver_name"])
```

## Known limitations

- **Apple Silicon only** for the prebuilt library and wheel; no Intel build is
  published.
- **Two driver managers, two bridge builds.** A driver built against iODBC needs
  an iODBC-built bridge; a driver built against unixODBC needs the unixODBC-built
  bridge. You cannot mix a driver and a bridge across the two managers.
- **The verified count on macOS is 45** of the 53-database workload; the
  remaining eight lack an obtainable driver or a runnable server on this platform
  (for example, some drivers ship no macOS build, and some server images do not
  run under Docker Desktop's virtual machine). The per-database detail, including
  the macOS column, is in [COMPATIBILITY.md](../COMPATIBILITY.md).
- The same driver-load and static-TLS caveats as Linux apply; see
  [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).
