<!-- SPDX-License-Identifier: Apache-2.0 -->
# Installing on Linux

This page takes you from a machine with nothing installed to a working
adbcBridge that a five-line Python program can query. There are three
ingredients, installed in this order:

1. an **ODBC driver manager** (the system library that loads database drivers);
2. one or more **vendor ODBC drivers** (one per database you want to reach);
3. **adbcBridge** itself (the single library that presents ODBC to Arrow ADBC).

Every command below is copy-paste ready. Package names are shown for
Debian/Ubuntu (`apt`) and Fedora/RHEL (`dnf`); translate to your distribution as
needed.

## 1. The ODBC driver manager: unixODBC

ODBC (Open Database Connectivity) drivers are loaded by a *driver manager*. On
Linux the driver manager is **unixODBC**. (There is a second driver manager,
iODBC, but it is not needed on Linux — every driver in adbcBridge's test suite
loads through unixODBC. iODBC only matters on macOS, and only for a few vendor
drivers; see [Installing on macOS](install-macos.md).)

Install the runtime and the development headers — you need the headers only if
you will build adbcBridge from source:

```sh
# Debian/Ubuntu
sudo apt install unixodbc unixodbc-dev

# Fedora/RHEL
sudo dnf install unixODBC unixODBC-devel
```

Confirm it is there and see where it keeps its configuration:

```sh
odbcinst -j
```

`odbcinst -j` prints the paths of `odbcinst.ini` (the file that lists installed
**drivers**) and `odbc.ini` (the file that lists named **data sources**, or
DSNs). You will not usually edit these by hand — most driver packages register
themselves — but knowing where they are makes troubleshooting far easier.

## 2. Example ODBC drivers

You need one vendor driver per database. These are the ones adbcBridge's own
test suite uses, all installable from distribution packages or vendor
repositories:

| Database | Driver | Debian/Ubuntu package |
|---|---|---|
| SQLite | sqliteodbc | `libsqliteodbc` |
| PostgreSQL | psqlodbc | `odbc-postgresql` |
| MySQL / MariaDB | MariaDB Connector/ODBC | `odbc-mariadb` |
| ClickHouse | clickhouse-odbc | vendor package (`clickhouse-odbc`) |
| SQL Server | msodbcsql18 | `msodbcsql18` (Microsoft's `packages.microsoft.com` repo) |

```sh
# Debian/Ubuntu: the three that are in the base repositories
sudo apt install libsqliteodbc odbc-postgresql odbc-mariadb
```

msodbcsql18 (Microsoft's SQL Server driver) and clickhouse-odbc come from vendor
repositories; follow each vendor's Linux instructions. After installing any
driver, check that the driver manager registered it:

```sh
odbcinst -q -d      # list the driver names now in odbcinst.ini
```

> **Tip:** A driver name from this list (for example `SQLite3` or `PostgreSQL
> Unicode`) is what you put after `Driver=` in a connection string. You can also
> put the full path to the driver's `.so` there instead — adbcBridge accepts
> either. See [Connection strings](../reference/connection-strings.md).

## 3. adbcBridge

Pick whichever of these fits how you work. All four leave you with the same
library and, except where noted, the same by-name discovery.

### From the GitHub Release (prebuilt library)

Each release attaches a `.tar.gz` holding the prebuilt library for a platform.
On Linux the assets are named `adbcbridge-<tag>-linux-x64.tar.gz` (x86-64) and
`adbcbridge-<tag>-linux-arm64.tar.gz` (ARM64), built on `manylinux_2_28` so they
run on any reasonably recent glibc-based distribution.

```sh
tar xzf adbcbridge-v0.1.0-linux-x64.tar.gz
# → linux-x64/libadbc_driver_odbc.so
```

Point adbcBridge's clients at it with the `ADBC_ODBC_DRIVER` environment
variable (see [environment variables](#environment-variables-the-library-and-bindings-honour)),
or copy it into `~/.local/lib` and write a manifest as the [source](#from-source-cmake)
and [`install.sh`](#via-installsh) paths do.

### From the Python wheel (bundles the library)

The Python wheel carries the compiled library inside it, so `pip install` gives
you both the binding and the driver in one step:

```sh
pip install adbcbridge          # once published to PyPI
# or, from a downloaded release wheel (the aarch64 wheel is named likewise):
pip install adbcbridge-0.1.0-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.manylinux_2_28_x86_64.whl
pip install adbc-driver-manager pyarrow
```

The wheel's own `adbcbridge.driver_path()` finds the bundled library, so nothing
else has to be set. The native library still needs unixODBC present at run time
(`libodbc.so.2`) and the vendor driver for your database — those are never
bundled.

### From source (cmake)

You need a C compiler, CMake 3.16+, and `unixodbc-dev`:

```sh
git clone https://github.com/singhpratech/adbcbridge && cd adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DADBCBRIDGE_MANIFEST_DIR="$HOME/.config/adbc/drivers"
cmake --build build -j
cmake --install build --prefix "$HOME/.local"
```

`cmake --install` writes two things: the library into
`lib/libadbc_driver_odbc.so` under the prefix, and the driver manifest into
`~/.config/adbc/drivers/odbc.toml`. `ADBCBRIDGE_MANIFEST_DIR` is given as an
absolute path because its default (`etc/adbc/drivers`, relative to the prefix)
would put the manifest in `~/.local/etc/adbc/drivers`, a directory the ADBC
driver manager does not search, so `driver="odbc"` would not resolve. See
[The driver manifest](#the-adbc-driver-manifest) for how that manifest makes
`driver="odbc"` resolve.

### Via install.sh

`install.sh` is the no-root, no-thinking path: it configures, builds, and
installs into your home directory, and writes the manifest into the ADBC user
config directory so discovery just works.

```sh
./install.sh
```

It puts the library in `~/.local/lib/libadbc_driver_odbc.so` (`lib64` on
Fedora/RHEL-style 64-bit systems) and the manifest in
`~/.config/adbc/drivers/odbc.toml`, then prints both paths. Re-running it is
safe — it reconfigures the same build tree and overwrites the same two files. It
honours these environment overrides:

| Variable | Meaning | Default |
|---|---|---|
| `PREFIX` | install prefix for the library | `$HOME/.local` |
| `MANIFEST_DIR` | directory to write `odbc.toml` into | `${XDG_CONFIG_HOME:-$HOME/.config}/adbc/drivers` |
| `BUILD_DIR` | CMake build tree | `<repo>/build` |
| `BUILD_TYPE` | CMake build type | `Release` |
| `JOBS` | parallel build jobs | `nproc` |

> **Troubleshooting:** If `install.sh` stops with `cmake not found`, install the
> build prerequisites first — on Debian/Ubuntu `sudo apt install cmake
> unixodbc-dev`.

## The ADBC driver manifest

A *driver manifest* is a small TOML file that lets the ADBC driver manager find
a driver by a short name. adbcBridge installs one called `odbc.toml`, so any
ADBC client can load the bridge as `driver="odbc"` with no path and no
environment variable.

### Where it is installed

- `cmake --install --prefix P` writes it to `P/etc/adbc/drivers/odbc.toml`
  unless `-DADBCBRIDGE_MANIFEST_DIR` points elsewhere, as the recipe above does.
- `install.sh` writes it to `~/.config/adbc/drivers/odbc.toml` — the ADBC
  *user config directory*, which the driver manager searches automatically.

### What is inside

The manifest names the library per platform, under a `[Driver.shared]` key that
is an operating-system-plus-architecture tuple:

```toml
[Driver.shared]
linux_amd64 = '/home/you/.local/lib/libadbc_driver_odbc.so'
```

The key is `linux_amd64` on a normal (glibc) x86-64 system and
`linux_arm64` on ARM64. On a **musl**-based distribution (Alpine, for example)
the driver manager appends a `_musl` suffix and looks up `linux_amd64_musl`
instead — adbcBridge detects musl the same way the driver manager does and
writes the matching key, so a manifest built on musl carries `linux_amd64_musl`.

> **Troubleshooting:** If `driver="odbc"` silently loads the wrong library (or
> unixODBC's own `libodbc.so`), the manifest's platform key probably does not
> match what the driver manager is looking up. Rebuild adbcBridge on the target
> machine so the key is generated for that libc, rather than copying a manifest
> between glibc and musl systems.

### How `driver="odbc"` resolves

When a client asks for `driver="odbc"`, the ADBC driver manager searches its
manifest directories — `$ADBC_DRIVER_PATH`, then `~/.config/adbc/drivers`, then
`/etc/adbc/drivers`, and a few more — for `odbc.toml`, reads the path under the
platform key, and loads that library. Because `install.sh` writes the manifest
into one of those default directories, `driver="odbc"` works with **no**
`ADBC_DRIVER_PATH` and **no** `LD_LIBRARY_PATH` set.

## Environment variables the library and bindings honour

You rarely need any of these once the manifest is installed, but they are the
escape hatches:

| Variable | Read by | Effect |
|---|---|---|
| `ADBC_ODBC_DRIVER` | every binding's driver finder | Absolute path to `libadbc_driver_odbc.so` to use. Set but non-existent is an error, not a silent fallback. |
| `ADBC_DRIVER_PATH` | the ADBC driver manager, and adbcBridge's delegation search | Extra directory (or directories) to search for driver manifests. |
| `ADBCBRIDGE_LIBRARY` | the Rust, C#, Java and Go finders | Same idea as `ADBC_ODBC_DRIVER`; where both are read, the order is documented on each language page. |
| `ADBCBRIDGE_PRELOAD` | the Python package | `0` disables the automatic ODBC-driver preload that `adbcbridge.connect()` does before importing pyarrow. |
| `ADBC_ODBC_DELEGATE` | the driver | `never` / `auto` / `always` for [native delegation](../how-it-works/delegation.md). |
| `ADBC_ODBC_DELEGATE_PATH` | the driver | Extra directories to search for native ADBC drivers to delegate to (ignored for setuid/setcap processes). |

## Verifying the install

### Check the ODBC layer with the driver manager's own tools

`odbcinst -j` shows the driver manager is present and where its config lives.
`isql` (shipped with unixODBC) opens an interactive session against a DSN, which
proves the vendor driver and server work *before* adbcBridge is in the picture:

```sh
odbcinst -j
isql -v <your-DSN>          # then type: SELECT 1;  and: quit
```

### Check adbcBridge with five lines of Python

```sh
pip install adbc-driver-manager pyarrow
```

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(driver="odbc",
                   db_kwargs={"uri": "Driver=SQLite3;Database=/tmp/t.db;"}) as conn:
    print(conn.adbc_get_info()["driver_name"])
```

It prints `ADBC ODBC Driver` (or, if a native driver was installed and
[delegation](../how-it-works/delegation.md) took over, the native driver's name). If
you installed the Python wheel you can use its convenience wrapper instead —
`import adbcbridge; adbcbridge.connect(uri="Driver=SQLite3;Database=/tmp/t.db;")`
— which additionally finds the bundled library for you.

> **Tip:** The `adbcbridge` command line (from the Python package) has two
> checks worth knowing: `adbcbridge drivers` lists the ODBC drivers the driver
> manager can see, and `adbcbridge driver-path` prints which
> `libadbc_driver_odbc.so` would be loaded. See
> [the command-line tool](../languages/python.md#the-command-line-tool) and
> [environment variables](../reference/options.md#environment-variables).

## Known limitations

- **The driver manager and vendor drivers are yours.** adbcBridge never bundles
  a driver manager or a vendor ODBC driver; it loads whatever is installed. A
  missing or misconfigured vendor driver is reported through adbcBridge but
  fixed in the vendor driver's own setup.
- **A driver present but failing to load** is reported by unixODBC as the
  misleading `Can't open lib '<path>' : file not found`, even when the file is
  there. adbcBridge re-opens the path itself and adds the real reason to the
  error. The most common real causes are a static-TLS conflict after importing
  pyarrow, a missing dependency of the driver (`ldd <driver.so>`), and a 32-bit
  driver in a 64-bit process. See [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).
- **32-bit-`SQLLEN` drivers** (IBM Db2's `libdb2.so` is the known case) need the
  `adbc.odbc.sqllen_32bit` quirk, which is autodetected for Db2 and can be set
  by hand for others. See [Options](../reference/options.md).
- Full per-driver quirks and the exact verified set are in
  [COMPATIBILITY.md](../COMPATIBILITY.md).
