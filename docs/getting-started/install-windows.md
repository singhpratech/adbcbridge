<!-- SPDX-License-Identifier: Apache-2.0 -->
# Installing on Windows

Windows differs from Linux and macOS in one fundamental way: the ODBC driver
manager is **part of the operating system**. You do not install one. Everything
else — vendor drivers, then adbcBridge — follows the same shape as the other
platforms, with the Windows-specific details called out below.

## 1. The driver manager: built into Windows

Windows ships its own ODBC driver manager as `odbc32.dll` (the name is
historical; it is the 64-bit driver manager on a 64-bit Windows). There is
nothing to install and nothing named unixODBC or iODBC involved.

Two consequences matter for adbcBridge:

- **The narrow ODBC path is the ANSI code page, not UTF-8.** On Linux and macOS
  the narrow (`SQLCHAR`) ODBC entry points carry UTF-8 bytes untouched. The
  Windows driver manager instead transcodes every narrow string through the
  process's ANSI code page (code page 1252 on a Western install). adbcBridge
  handles this by routing statement text, catalog names, column and type names,
  diagnostics, and character columns through the wide (`W`) entry points on
  Windows, so text is correct end to end. You do not configure anything; this is
  simply why the internal code path differs from the other platforms.
- **Drivers are registered in the registry, by MSI installers.** You cannot
  register a driver just by naming its DLL path in a connection string the way
  unixODBC tolerates — see the next section.

Manage installed drivers and DSNs with the built-in **ODBC Data Source
Administrator** (`odbcad32.exe`, reachable from the Start menu as "ODBC Data
Sources (64-bit)").

## 2. Installing vendor drivers (MSI installers)

On Windows, ODBC drivers come as **MSI installer packages** that register the
driver in the registry. Install the driver for each database you need — for
example Microsoft's msodbcsql18 for SQL Server (Windows already ships a SQL
Server ODBC driver), the MySQL Connector/ODBC MSI, the PostgreSQL psqlODBC MSI,
and so on — then confirm it appears in the ODBC Data Source Administrator.

> **Troubleshooting: `IM002` when you name a DLL by path.** Unlike unixODBC,
> the Windows driver manager will **not** accept a driver referenced only by its
> DLL path — `Driver=C:\path\to\driver.dll;…` is refused with SQLSTATE `IM002`
> ("Data source name not found and no default driver specified"). The driver
> must be registered (installed by its MSI) and named by its registered driver
> name. This is a driver-manager difference, not an adbcBridge limitation; the
> YDB row in [bench/BENCHMARKS-windows.md](../../bench/BENCHMARKS-windows.md)
> records a concrete case.

## 3. adbcBridge

### The library name

On Windows the library is **`libadbc_driver_odbc.dll`** — note the `lib` prefix
is kept on Windows too (the build sets it deliberately, so the file is
`libadbc_driver_odbc.dll`, not `adbc_driver_odbc.dll`).

### From the GitHub Release (prebuilt library)

The Windows release asset is `adbcbridge-<tag>-win-x64.tar.gz`, containing
`win-x64/libadbc_driver_odbc.dll`, built for 64-bit x86 with MSVC.

The release page also carries `SHA256SUMS`; `Get-FileHash adbcbridge-v0.1.0-win-x64.tar.gz -Algorithm SHA256`
in PowerShell gives the value to compare. [Security](../community/security.md#verifying-a-download)
covers checking the file's GPG signature, and the provenance and SBOM that releases after v0.1.0 add.

### From the Python wheel

The Windows wheel bundles the DLL. Install it, plus the ADBC client:

```
pip install adbcbridge          # from PyPI
# or, from a downloaded release wheel:
pip install adbcbridge-0.1.0-py3-none-win_amd64.whl
pip install adbc-driver-manager pyarrow
```

### From source, and the `cmake --install` layout

You need CMake and a C toolchain (see [MSVC vs MinGW](#msvc-vs-mingw)). The
driver manager comes from the Windows SDK's `odbc32`; there is nothing extra to
install for it.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --prefix C:\adbcbridge
```

`cmake --install` lays the files out like this under the prefix:

```
C:\adbcbridge\
  bin\libadbc_driver_odbc.dll        the DLL (Windows puts shared libs in bin\)
  lib\                               import library, if produced
  etc\adbc\drivers\odbc.toml         the driver manifest
```

The manifest's `[Driver.shared]` platform key is **`windows_amd64`**:

```toml
[Driver.shared]
windows_amd64 = 'C:/adbcbridge/bin/libadbc_driver_odbc.dll'
```

Paths in the manifest are TOML literal strings, so forward slashes work on
Windows and no backslash escaping is needed. Once the manifest is in a directory
the ADBC driver manager searches (`%LOCALAPPDATA%\ADBC\Drivers` or
`%PROGRAMDATA%\ADBC\Drivers`), `driver="odbc"` resolves by name.

### 🪟 64- vs 32-bit builds

Windows keeps separate 32-bit and 64-bit ODBC worlds: a 64-bit process needs a
64-bit driver manager and 64-bit drivers, and a 32-bit process needs the 32-bit
ones. The released library is **64-bit x86**. If you build for 32-bit, every
piece — the bridge, the driver manager, and the vendor driver — must be 32-bit
too. A 32-bit driver in a 64-bit process (or the reverse) fails to load.

### MSVC vs MinGW

The releases are built with **MSVC** (the Visual Studio C toolchain). Building
with **MinGW** (a GCC toolchain for Windows) also works, but note one manifest
detail: the driver manager appends a `_mingw` suffix to the platform key for a
MinGW toolchain (`windows_amd64_mingw`), and CMake only recognises a build as
MinGW when the compiler is a GNU one. If you build with MinGW, the manifest key
must be `windows_amd64_mingw` for by-name discovery to match; a mismatched key
means `driver="odbc"` will not find the library.

> **Note:** The Go binding is a special case on Windows — ADBC's Go driver
> manager is a cgo package, and MSVC cannot serve cgo. Building anything that
> imports the Go module needs a GCC (MinGW-w64) on `PATH` with `CGO_ENABLED=1`.
> See [languages/go.md](../languages/go.md).

## Prefetch and parallel ingest on Windows

Two features are compiled out on Windows because both are built on pthreads,
which is a POSIX facility (the guard is `_WIN32`):

- the **prefetch pipeline** (`adbc.odbc.prefetch`), which overlaps ODBC fetches
  with Arrow conversion on a background thread; and
- **parallel bulk ingest** (`adbc.odbc.ingest_connections`), which spreads one
  ingest over several connections. On Windows `ingest_connections` is clamped to
  1.

Everything else — queries, types, parameters, single-connection bulk ingest,
metadata, error mapping — works. Because these two are absent, a Windows read or
ingest measures a materially different code path from the Linux one; keep that in
mind when comparing performance across platforms. A Win32 port of both (using
SRWLOCK, CONDITION_VARIABLE and `_beginthreadex`) is on the roadmap — see
[ROADMAP.md](../ROADMAP.md).

> **Tip: `NO_SSPS=1` for MySQL Connector/ODBC against non-MySQL servers.** The
> Windows MySQL Connector/ODBC needs `NO_SSPS=1` (no server-side prepared
> statements) in the connection string when you point it at a server that speaks
> the MySQL wire protocol but is not MySQL itself — otherwise it fails with `No
> data supplied for parameters in prepared statement`. This is a known driver
> behaviour on Windows, recorded in
> [UPSTREAM.md](../UPSTREAM.md) and
> [bench/BENCHMARKS-windows.md](../../bench/BENCHMARKS-windows.md).

## Verifying the install

Use the built-in ODBC Data Source Administrator (`odbcad32.exe`) to confirm your
vendor driver is registered, then run the same five-line Python check as the
other platforms:

```
pip install adbc-driver-manager pyarrow
```

```python
import adbc_driver_manager.dbapi as dbapi

with dbapi.connect(driver="odbc",
                   db_kwargs={"uri": "Driver=SQLite3;Database=C:/temp/t.db;"}) as conn:
    print(conn.adbc_get_info()["driver_name"])
```

It prints `ADBC ODBC Driver`. (Native delegation is not implemented on Windows,
so the bridge always takes the ODBC path here; see
[Native delegation](../how-it-works/delegation.md).)

## Known limitations

- **No prefetch and no parallel ingest** (pthreads compiled out on `_WIN32`), as
  above.
- **Native delegation is unavailable on Windows** — `adbc.odbc.delegate=auto`
  always takes the ODBC path, and `always` fails with a clear message.
- **Driver registration is stricter**: a driver must be installed by its MSI and
  named by its registered name; a bare DLL path is refused with `IM002`.
- **64-bit release only**; a 32-bit build requires a 32-bit driver manager and
  32-bit drivers throughout.
- Windows-specific driver findings (for example ANSI-code-page conversion issues
  inside certain vendor drivers) are recorded in
  [bench/BENCHMARKS-windows.md](../../bench/BENCHMARKS-windows.md) and
  [UPSTREAM.md](../UPSTREAM.md).
