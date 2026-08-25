<!-- SPDX-License-Identifier: Apache-2.0 -->
# Go

adbcBridge is a plain-C ADBC (Arrow Database Connectivity) driver that reaches
any data source with an ODBC (Open Database Connectivity) driver. This page
covers using it from Go through the `github.com/singhpratech/adbcbridge/go`
module.

The module is a thin layer over the ADBC Go driver manager
(`github.com/apache/arrow-adbc/go/adbc/drivermgr`): it **finds** the shared
library (`DriverPath`), **hands its path** to the driver manager (`NewDriver`),
and **fills in** the ODBC connection string (`Open`). Everything past that —
`adbc.Database`, `adbc.Connection`, `adbc.Statement` — is the ordinary ADBC Go
API. adbcBridge adds no query API of its own, only the ODBC connection string
and a handful of `adbc.odbc.*` options.

> **Abbreviations.** *ADBC* — Arrow Database Connectivity, a columnar database
> API returning Apache Arrow data. *ODBC* — Open Database Connectivity, the
> older row-oriented C API most databases ship a driver for. *cgo* — Go's
> foreign-function mechanism for calling C; the ADBC Go driver manager is a cgo
> package, so a C compiler is required to build anything that imports this
> module.

## The module

| | |
|---|---|
| Module path | `github.com/singhpratech/adbcbridge/go` |
| Go version | 1.24.0 |
| Direct dependencies | `github.com/apache/arrow-adbc/go/adbc v1.8.0`, `github.com/apache/arrow-go/v18 v18.4.1` |
| License | Apache-2.0 |

The import path ends in `go`, so give the package an explicit name on import:

```go
import adbcbridge "github.com/singhpratech/adbcbridge/go"
```

### cgo and a C compiler

The ADBC Go driver manager is a cgo package. Building anything that imports this
module needs a C compiler on `PATH`:

| OS | Compiler | Notes |
|---|---|---|
| Linux | `gcc` or `clang` | usually already present |
| macOS | `clang` | the Xcode command-line tools are enough |
| Windows | a GCC such as **mingw-w64** | MSVC cannot serve cgo; set `CGO_ENABLED=1` |

> **Troubleshooting: `undefined: drivermgr.Driver` on Windows.** ADBC's
> `drivermgr` is cgo-only and MSVC cannot compile cgo. Without a GCC on `PATH`,
> Go silently sets `CGO_ENABLED=0`, the package compiles to a stub, and the
> build fails with `undefined: drivermgr.Driver`. That is a missing toolchain,
> not a version mismatch. Install mingw-w64 (for example
> `winget install BrechtSanders.WinLibs.POSIX.UCRT`), then build with
> `CGO_ENABLED=1`. `adbc.h`'s dllexport warnings under GCC are harmless.

### Install

```sh
go get github.com/singhpratech/adbcbridge/go
```

The module contains **no** shared library: `go get` fetches source, and a `.so`
in the module cache is neither platform-selected nor executable-installed.
`DriverPath` finds a library that was put on the machine some other way — see
[Where the library comes from](#where-the-library-comes-from) — or you embed one
at build time (see [Embedding the library](#embedding-the-library)).

## First program

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
		map[string]string{"adbc.odbc.prefetch": "1"}) // extra options, optional
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

| Function / method | What it does |
|---|---|
| `DriverPath() (string, error)` | Absolute path of the adbcbridge shared library, found as [described below](#where-the-library-comes-from). The error is a `*DriverNotFoundError` whose `Searched` field lists every place tried. |
| `NewDriver(alloc memory.Allocator) (adbc.Driver, error)` | An `adbc.Driver` (concretely `*adbcbridge.Driver`, wrapping `drivermgr.Driver`) whose `NewDatabase(opts)` adds the resolved path under the `"driver"` option. Pass `"uri"` and any `adbc.odbc.*` options yourself. |
| `Open(ctx, alloc, connectionString, options) (adbc.Database, error)` | `NewDriver` + `NewDatabase` with `connectionString` stored under `"uri"` and `options` (may be `nil`) passed through. The database is initialised, not connected: call `Open(ctx)` on it for an `adbc.Connection`. |
| `Driver.Path() string` | the shared library `NewDatabase` loads |
| `Driver.Allocator() memory.Allocator` | the allocator `NewDriver` was given |
| `Driver.NewDatabase(opts) / NewDatabaseWithContext(ctx, opts)` | create an `adbc.Database`; the resolved path is added under `"driver"` unless you set `"driver"` yourself |

`alloc` is the allocator ADBC Go drivers conventionally take. `drivermgr` v1.8.0
imports Arrow data through the C Data Interface and uses no allocator, so it is
kept only for API parity; passing `nil` selects `memory.DefaultAllocator`.

### Exported constants

| Constant | Value | Meaning |
|---|---|---|
| `OptionKeyDriver` | `"driver"` | drivermgr option naming the shared library to load |
| `OptionKeyURI` | `"uri"` | option carrying the ODBC connection string |
| `EnvDriver` | `"ADBC_ODBC_DRIVER"` | first environment variable `DriverPath` checks |
| `EnvLibrary` | `"ADBCBRIDGE_LIBRARY"` | second environment variable `DriverPath` checks |
| `EnvManifestPath` | `"ADBC_DRIVER_PATH"` | ADBC driver manager's manifest search path (`os.PathListSeparator`-separated) |
| `ManifestName` | `"odbc.toml"` | file name of the ADBC driver manifest that names the driver `odbc` |
| `Embedded` | `true`/`false` | whether this build carries an embedded copy (`-tags adbcbridge_embed`) |

## Where the library comes from

`DriverPath` returns the first hit from, in order:

1. the `ADBC_ODBC_DRIVER` environment variable, then `ADBCBRIDGE_LIBRARY` — an
   explicit value that does not exist is an **error**, not a silent fallback;
2. the copy embedded in the binary, if built with `-tags adbcbridge_embed`
   (extracted to the user cache directory on first use);
3. the ADBC driver manifest `odbc.toml` in the directories the ADBC driver
   manager searches: `$ADBC_DRIVER_PATH`, the user directory
   (`~/.config/adbc/drivers` — `$XDG_CONFIG_HOME` if set —,
   `~/Library/Application Support/ADBC/Drivers`, `%LOCALAPPDATA%\ADBC\Drivers`),
   the system ones (`/etc/adbc/drivers`, `/usr/local/etc/adbc/drivers`,
   `/usr/share/adbc/drivers`, `/usr/local/share/adbc/drivers`), and
   `etc/adbc/drivers` / `share/adbc/drivers` under `$VIRTUAL_ENV` or
   `$CONDA_PREFIX`;
4. common install locations (`/usr/local/lib`, `/usr/lib`, `/opt/adbcbridge/lib`,
   `/opt/homebrew/lib` and their `lib64`; `/usr/lib/<arch>-linux-gnu`;
   `%ProgramFiles%\adbcbridge\{bin,lib}`; the `lib` directories of `$VIRTUAL_ENV`
   / `$CONDA_PREFIX`) and a CMake `build/` tree next to a source checkout of this
   package.

The library file goes by these names, most-preferred first:

| OS | File name |
|---|---|
| Linux | `libadbc_driver_odbc.so` |
| macOS | `libadbc_driver_odbc.dylib` |
| Windows | `adbc_driver_odbc.dll`, then `libadbc_driver_odbc.dll` |

> **Tip.** The driver's `install.sh` (run in a checkout) writes the `odbc.toml`
> manifest into the user directory, so `DriverPath` resolves without any
> environment variable after it. For a build tree, `cmake --build build` is
> found by step 4 when a `replace` directive or `go.work` points at `./go`.

If nothing matches, `DriverPath` — and so `NewDriver` and `Open` — return a
`*DriverNotFoundError` listing each place checked and why it did not match.

## Connection strings

The connection string is an ordinary ODBC one, stored under `"uri"` — a
`Driver=…` string or a `DSN=…` string:

```text
Driver=SQLite3;Database=my.db;
Driver={PostgreSQL Unicode};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;
DSN=mydsn;
```

Real connection strings the compatibility suite uses
(`tests/compat/test_matrix.py`):

| Database | Connection string |
|---|---|
| SQLite | `Driver=<libsqlite3odbc.so>;Database=<file>;` |
| PostgreSQL | `Driver=<psqlodbcw.so>;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| MySQL | `Driver=<myodbc>;Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;` |
| SQL Server | `Driver=<msodbcsql>;Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=…;TrustServerCertificate=yes;` |

With `Open`, the connection string is passed as the third argument and wins over
any `"uri"` in the options map. With `NewDriver` + `NewDatabase`, put it in the
options map under `OptionKeyURI` (`"uri"`) yourself.

## Options

Pass driver options in the map given to `Open` (or `NewDatabase`). Standard ADBC
keys carry the connection and identity; `adbc.odbc.*` keys tune the driver. They
go through unchanged.

| Key | Meaning |
|---|---|
| `uri` | full ODBC connection string (`Driver=…;Server=…;`) |
| `dsn` | DSN name from `odbc.ini` (appended as `DSN=…`) |
| `username`, `password` | appended as `UID=` / `PWD=` |
| `adbc.odbc.batch_size` | rows per Arrow batch (default 1024) |
| `adbc.odbc.max_bind_bytes` | widest value bound at its declared width, in bytes (default 32768) |
| `adbc.odbc.long_bind_bytes` | width, in bytes, for a column with no real declared bound (`TEXT`/`NVARCHAR(MAX)`/`bytea`); default 2048 |
| `adbc.odbc.rowset_bytes` | ceiling on a reader's bound rowset buffers, in bytes (default 8388608) |
| `adbc.odbc.decimal_as_string` | `true` to return DECIMAL/NUMERIC as strings |
| `adbc.odbc.partitions` | partitions `ExecutePartitions` splits a query into — `0` (default) auto, `1` never. Set on the **statement** |
| `adbc.odbc.prefetch` | rowsets kept in flight on a background fetch thread — `0` (default) off, up to `8`. Settable on database/connection/statement. *(Compiled out on Windows; see [Known limitations](#known-limitations).)* |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — native delegation |
| `adbc.odbc.delegate.driver` | force a specific native driver (bare or manifest name; a path only with `allow_paths`) |
| `adbc.odbc.delegate.search_path` | extra directories to search for native drivers (`:`-separated); needs `allow_paths` |
| `adbc.odbc.delegate.allow_paths` | `true` to let the two options above name filesystem paths (default `false`) |
| `adbc.odbc.delegate.last_error` | read-only: why delegation did not happen |
| `adbc.odbc.delegated_to` | read-only: the native driver serving this database/connection, or `odbc` |
| `adbc.odbc.tune` | `true` (default) / `false` — may the driver add ODBC connection keywords of its own where it recognises the target? |
| `adbc.odbc.sqllen_32bit` | force the 32-bit-`SQLLEN` driver quirk on/off (autodetected for IBM Db2). Also settable on connection/statement |
| `adbc.odbc.rows_per_insert` | rows of parameters per `INSERT` for **bulk ingest** — `0` (default) auto, `1` off |
| `adbc.odbc.ingest_connections` | connections a **bulk ingest** may spread over — `1` (default) single transaction; `N > 1` **trades atomicity for speed** *(compiled out on Windows)* |
| `adbc.odbc.array_binding` | `true` (default) binds each Arrow batch as a column-wise parameter array; `false` forces row-at-a-time |

### Native delegation

Where a native ADBC driver exists for the target (PostgreSQL, SQLite, DuckDB,
Snowflake, BigQuery, Flight SQL), `adbc.odbc.delegate=auto` (the default) hands
the whole database over to it for native speed from the same install. `never`
keeps adbcBridge on its own ODBC path; `always` requires delegation.

## Queries with the arrow-go RecordReader

`Statement.ExecuteQuery(ctx)` returns an `array.RecordReader` (from
`github.com/apache/arrow-go/v18/arrow/array`), the number of rows if known, and
an error. Drive it with `Next` / `RecordBatch` / `Err`, and always `Release` it:

```go
rdr, _, err := stmt.ExecuteQuery(ctx)
if err != nil {
	return err
}
defer rdr.Release()

total := int64(0)
for rdr.Next() {
	rec := rdr.RecordBatch()      // arrow.RecordBatch for this chunk
	total += rec.NumRows()
	// rec.Column(i) is an arrow.Array; rec.Schema() is the schema
}
return rdr.Err()                  // checked after the loop ends
```

`rdr.Schema()` gives the Arrow schema before you pull any rows. For a statement
run for its side effect (DML), use `ExecuteUpdate(ctx)`, which returns the
affected row count instead of a reader.

## Parameters

Bind an Arrow record of parameter values with `Statement.Bind(ctx, rec)` (or a
whole stream with `BindStream`), then execute. Each row of the record is one set
of parameters:

```go
if err := stmt.SetSqlQuery("INSERT INTO t (a, b) VALUES (?, ?)"); err != nil {
	return err
}
if err := stmt.Bind(ctx, rec); err != nil {   // rec: arrow.Record, its rows = parameter sets
	return err
}
_, err := stmt.ExecuteUpdate(ctx)             // rows affected
return err
```

## Bulk ingest

Set the target table and ingest mode as statement options, bind the Arrow
record, and run `ExecuteUpdate`. adbcBridge generates the DDL and batches the
rows into multi-row `INSERT`s inside one transaction:

```go
import "github.com/apache/arrow-adbc/go/adbc"

stmt, err := cnxn.NewStatement()
if err != nil {
	return err
}
defer stmt.Close()

if err := stmt.SetOption(adbc.OptionKeyIngestTargetTable, "my_table"); err != nil {
	return err
}
if err := stmt.SetOption(adbc.OptionKeyIngestMode, adbc.OptionValueIngestModeCreate); err != nil {
	return err
}
if err := stmt.Bind(ctx, rec); err != nil {   // the arrow.Record to ingest
	return err
}
if _, err := stmt.ExecuteUpdate(ctx); err != nil {
	return err
}
return cnxn.Commit(ctx)                        // if autocommit is off
```

To turn autocommit off, set it on the connection (which implements
`adbc.PostInitOptions`):

```go
opts := cnxn.(adbc.PostInitOptions)
if err := opts.SetOption(adbc.OptionKeyAutoCommit, adbc.OptionValueDisabled); err != nil {
	return err
}
```

Tune ingest with `adbc.odbc.rows_per_insert`, `adbc.odbc.array_binding` and
`adbc.odbc.ingest_connections` (see the options table).

## Metadata

adbcBridge implements the standard ADBC metadata calls on the `adbc.Connection`
interface — `GetInfo`, `GetObjects`, `GetTableTypes` and `GetTableSchema` —
backed by ODBC catalog functions and returning Arrow. Use them exactly as with
any ADBC Go driver; adbcBridge adds nothing of its own here.

## Errors

| Type | When |
|---|---|
| `*adbcbridge.DriverNotFoundError` | `DriverPath` / `NewDriver` / `Open` could not find the shared library. Fields: `Library` (the file name looked for on this OS) and `Searched` (every place checked, each with a note on why it did not match). The `Error()` text ends with how to fix it. |
| `adbc.Error` | query, connection and statement failures from the driver manager, carrying the structured ODBC diagnostics (SQLSTATE plus the native error code) adbcbridge maps from the driver. |

Detect a missing driver with `errors.As`:

```go
_, err := adbcbridge.DriverPath()
var notFound *adbcbridge.DriverNotFoundError
if errors.As(err, &notFound) {
	fmt.Println("looked in:", notFound.Searched)
}
```

## Embedding the library

For a self-contained binary, copy the driver library into

```
go/internal/native/<goos>_<goarch>/libadbc_driver_odbc.so     # linux
go/internal/native/<goos>_<goarch>/libadbc_driver_odbc.dylib  # darwin
go/internal/native/<goos>_<goarch>/adbc_driver_odbc.dll       # windows
```

— for example `go/internal/native/linux_amd64/libadbc_driver_odbc.so` — and
build with:

```sh
go build -tags adbcbridge_embed ./...
```

The tagged build `//go:embed`s that tree. On first use, `DriverPath` writes the
copy for the running platform to the user cache directory (`$XDG_CACHE_HOME` /
`~/.cache`, `~/Library/Caches`, `%LOCALAPPDATA%` — or the temporary directory if
there is none) under `adbcbridge/native/<content hash>/`, and returns that path.
The extraction is atomic and keyed by the library's content, so upgrading the
binary never loads a stale copy and concurrent processes converge on the same
file. Step 1 of the lookup order still wins, so `ADBC_ODBC_DRIVER` can override
the embedded copy for debugging.

The `internal/native/` tree is empty in the repository (only a `.gitkeep`) and
the library files are git-ignored. Without the tag the tree is not read at all;
with the tag but an empty tree the build still succeeds and `DriverPath` simply
moves on to the manifest and install directories. The constant
`adbcbridge.Embedded` reports whether an embedded build was compiled. Embedding
only helps when the module is built from a checkout (a `replace` directive, a
`go.work`, or a vendored copy) — `go get` fetches the published source, which
contains no library.

The extracted library still needs an ODBC driver manager (unixODBC / iODBC / the
Windows one) and the ODBC driver for your database, exactly as any other way of
loading it does.

## Known limitations

- **cgo is mandatory.** A pure-Go build (`CGO_ENABLED=0`) cannot use this
  module; the ADBC driver manager is a cgo package. On Windows this means a GCC
  toolchain (mingw-w64) and `CGO_ENABLED=1`; see [cgo and a C
  compiler](#cgo-and-a-c-compiler).
- **No `database/sql` interface.** This module exposes the ADBC Go API
  (`adbc.Database` / `adbc.Connection` / `adbc.Statement`), not a
  `database/sql` driver. If you need `database/sql` against ODBC, that is a
  separate third-party driver, unrelated to adbcbridge.
- **Windows: no prefetch, no parallel ingest.** The prefetch pipeline
  (`adbc.odbc.prefetch`) and the ingest fan-out (`adbc.odbc.ingest_connections`)
  use POSIX threads and are compiled out on Windows; those options have no
  effect there. Queries and single-connection ingest work normally.
- **A third-party `database/sql` ODBC driver was measured to crash on
  Windows.** The repository's Go benchmark compares adbcBridge against
  `github.com/alexbrainman/odbc` (a separate `database/sql` ODBC driver, *not*
  part of adbcBridge). On Windows that library was measured to fault inside the
  ODBC driver — `Exception 0xc0000005` (access violation) in `SQLGetDiagRec` —
  taking the process down, so the benchmark ran its comparison columns with that
  path disabled
  ([`bench/LANGUAGE_BENCHMARKS-windows.md`](../../bench/LANGUAGE_BENCHMARKS-windows.md)).
  This is a measured property of that separate library, not of adbcBridge, and
  it does not affect adbcBridge's own ADBC path, which ran on Windows in the
  same campaign.
- **You still need an ODBC driver for your database.** adbcBridge is the bridge,
  not the database driver: the driver manager still has to find the ODBC driver
  your `Driver=…`/`DSN=…` names.

## Complete worked example

A self-contained program that opens SQLite over ODBC, creates a table via bulk
ingest from an Arrow record, and reads the rows back — all through the ADBC Go
API. Build it with a C compiler on `PATH` (and `CGO_ENABLED=1` on Windows).

```go
package main

import (
	"context"
	"fmt"

	adbcbridge "github.com/singhpratech/adbcbridge/go"
	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

func main() {
	ctx := context.Background()

	// 1. Open the database. adbc.odbc.delegate=never keeps us on the ODBC path
	//    even if a native ADBC SQLite driver happens to be installed.
	db, err := adbcbridge.Open(ctx, memory.DefaultAllocator,
		"Driver=SQLite3;Database=example.db;",
		map[string]string{"adbc.odbc.delegate": "never"})
	if err != nil {
		panic(err)
	}
	defer db.Close()

	cnxn, err := db.Open(ctx)
	if err != nil {
		panic(err)
	}
	defer cnxn.Close()

	// 2. Turn autocommit off so the ingest and its commit are one transaction.
	if err := cnxn.(adbc.PostInitOptions).SetOption(
		adbc.OptionKeyAutoCommit, adbc.OptionValueDisabled); err != nil {
		panic(err)
	}

	// 3. Build a small Arrow record.
	schema := arrow.NewSchema([]arrow.Field{
		{Name: "id", Type: arrow.PrimitiveTypes.Int32},
		{Name: "name", Type: arrow.BinaryTypes.String},
	}, nil)
	bld := array.NewRecordBuilder(memory.DefaultAllocator, schema)
	defer bld.Release()
	bld.Field(0).(*array.Int32Builder).AppendValues([]int32{1, 2, 3}, nil)
	bld.Field(1).(*array.StringBuilder).AppendValues([]string{"ada", "grace", "linus"}, nil)
	rec := bld.NewRecord()
	defer rec.Release()

	// 4. Bulk-ingest it into a new table.
	ingest, err := cnxn.NewStatement()
	if err != nil {
		panic(err)
	}
	if err := ingest.SetOption(adbc.OptionKeyIngestTargetTable, "people"); err != nil {
		panic(err)
	}
	if err := ingest.SetOption(adbc.OptionKeyIngestMode, adbc.OptionValueIngestModeCreate); err != nil {
		panic(err)
	}
	if err := ingest.Bind(ctx, rec); err != nil {
		panic(err)
	}
	if _, err := ingest.ExecuteUpdate(ctx); err != nil {
		panic(err)
	}
	ingest.Close()
	if err := cnxn.Commit(ctx); err != nil {
		panic(err)
	}

	// 5. Read the rows back into Arrow.
	query, err := cnxn.NewStatement()
	if err != nil {
		panic(err)
	}
	defer query.Close()
	if err := query.SetSqlQuery("SELECT id, name FROM people ORDER BY id"); err != nil {
		panic(err)
	}
	rdr, _, err := query.ExecuteQuery(ctx)
	if err != nil {
		panic(err)
	}
	defer rdr.Release()

	total := int64(0)
	for rdr.Next() {
		total += rdr.RecordBatch().NumRows()
	}
	if err := rdr.Err(); err != nil {
		panic(err)
	}
	fmt.Printf("read %d rows back\n", total)
}
```

> **Tip.** If `Open` returns a `*DriverNotFoundError`, set `ADBC_ODBC_DRIVER` to
> the path of `libadbc_driver_odbc.{so,dylib}` / `adbc_driver_odbc.dll`, run the
> driver's `install.sh` in a checkout (which also writes the `odbc.toml`
> manifest), or embed a copy with `-tags adbcbridge_embed`. The error lists
> every location it checked.
