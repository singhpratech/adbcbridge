# adbcbridge for Go

`github.com/singhpratech/adbcbridge/go` loads adbcbridge — the ADBC-over-ODBC
driver built as `libadbc_driver_odbc.so` / `.dylib` / `.dll` — through the ADBC
Go driver manager (`github.com/apache/arrow-adbc/go/adbc/drivermgr`). It finds
the shared library, hands its path to the driver manager and fills in the ODBC
connection string; everything past that is the ordinary ADBC Go API.

```sh
go get github.com/singhpratech/adbcbridge/go
```

The import path ends in `go`, so give it its package name on import:

```go
import adbcbridge "github.com/singhpratech/adbcbridge/go"
```

## Use

```go
package main

import (
	"context"
	"fmt"

	adbcbridge "github.com/singhpratech/adbcbridge/go"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

func main() {
	ctx := context.Background()

	// An ODBC connection string, exactly as unixODBC / the Windows driver
	// manager would take it: "Driver=...;Database=...;" or "DSN=...;".
	db, err := adbcbridge.Open(ctx, memory.DefaultAllocator,
		"Driver=SQLite3;Database=my.db;",
		map[string]string{"adbc.odbc.prefetch": "1"}) // extra driver options, optional
	if err != nil {
		panic(err)
	}
	defer db.Close()

	cnxn, err := db.Open(ctx)
	if err != nil {
		panic(err)
	}
	defer cnxn.Close()

	stmt, err := cnxn.NewStatement()
	if err != nil {
		panic(err)
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery("SELECT 1 AS one"); err != nil {
		panic(err)
	}
	rdr, _, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		panic(err)
	}
	defer rdr.Release()
	for rdr.Next() {
		fmt.Println(rdr.RecordBatch())
	}
	if err := rdr.Err(); err != nil {
		panic(err)
	}
}
```

## API

| function | what it does |
|---|---|
| `DriverPath() (string, error)` | Absolute path of the adbcbridge shared library, found as described below. The error is a `*DriverNotFoundError` whose `Searched` field lists every place that was tried. |
| `NewDriver(alloc memory.Allocator) (adbc.Driver, error)` | An `adbc.Driver` (concretely `*adbcbridge.Driver`, wrapping `drivermgr.Driver`) whose `NewDatabase(opts)` adds the resolved path under the `"driver"` option. Pass `"uri"` and any `adbc.odbc.*` options yourself. `alloc` is the allocator ADBC Go drivers conventionally take; `drivermgr` v1.8.0 imports Arrow data through the C Data Interface and does not use one, so it is kept for API parity (`nil` means `memory.DefaultAllocator`). |
| `Open(ctx, alloc, connectionString, options) (adbc.Database, error)` | `NewDriver` + `NewDatabase` with `connectionString` stored under `"uri"` and `options` (may be `nil`) passed through. The database is initialised, not connected: call `Open(ctx)` on it for an `adbc.Connection`. |

`adbcbridge.Driver` also has `Path()`, `Allocator()` and
`NewDatabaseWithContext(ctx, opts)`. The constants `OptionKeyDriver` (`"driver"`),
`OptionKeyURI` (`"uri"`), `EnvDriver`, `EnvLibrary`, `EnvManifestPath`,
`ManifestName` and `Embedded` name the strings the package relies on.

The driver's own options (`adbc.odbc.delegate`, `adbc.odbc.prefetch`,
`adbc.odbc.parallel`, ...) and its behaviour are documented in the repository
README; they go in the options map unchanged.

## cgo and a C compiler

The ADBC Go driver manager is a cgo package, so building anything that imports
this module needs a C compiler on PATH: `gcc` or `clang` on Linux and macOS (the
Xcode command-line tools are enough).

On Windows ADBC's `drivermgr` is cgo-only and MSVC cannot serve cgo: without a
GCC (mingw-w64, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`) Go
silently sets `CGO_ENABLED=0`, the package compiles to a stub, and the build
fails with `undefined: drivermgr.Driver` — a missing toolchain, not a version
mismatch. Set `CGO_ENABLED=1` once GCC is on PATH; `adbc.h`'s dllexport warnings
under GCC are harmless.

## Where the library comes from

A Go module cannot usefully ship a prebuilt shared library: `go get` fetches
source, and a `.so` in the module cache is neither platform-selected nor
executable-installed. So the module contains no binary, and `DriverPath()`
looks for one that was put on the machine some other way — by `install.sh` or
`cmake --install` in a checkout of the driver, by unpacking a release binary,
or by an ADBC driver manifest named `odbc` — in this order:

1. the `ADBC_ODBC_DRIVER` environment variable, then `ADBCBRIDGE_LIBRARY` (an
   explicit value that does not exist is an error, not a silent fallback);
2. the copy embedded in the binary, if it was built with `-tags adbcbridge_embed`
   (see below);
3. the ADBC driver manifest `odbc.toml` in the directories the ADBC driver
   manager searches: `$ADBC_DRIVER_PATH`, the user directory
   (`~/.config/adbc/drivers` — `$XDG_CONFIG_HOME` if set —,
   `~/Library/Application Support/ADBC/Drivers`, `%LOCALAPPDATA%\ADBC\Drivers`),
   `/etc/adbc/drivers`, `/usr/local/etc/adbc/drivers`, `/usr/share/adbc/drivers`,
   `/usr/local/share/adbc/drivers`, and `etc/adbc/drivers` under `$VIRTUAL_ENV`
   or `$CONDA_PREFIX`. `install.sh` writes this manifest into the user
   directory, so after it `DriverPath()` just works;
4. common install locations: `/usr/local/lib`, `/usr/lib`, `/opt/adbcbridge/lib`,
   `/opt/homebrew/lib` (and their `lib64`), `/usr/lib/<arch>-linux-gnu`,
   `%ProgramFiles%\adbcbridge\{bin,lib}`, the `lib` directories of `$VIRTUAL_ENV`
   / `$CONDA_PREFIX`, and a CMake `build/` tree next to a source checkout of this
   package (so a `replace` directive or `go.work` pointing at `./go` works
   straight after `cmake --build build`).

If nothing matches, `DriverPath()` — and so `NewDriver` and `Open` — return a
`*DriverNotFoundError` that lists each place checked and why it did not match.

## Embedding the library (`-tags adbcbridge_embed`)

For a self-contained binary, copy the driver library into

```
go/internal/native/<goos>_<goarch>/libadbc_driver_odbc.so     # linux
go/internal/native/<goos>_<goarch>/libadbc_driver_odbc.dylib  # darwin
go/internal/native/<goos>_<goarch>/adbc_driver_odbc.dll       # windows
```

— for example `go/internal/native/linux_amd64/libadbc_driver_odbc.so` — and
build with

```sh
go build -tags adbcbridge_embed ./...
```

The tagged build `//go:embed`s that tree; on first use `DriverPath()` writes
the copy for the running platform to the user cache directory
(`$XDG_CACHE_HOME`/`~/.cache`, `~/Library/Caches`, `%LOCALAPPDATA%` — or the
temporary directory if there is none) under `adbcbridge/native/<content hash>/`
and returns that path. The extraction is atomic, keyed by the library's
content, so upgrading the binary never loads a stale copy and concurrent
processes converge on the same file. Step 1 of the lookup order still wins, so
`ADBC_ODBC_DRIVER` can override the embedded copy for debugging.

The directory is empty in the repository (only a `.gitkeep`) and the library
files are git-ignored: without the tag the tree is not read at all, and with the
tag but an empty tree the build still succeeds and `DriverPath()` simply moves
on to the manifest and install directories. The constant `adbcbridge.Embedded`
tells which variant was compiled in. Embedding only helps when the module is
built from a checkout (a `replace` directive, a `go.work`, or a vendored copy)
— `go get` fetches the published source, which contains no library.

The extracted library still needs an ODBC driver manager (unixODBC / iODBC /
the Windows one) and the ODBC driver for your database, exactly as any other
way of loading it does.

## Tests

```sh
export ADBC_ODBC_DRIVER=/path/to/libadbc_driver_odbc.so   # or install.sh first
export SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so
cd go && go test ./...
```

The tests resolve the library, open SQLite through `SQLITE_ODBC_DRIVER` with
`Driver=<library>;Database=<temp file>;` (the spelling
`tests/compat/test_matrix.py` uses), run `SELECT 1` and read the record batch.
They skip, with a message, when `SQLITE_ODBC_DRIVER` is unset or no driver
library can be found. `go test -tags adbcbridge_embed ./...` additionally
checks the embedded variant, exercising the extraction when a library is present
under `internal/native/`.

## License

Apache-2.0, like the rest of adbcbridge.
