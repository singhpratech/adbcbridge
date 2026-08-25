<!-- SPDX-License-Identifier: Apache-2.0 -->
# C and C++

adbcBridge is a plain C11 shared library (`libadbc_driver_odbc.so` / `.dylib` /
`.dll`) that implements the [ADBC](https://arrow.apache.org/adbc/) (Arrow Database
Connectivity) C ABI over any [ODBC](https://en.wikipedia.org/wiki/Open_Database_Connectivity)
driver. From C or C++ you drive it through the ADBC 1.1.0 API: a `struct AdbcDriver`
vtable of function pointers, and the Arrow C data interface
(`ArrowArray` / `ArrowSchema` / `ArrowArrayStream`) for the data itself.

This page shows the whole surface on one page — loading the driver, the
database → connection → statement lifecycle, error handling, consuming results,
setting options, bulk ingest, and a full compilable example. It assumes C but not
prior ADBC or ODBC experience.

---

## Contents

- [The header](#the-header)
- [Loading the driver](#loading-the-driver)
- [Error handling](#error-handling)
- [Lifecycle: database, connection, statement](#lifecycle-database-connection-statement)
- [Consuming an ArrowArrayStream](#consuming-an-arrowarraystream)
- [Setting options](#setting-options)
- [Bulk ingest](#bulk-ingest)
- [A complete, compilable example](#a-complete-compilable-example)
- [Building against the repository](#building-against-the-repository)
- [C++ notes](#c-notes)
- [Known limitations](#known-limitations)

---

## The header

The only header you need to compile against is the ADBC C API, vendored in the
repository at:

```
include/arrow-adbc/adbc.h
```

It declares the status codes, the `AdbcError`, `AdbcDatabase`, `AdbcConnection`,
`AdbcStatement` and `AdbcDriver` structs, the option-key macros, and the
`AdbcDriverInit` entry-point signature. It is a single self-contained header with
no dependencies of its own.

For the Arrow C data interface types (`ArrowArray`, `ArrowSchema`,
`ArrowArrayStream`) you need a consumer library. The repository vendors
[nanoarrow](https://arrow.apache.org/nanoarrow/) at `vendor/nanoarrow/`
(`nanoarrow.h` and `nanoarrow.c`), which is dependency-free and what the driver's
own C tests use. Arrow C++ works equally well (see [C++ notes](#c-notes)).

**Note:** the ADBC *driver manager* header (`adbc_driver_manager.h`) and library
are a **separate** package (part of the upstream Arrow ADBC distribution) and are
not vendored here. If you want to load the driver by name through the manager you
must install that yourself — see the two loading options below.

---

## Loading the driver

adbcBridge exports one C entry point:

```c
AdbcStatusCode AdbcDriverInit(int version, void* raw_driver, struct AdbcError* error);
```

It fills a caller-provided `struct AdbcDriver` (the vtable) for the requested ADBC
version — pass `ADBC_VERSION_1_1_0`. There are two ways to reach it.

### Option A — `dlopen` the library directly (no driver manager)

This is the fully self-contained route, and exactly what the driver's own smoke
test does. You open the shared library, resolve `AdbcDriverInit`, and call it:

```c
#include <dlfcn.h>
#include <arrow-adbc/adbc.h>

typedef AdbcStatusCode (*AdbcDriverInitFunc)(int, void*, struct AdbcError*);

void* handle = dlopen("libadbc_driver_odbc.so", RTLD_NOW | RTLD_LOCAL);
if (!handle) { /* dlerror() */ }

AdbcDriverInitFunc init = (AdbcDriverInitFunc)dlsym(handle, "AdbcDriverInit");

struct AdbcDriver driver;
memset(&driver, 0, sizeof(driver));
struct AdbcError error = ADBC_ERROR_INIT;
AdbcStatusCode status = init(ADBC_VERSION_1_1_0, &driver, &error);
```

From here every ADBC call is a member of `driver`: `driver.DatabaseNew(...)`,
`driver.StatementExecuteQuery(...)`, and so on. When you are finished, release the
vtable with `driver.release(&driver, &error)` and, on POSIX, `dlclose(handle)`.

**Note:** `AdbcDriverInit` respects the requested version. Called with
`ADBC_VERSION_1_0_0` it leaves the 1.1.0-only entry points (such as
`StatementExecuteSchema` and `ErrorGetDetailCount`) as `NULL`; an unknown version
returns a non-OK status with a message.

### Option B — the ADBC driver manager, by name or path

If you have the ADBC driver manager installed, link against it and let it load the
library — by absolute path, or by the manifest name `odbc` when adbcBridge was
installed with `cmake --install` (which writes an `odbc.toml` manifest):

```c
#include <arrow-adbc/adbc.h>
#include <arrow-adbc/adbc_driver_manager.h>   /* from the driver-manager package */

struct AdbcDriver driver;
struct AdbcError error = ADBC_ERROR_INIT;
memset(&driver, 0, sizeof(driver));

AdbcStatusCode status = AdbcLoadDriver(
    "/path/to/libadbc_driver_odbc.so",   /* or "odbc" to use the manifest */
    "AdbcDriverInit",
    ADBC_VERSION_1_1_0,
    &driver,
    &error);
```

The rest of the code is identical to Option A. This page uses Option A in its
examples so they compile with nothing but the vendored header and `libdl`.

---

## Error handling

Every ADBC call takes a `struct AdbcError*` as its last argument and returns an
`AdbcStatusCode`. `ADBC_STATUS_OK` (`0`) is success; anything else means the
`AdbcError` has been populated.

Always zero-initialise the error with `ADBC_ERROR_INIT` before first use:

```c
struct AdbcError error = ADBC_ERROR_INIT;
```

On a non-OK status, the fields you read are:

| Field | Meaning |
|---|---|
| `error.message` | Human-readable diagnostic (owned by the error; freed by `release`) |
| `error.sqlstate` | Five-character `SQL:2003` SQLSTATE, or `"\0\0\0\0\0"` if none |
| `error.vendor_code` | Database-specific integer code, when provided |
| `error.release` | Callback that frees the error; call it, then re-zero the struct |

Because ADBC 1.1.0 extends the struct, the driver only touches the 1.1.0 fields
(`private_data`, `private_driver`) when `vendor_code` equals
`ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA` — which is what `ADBC_ERROR_INIT` sets. A
correct release-and-reset looks like this:

```c
static void ReleaseError(struct AdbcError* error) {
  if (error->release) error->release(error);
  memset(error, 0, sizeof(*error));
  error->vendor_code = ADBC_ERROR_VENDOR_CODE_PRIVATE_DATA;
}
```

The status codes are `ADBC_STATUS_OK`, `_UNKNOWN`, `_NOT_IMPLEMENTED`,
`_NOT_FOUND`, `_ALREADY_EXISTS`, `_INVALID_ARGUMENT`, `_INVALID_STATE`,
`_INVALID_DATA`, `_INTEGRITY`, `_INTERNAL`, `_IO`, `_CANCELLED`, `_TIMEOUT`,
`_UNAUTHENTICATED`, `_UNAUTHORIZED` (values `0`–`14`).

For richer error metadata, ADBC 1.1.0 adds `ErrorGetDetailCount` /
`ErrorGetDetail` on the vtable, and `AdbcErrorFromArrayStream()` retrieves an error
attached to a result stream.

An error does not poison a handle: a statement or connection is fully usable again
after a failed call.

---

## Lifecycle: database, connection, statement

ADBC objects nest: one **database** holds configuration and yields one or more
**connections**; each connection yields **statements**. Every object is
`New`-ed, configured, `Init`-ed, used, then `Release`-d, in that order, and
released children before their parents.

The full sequence, with the vtable calls in order:

```c
/* --- database --- */
struct AdbcDatabase database;
memset(&database, 0, sizeof(database));
driver.DatabaseNew(&database, &error);
driver.DatabaseSetOption(&database, "uri",
                         "Driver=SQLite3;Database=my.db;", &error);
driver.DatabaseInit(&database, &error);          /* connects lazily / validates */

/* --- connection --- */
struct AdbcConnection connection;
memset(&connection, 0, sizeof(connection));
driver.ConnectionNew(&connection, &error);
driver.ConnectionInit(&connection, &database, &error);   /* opens the ODBC connection */

/* --- statement --- */
struct AdbcStatement statement;
memset(&statement, 0, sizeof(statement));
driver.StatementNew(&connection, &statement, &error);
driver.StatementSetSqlQuery(&statement, "SELECT i, s FROM t", &error);

struct ArrowArrayStream stream;
memset(&stream, 0, sizeof(stream));
int64_t rows_affected = 0;
driver.StatementExecuteQuery(&statement, &stream, &rows_affected, &error);
/* ... consume `stream` (next section) ... */

/* --- teardown, children before parents --- */
driver.StatementRelease(&statement, &error);
driver.ConnectionRelease(&connection, &error);
driver.DatabaseRelease(&database, &error);
```

Key points, all exercised by the driver's C tests:

- **Configuration goes on the database.** At minimum set `uri` (the ODBC
  connection string) or `dsn`. `DatabaseInit` without either fails with
  `ADBC_STATUS_INVALID_ARGUMENT`.
- **`StatementExecuteQuery`'s `out` argument is optional.** Pass a non-NULL
  `ArrowArrayStream*` to receive the result set; pass `NULL` to run a statement for
  its side effect only (DDL/DML), reading the affected-row count from
  `rows_affected` (ODBC reports `-1` when it has no meaningful count).
- **A statement is reusable.** Set a new query and execute again; you may
  `StatementPrepare` once and `StatementExecuteQuery` repeatedly.
- **Result streams outlive their statement.** You may `StatementRelease` (or
  execute a second query on the same statement) while an earlier stream is still
  open; the stream stays independently readable until you release *it*. Release
  order between a statement and its open streams is up to you.

---

## Consuming an ArrowArrayStream

A result set arrives as a `struct ArrowArrayStream` — the Arrow C stream interface.
You call `get_schema` once, then `get_next` until it returns an array whose
`release` is `NULL` (end of stream). Each array is one record batch of up to
`adbc.odbc.batch_size` rows.

Using nanoarrow to read the values:

```c
#include "nanoarrow/nanoarrow.h"

struct ArrowSchema schema;
memset(&schema, 0, sizeof(schema));
if (stream.get_schema(&stream, &schema) != 0) {
  const char* msg = stream.get_last_error ? stream.get_last_error(&stream) : NULL;
  /* handle error: msg */
}

struct ArrowArrayView view;
struct ArrowError na_error;
ArrowArrayViewInitFromSchema(&view, &schema, &na_error);

int64_t total = 0;
while (1) {
  struct ArrowArray array;
  memset(&array, 0, sizeof(array));
  if (stream.get_next(&stream, &array) != 0) { /* error via get_last_error */ break; }
  if (array.release == NULL) break;                 /* end of stream */

  ArrowArrayViewSetArray(&view, &array, &na_error);
  for (int64_t i = 0; i < view.length; i++) {
    if (!ArrowArrayViewIsNull(view.children[0], i)) {
      int64_t v = ArrowArrayViewGetIntUnsafe(view.children[0], i);
      struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(view.children[1], i);
      /* use v and s.data / s.size_bytes */
    }
  }
  total += array.length;
  ArrowArrayRelease(&array);
}

ArrowArrayViewReset(&view);
ArrowSchemaRelease(&schema);
stream.release(&stream);      /* releasing the stream is your responsibility */
```

The result schema is a struct (`schema.format == "+s"`); its `n_children` are the
columns, each `children[c]` carrying the column `name` and Arrow `format` string.
An exhausted stream keeps reporting end-of-stream, and `get_schema` keeps handing
out an independently owned schema copy — both are guaranteed by the stream
contract.

`StatementExecuteSchema` (ADBC 1.1.0) returns just the result schema of a query
without running it, useful for preparing consumers ahead of the data.

---

## Setting options

Options are string key–value pairs, set at the level the driver reads them —
database, connection, or statement — with the corresponding `SetOption`. Integer
options can also be set with the typed `SetOptionInt` and read back with
`GetOptionInt`.

```c
/* database-scoped: connection string, decimals as strings */
driver.DatabaseSetOption(&database, "uri", conn_str, &error);
driver.DatabaseSetOption(&database, "adbc.odbc.decimal_as_string", "true", &error);

/* statement-scoped: rows per Arrow batch, as an integer */
driver.StatementSetOptionInt(&statement, "adbc.odbc.batch_size", 4096, &error);
int64_t batch_size = 0;
driver.StatementGetOptionInt(&statement, "adbc.odbc.batch_size", &batch_size, &error);
```

adbcBridge's own options are prefixed `adbc.odbc.`; it also honours the standard
`uri`, `dsn`, `username`, `password`, `adbc.connection.autocommit`, and the
`adbc.ingest.*` options. The complete `adbc.odbc.*` table, with each key's scope,
values and default, is in the [Python page's Options section](python.md#options) —
the keys and semantics are the same from C. A few worth naming here:

| Key | Scope | Default | Purpose |
|---|---|---|---|
| `adbc.odbc.batch_size` | database, connection, statement | `1024` | Rows per Arrow batch |
| `adbc.odbc.decimal_as_string` | database | `false` | Return `DECIMAL`/`NUMERIC` as strings |
| `adbc.odbc.max_bind_bytes` | database | `32768` | Widest column given a full bound buffer |
| `adbc.odbc.long_bind_bytes` | database | `2048` | Bind width for unbounded (`TEXT`/`bytea`) columns |
| `adbc.odbc.prefetch` | database, connection, statement | `0` | Background-fetch rowsets in flight (`0`–`8`) |
| `adbc.odbc.rows_per_insert` | statement | `0` (auto) | Row-groups per multi-row `INSERT` for ingest |

An unknown option returns a non-OK status with a message rather than being ignored.

---

## Bulk ingest

To load an Arrow batch or stream into a table, set the ingest options on a
statement, bind the data, and execute. adbcBridge generates the `CREATE TABLE`
(for the create modes) and the batched `INSERT`s.

```c
/* target table and write mode */
driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_TARGET_TABLE,
                          "my_table", &error);
driver.StatementSetOption(&statement, ADBC_INGEST_OPTION_MODE,
                          ADBC_INGEST_OPTION_MODE_CREATE, &error);

/* bind one batch ... */
driver.StatementBind(&statement, &array, &schema, &error);
/* ... or a whole stream */
driver.StatementBindStream(&statement, &input_stream, &error);

int64_t rows_affected = 0;
driver.StatementExecuteQuery(&statement, NULL, &rows_affected, &error);
```

The mode macros are `ADBC_INGEST_OPTION_MODE_CREATE`, `_APPEND`, `_REPLACE` and
`_CREATE_APPEND`. `StatementBind` binds a single `ArrowArray` (with its
`ArrowSchema`); `StatementBindStream` binds an `ArrowArrayStream` and streams it in
batches. Under the hood adbcBridge sends one multi-row `INSERT` per *K* rows in a
single transaction, probing *K* against the driver at run time;
`adbc.odbc.rows_per_insert` overrides the choice and `adbc.odbc.array_binding`
selects column-wise ODBC parameter arrays where the driver honours them.

`StatementBind` / `StatementBindStream` also supply the parameters for a
parameterised query (one column per `?`, one row per execution) — the same
mechanism, without the ingest options.

---

## A complete, compilable example

A single file that loads the driver, opens a SQLite database over ODBC, creates and
populates a table, runs a query, and prints the row count. It uses only the
vendored `adbc.h` and nanoarrow, and `libdl`.

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* example.c — minimal adbcBridge C program (POSIX). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arrow-adbc/adbc.h>
#include "nanoarrow/nanoarrow.h"

typedef AdbcStatusCode (*AdbcDriverInitFunc)(int, void*, struct AdbcError*);

static void die(const char* what, struct AdbcError* err) {
  fprintf(stderr, "%s: %s\n", what, err && err->message ? err->message : "(no message)");
  if (err && err->release) err->release(err);
  exit(1);
}
#define OK(call) do {                                    \
    AdbcStatusCode s_ = (call);                          \
    if (s_ != ADBC_STATUS_OK) die(#call, &error);        \
  } while (0)

int main(int argc, char** argv) {
  const char* lib = getenv("ADBC_ODBC_DRIVER");
  if (!lib) lib = "libadbc_driver_odbc.so";
  const char* uri = (argc > 1) ? argv[1]
                               : "Driver=SQLite3;Database=example.db;";

  void* handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  AdbcDriverInitFunc init = (AdbcDriverInitFunc)dlsym(handle, "AdbcDriverInit");
  if (!init) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

  struct AdbcError error = ADBC_ERROR_INIT;
  struct AdbcDriver driver;
  memset(&driver, 0, sizeof(driver));
  OK(init(ADBC_VERSION_1_1_0, &driver, &error));

  struct AdbcDatabase database;
  memset(&database, 0, sizeof(database));
  OK(driver.DatabaseNew(&database, &error));
  OK(driver.DatabaseSetOption(&database, "uri", uri, &error));
  OK(driver.DatabaseInit(&database, &error));

  struct AdbcConnection connection;
  memset(&connection, 0, sizeof(connection));
  OK(driver.ConnectionNew(&connection, &error));
  OK(driver.ConnectionInit(&connection, &database, &error));

  /* DDL/DML: pass NULL for the out-stream, read the affected-row count. */
  struct AdbcStatement stmt;
  const char* ddl[] = {
      "CREATE TABLE IF NOT EXISTS t (i INTEGER, s TEXT)",
      "INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three')",
  };
  for (size_t k = 0; k < sizeof(ddl) / sizeof(ddl[0]); k++) {
    memset(&stmt, 0, sizeof(stmt));
    OK(driver.StatementNew(&connection, &stmt, &error));
    OK(driver.StatementSetSqlQuery(&stmt, ddl[k], &error));
    int64_t affected = -1;
    OK(driver.StatementExecuteQuery(&stmt, NULL, &affected, &error));
    OK(driver.StatementRelease(&stmt, &error));
  }

  /* A SELECT: consume the ArrowArrayStream. */
  memset(&stmt, 0, sizeof(stmt));
  OK(driver.StatementNew(&connection, &stmt, &error));
  OK(driver.StatementSetSqlQuery(&stmt, "SELECT i, s FROM t ORDER BY i", &error));
  struct ArrowArrayStream stream;
  memset(&stream, 0, sizeof(stream));
  OK(driver.StatementExecuteQuery(&stmt, &stream, NULL, &error));

  struct ArrowSchema schema;
  memset(&schema, 0, sizeof(schema));
  if (stream.get_schema(&stream, &schema) != 0) die("get_schema", NULL);
  printf("columns: %lld\n", (long long)schema.n_children);

  int64_t total = 0;
  while (1) {
    struct ArrowArray array;
    memset(&array, 0, sizeof(array));
    if (stream.get_next(&stream, &array) != 0) die("get_next", NULL);
    if (array.release == NULL) break;
    total += array.length;
    ArrowArrayRelease(&array);
  }
  printf("rows: %lld\n", (long long)total);

  ArrowSchemaRelease(&schema);
  stream.release(&stream);
  OK(driver.StatementRelease(&stmt, &error));
  OK(driver.ConnectionRelease(&connection, &error));
  OK(driver.DatabaseRelease(&database, &error));
  OK(driver.release(&driver, &error));
  dlclose(handle);
  return 0;
}
```

### Compiling and linking

The program does not link against the driver — it `dlopen`s it at run time — so you
only need the header search paths and `libdl`. Point the compiler at the
repository's `include/` (for `arrow-adbc/adbc.h`) and `vendor/` (for nanoarrow),
and compile nanoarrow alongside:

```sh
cc -std=c11 -I /path/to/adbcbridge/include -I /path/to/adbcbridge/vendor \
   example.c /path/to/adbcbridge/vendor/nanoarrow/nanoarrow.c \
   -ldl -o example

# Tell it which driver library and database to use, then run:
ADBC_ODBC_DRIVER=/path/to/libadbc_driver_odbc.so ./example
```

On macOS drop `-ldl` (it is part of libc); on Linux keep it. If you load the driver
through the ADBC driver manager instead ([Option B](#loading-the-driver)), add its
include path and link `-ladbc_driver_manager` in place of the `dlopen` code.

---

## Building against the repository

The library is built with CMake. Its target and layout:

| | |
|---|---|
| CMake target | `adbc_driver_odbc` (a `SHARED` library) |
| Output name | `libadbc_driver_odbc.so` / `.dylib` / `.dll` |
| Public header | `include/arrow-adbc/adbc.h` |
| Vendored Arrow consumer | `vendor/nanoarrow/` (`nanoarrow.h`, `nanoarrow.c`) |
| C standard | C11 |
| Depends on | an ODBC driver manager (`unixODBC` / `iODBC` / Windows `odbc32`) and `Threads` |

Build it:

```sh
sudo apt install unixodbc-dev cmake        # Debian/Ubuntu
brew install unixodbc cmake                # macOS
# Windows: the ODBC driver manager ships with the OS

cmake -S . -B build && cmake --build build
# -> build/libadbc_driver_odbc.so
```

CMake finds the ODBC driver manager with `find_package(ODBC)`; to build against a
specific one (for example iODBC on macOS), pass
`-DODBC_INCLUDE_DIR=<dir> -DODBC_LIBRARY=<lib>`.

`cmake --install build --prefix <prefix>` installs the library and, unless you pass
`-DADBCBRIDGE_INSTALL_MANIFEST=OFF`, an `odbc.toml` driver manifest so applications
can load the driver by the name `odbc`.

Your own C or C++ program does not link the driver target; it loads
`libadbc_driver_odbc.*` at run time. What you take from the repository at build time
is the header (`include/`) and, if you use it, nanoarrow (`vendor/`). To vendor them
into your own CMake project, add `include` and `vendor` to your target's include
directories exactly as the driver's own tests do
(`target_include_directories(<your_target> PRIVATE include vendor)`).

---

## C++ notes

From C++ the driver is identical — the ADBC ABI is C — but you have richer options
for consuming results.

- **Arrow C++** imports the result stream directly. Given the `ArrowArrayStream`
  that `StatementExecuteQuery` filled, hand it to
  `arrow::ImportRecordBatchReader(&stream)` to get an
  `arrow::RecordBatchReader`, then read `arrow::RecordBatch`es with the full Arrow
  compute and I/O surface. (Arrow C++ is a separate dependency, not vendored here.)

  ```cpp
  #include <arrow/c/bridge.h>
  // ... after StatementExecuteQuery(&stmt, &stream, nullptr, &error):
  auto reader = arrow::ImportRecordBatchReader(&stream).ValueOrDie();
  std::shared_ptr<arrow::RecordBatch> batch;
  while (reader->ReadNext(&batch).ok() && batch) {
    // use batch
  }
  ```

  `arrow::ImportRecordBatchReader` takes ownership of the stream, so do **not**
  also call `stream.release`.

- **Schemas and arrays** similarly import with `arrow::ImportSchema` and
  `arrow::ImportArray` / `arrow::ImportRecordBatch` if you consume batches by hand
  rather than through a reader.

- **`ADBC_ERROR_INIT`** has a C++ overload in the header (using `AdbcError{...}`),
  so the same initialisation works. Wrap the release-on-scope-exit pattern in a
  small RAII guard if you prefer.

- **nanoarrow** is still available from C++ if you would rather not depend on the
  full Arrow C++ build; it is the lighter choice for a small consumer.

Everything else — the lifecycle, options, error handling, ingest — is the same as
the C sections above.

---

## Known limitations

- **The ADBC driver manager is a separate dependency.** Only `adbc.h` (and
  nanoarrow) are vendored here; loading by name/manifest (Option B) needs the
  driver-manager package installed. The `dlopen` route (Option A) needs neither.
- **The ODBC driver manager and the database's ODBC driver are not part of this
  library.** You supply `unixODBC`/`iODBC`/`odbc32` and the target database's ODBC
  driver.
- **Native delegation is not available on Windows**, and some advanced paths
  (partitioning, prefetch) engage only when the underlying ODBC driver's behaviour
  allows it — see the driver's `README.md` and `docs/TROUBLESHOOTING.md`.
- **Behaviour varies by ODBC driver.** Type mappings, transaction support and
  reported row counts are the underlying driver's. The `dlopen`-based smoke test and
  examples here target SQLite for a dependency-light setup.
- **The C examples are POSIX** (`dlopen`/`dlsym`). On Windows use `LoadLibrary` /
  `GetProcAddress`, or load through the driver manager.
