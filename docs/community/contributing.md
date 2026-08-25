<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing

adbcBridge is a plain-C11 [ADBC](https://arrow.apache.org/adbc/) driver that
bridges any ODBC data source to Apache Arrow. Contributions of every size are
welcome: bug reports, driver-specific quirk fixes, type-mapping improvements,
docs, and tests against ODBC drivers not yet covered. This page is the working
guide; the short version and the bug-report checklist live in the repository's
[`CONTRIBUTING.md`](../../CONTRIBUTING.md).

Abbreviations: **ODBC** — Open Database Connectivity; **ADBC** — Arrow Database
Connectivity; **DDL** — Data Definition Language; **DM** — driver manager
(unixODBC, iODBC, or the Windows one); **quirk** — a per-driver or per-server
workaround the driver applies automatically.

---

## Getting set up

You need a C11 compiler, CMake ≥ 3.16, and an ODBC driver manager:

```sh
sudo apt install unixodbc-dev cmake      # Debian/Ubuntu
brew install unixodbc cmake              # macOS
# Windows: the ODBC driver manager ships with the OS
```

Build a debug tree:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
# -> build/libadbc_driver_odbc.so   (.dylib on macOS, .dll on Windows)
```

The build must stay **warning-free** under `-Wall -Wextra`.

---

## How the code is organised

Everything driver-internal is `static` or declared in `src/odbc_internal.h`; only
`AdbcDriverInit` (and the ADBC 1.0.0 entry point) is exported. The `src/`
directory:

| File | What it does |
|---|---|
| `odbc_driver.c` | The top-level ADBC driver: the database/connection lifecycle, connection setup, option setters and getters, and the driver-quirk detection in `OdbcDetectQuirks`. `struct OdbcDatabase` holds the environment handle, connection string / DSN / credentials, reader options and the native-delegation proxy. |
| `odbc_reader.c` | The result-set reader: turns ODBC rowset fetches into Arrow record batches (`ArrowArrayStream`). Statement-handle refcounting, the block-cursor fetch/repair machinery, and the read-side 32-bit-`SQLLEN` handling. |
| `odbc_bind.c` | Parameter binding (Arrow → `SQLBindParameter`) and bulk ingest. `struct ParamSlot` is the per-parameter scratch (C/SQL type, column size, indicator, UTF-16 conversion buffer, dictionary view). |
| `odbc_delegate.c` / `odbc_delegate.h` | [Native delegation](../../README.md#native-delegation): when a native ADBC driver exists for the target, let it serve the connection and use ODBC only as the fallback. The header is the contract — the decision happens in `AdbcDatabaseInit` — and defines the `adbc.odbc.delegate*` option names and the `OdbcDelegateMode` enum. |
| `odbc_objects.c` | `ConnectionGetObjects`: ODBC catalog functions → the ADBC nested catalog/schema/table/column object schema, reading columns defensively for minimal drivers (MDB Tools, sqliteodbc) that return empty or NULL for absent catalogs. |
| `odbc_partition.c` | [Partitioned reads](../../README.md#partitioned-reads-executepartitions) (`AdbcStatementExecutePartitions` / `AdbcConnectionReadPartition`): each descriptor is the original `SELECT` narrowed to a disjoint, half-open slice of the table. Carries the exactness argument and the per-backend split strategies. |
| `odbc_text.c` | Every string-carrying ODBC call, in UTF-8 end to end. On Linux/macOS these are thin passthroughs to the narrow entry points; on Windows they route through the wide (`W`) entry points, because the DM would otherwise transcode narrow text through the ANSI code page. |
| `odbc_internal.h` | The central internal header: platform and SQL-type shims, the `ADBC_ODBC_EXPORT` macro, `struct OdbcReaderOptions` (home of nearly all the quirk flags, documented field by field), and the `SQLLEN` read/indicator helpers. |
| `utils.c` / `utils.h`, `options.h` | Vendored from Apache Arrow ADBC — see [Vendored code](#vendored-code). |

---

## Coding conventions

Visible in the code and enforced by the tooling:

- **Plain C11** — no compiler extensions, no C++.
- **Formatting** is Google style at a 100-column limit with pointers bound to the
  type (`char* p`), via the checked-in `.clang-format`. Short guard clauses and
  one-line bodies are kept on one line (`AllowShortIfStatementsOnASingleLine`,
  `AllowShortBlocksOnASingleLine`). Install the hooks and let them do the work:

  ```sh
  pip install pre-commit && pre-commit install
  pre-commit run --all-files
  ```

  The existing sources predate the config, so a first touch of a file may
  reformat nearby lines — keep that reformat noise in a separate commit from the
  behaviour change.
- **Errors** go through the ODBC diagnostic helpers so SQLSTATE and the native
  error code reach the caller; do not invent error strings for ODBC failures.
- **Every allocation needs a matching release path**, including on error returns.
  Running the test suite under valgrind before a pull request is a good habit.
- **Comments explain the measured reason.** The distinctive style throughout the
  driver is that a workaround names the driver or server it is for, states what
  was observed, and where useful gives the numbers — not "work around a bug" but
  which call returned what. The quirk pattern below is the clearest example.

### The driver-quirk detection pattern

Quirks are detected in one place — `OdbcDetectQuirks(struct OdbcConnection*)` in
`src/odbc_driver.c`. It reads `SQL_DRIVER_NAME` with `SQLGetInfo` (falling back to
`SQL_DBMS_NAME` for MDB Tools, which does not implement it), lowercases it, then
runs a chain of `strstr(name, "…")` matches, each setting one or more flags on
`conn->reader_opts`. The flags themselves are declared and documented field by
field on `struct OdbcReaderOptions` in `src/odbc_internal.h`.

The pattern for a single quirk is: **key on the driver name, set a
`reader_opts` flag, and comment the measured reason.** For example, DuckDB's
parameter arrays are turned off with a comment that says exactly what breaks:

> DuckDB accepts `SQL_ATTR_PARAMSET_SIZE` but ignores the indicator array that
> goes with a column-wise parameter array: NULL parameter sets land as zeros and
> the values of the sets around them are dropped. Row-at-a-time only.

Two variations matter:

- **When the driver name is not enough, ask the server.** psqlodbc drives every
  PostgreSQL-wire server (PostgreSQL, CockroachDB, YugabyteDB, TimescaleDB,
  QuestDB, …), so a quirk keyed on its name would fire on real PostgreSQL too.
  For those, `OdbcDetectQuirks` issues one small identity query (`version()`, or
  a semantic probe) and keys on the answer. This is why the PostgreSQL
  array-ingest form is enabled only when `version()` is a genuine PostgreSQL
  banner with no fork marker.
- **When a name matches several `SQL_DRIVER_NAME` spellings** (Firebird's OdbcFb
  reports different names on Linux and Windows), match all of them; when a quirk
  is Windows-only (a `narrow_sql` fix for an ANSI-only driver), guard it with
  `#if defined(_WIN32)`.

Representative flags: `no_param_arrays`, `wchar_as_utf8`, `narrow_sql`,
`narrow_params`, `ind_stride_32bit`, `sqllen_32bit`, `bigint_param_as_string`,
`no_ssps`, `pg_array_ingest`. Several are also exposed as `adbc.odbc.*` options so
a user can force them for an unlisted driver — see the option table in
[Use (Python)](../../README.md#use-python).

---

## Adding a database to the compatibility matrix

A verified database touches five places. Use an existing entry of the same wire
protocol as the template.

1. **`tests/compat/test_matrix.py`** — add an entry to the `DBS` dict, keyed by a
   short name. An entry is a `dict` of: `env` (the environment variable naming its
   ODBC driver library), `conn` (the connection-string template, where `{drv}`
   expands to the driver library and `{drvdir}` to its directory), `ddl` (the
   `CREATE TABLE` in that server's dialect), and the tolerance fields an entry
   needs — for example `decimal_type`, `ts_precision`, `astral` (whether the
   server round-trips an astral-plane character), `text_sortable`, `ident` (an
   identifier-folding function such as `str.upper`), `unicode_env`, and `extra`
   (additional setup/read steps). The connection string can be overridden at run
   time with `<NAME>_CONN`.
2. **`tests/compat/docker-compose.yml`** — add the server as a service (under the
   `extra` profile if it is heavy or slow to start), with a pinned image and a
   host-side port that does not collide with the others.
3. **Fixtures** — anything the entry reads that is not created at run time goes in
   `tests/compat/fixtures/` (as the checked-in `.mdb` does for Access).
4. **`tests/compat/README.md`** — a section giving the root-free server command,
   the driver download/build steps, the run command with its expected `PASS`
   line, and any entry notes (identifier folding, tolerance flags, server-side
   setup).
5. **`docs/COMPATIBILITY.md`** and the **README matrix** — a row recording the
   database, its driver, the `PASS`/read-only status, and every quirk handled with
   its reason. Record the result for each operating system it was measured on.

Bring the servers up and run the new entry:

```sh
docker compose -f tests/compat/docker-compose.yml up -d
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
  python tests/compat/test_matrix.py <name>
```

An entry whose driver environment variable is unset is reported `SKIP`, so the
matrix runs cleanly wherever a given driver is not installed.

---

## Adding a driver quirk

When a driver or server misbehaves on the generic path:

1. **Reproduce it as small as you can**, ideally with plain `SQLBindCol` /
   `SQLFetch` or `isql` so the report is driver-independent — that reproduction is
   also what the [upstream record](#giving-findings-back-upstream) needs.
2. **Add or reuse a flag** on `struct OdbcReaderOptions` in
   `src/odbc_internal.h`, documented with what it does and why.
3. **Detect it** in `OdbcDetectQuirks` (`src/odbc_driver.c`): match on the driver
   name, or, if the name is shared across servers, on a server-identity query.
   Set the flag on `conn->reader_opts`.
4. **Comment the measured reason** at the detection site — the driver name, the
   observed failure, and numbers where they clarify (before/after rows per second,
   the SQLSTATE returned).
5. **Consider exposing it** as an `adbc.odbc.*` override so a user can force it on
   an unlisted driver with the same defect (as `adbc.odbc.sqllen_32bit` does).
6. **Add a compat-matrix case** or a `tests/test_sqlite.py` case that fails before
   the fix, and record the quirk in the matrix row.

---

## Running the tests

The core suite talks to a real database through the SQLite ODBC driver, so no
server is needed:

```sh
sudo apt install libsqliteodbc                    # Debian/Ubuntu
python -m venv .venv && .venv/bin/pip install adbc-driver-manager pyarrow
SQLITE_ODBC_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so \
  .venv/bin/python tests/test_sqlite.py           # prints ALL OK / PHASE2 OK
```

`ADBC_ODBC_DRIVER` overrides the path to the driver under test (default
`build/libadbc_driver_odbc.so`). Other suites: `tests/test_plug_and_play.py`
(install into a temp prefix, load by the name `odbc`), `tests/test_delegate.py`
(native delegation, each case skipping when its native driver or server is
missing), the C unit tests via `ctest --test-dir build`, and the per-language
smoke tests under `tests/rust/`, `tests/csharp/`, `tests/r/` and `tests/java/`.
The `python/` package has its own pytest suite (`pytest python/tests`).

The full compatibility matrix is `tests/compat/test_matrix.py` (see
[Adding a database](#adding-a-database-to-the-compatibility-matrix)).

CI (`.github/workflows/ci.yml`) builds on ubuntu-latest, macos-latest and
windows-latest — plus a Windows **x86** (Win32) job that is built Release, because
`SQLLEN` is 32 bits wide there and an optimiser-only test failure was once
invisible to a Debug job — and on Linux runs the SQLite suite, the Python-package
suite, and a driver-manifest discovery check against both install flavours.

---

## Benchmarks

The benchmark harnesses are in `bench/`. The governing rule, from
[`bench/README.md`](../../bench/README.md): **every number names the host it was
taken on and the host's state while it was taken.** The files are split by
operating system, because the driver manager, the drivers and the memory model
differ enough that a Linux figure says nothing about Windows.

Each benchmark file records:

- **host** — OS and version, CPU, RAM, driver manager and version, the ODBC driver
  and version per database, and the language runtime versions used;
- **state** — the load before and after, how many other servers were running, and
  the CPU governor;
- **method** — rows, repetitions, what is inside the timer, and how the result was
  verified (row count, checksum);
- **the numbers**, with the spread (min–max), not only the mean.

A file that says "not yet run" is telling the truth — do not promote a CI build to
a measurement. When you add a number, add its host and state alongside it; a bare
figure with no host is not a measurement anyone can trust or reproduce.

---

## Commit messages

The style in the git log: a **lowercase area prefix** and a colon, an
**imperative or descriptive summary**, and a **body that explains the measured
reason**. The prefix names the area touched — `driver:`, `docs:`, `bench:`. For
example:

```
driver: Virtuoso's 4-byte indicator stride on Windows (read-side ind_stride_32bit)
docs: Windows reopened for the six obtainable drivers -- 26 pass; two bridge bugs found
bench: ...
```

Keep one logical change per commit, and keep a reformat-only commit separate from
a behaviour change. Make sure `cmake --build build` is warning-free and
`tests/test_sqlite.py` passes before opening a pull request.

---

## The release process

Releases are cut by tag. Pushing a `v*` tag triggers
[`.github/workflows/release.yml`](../../.github/workflows/release.yml), which:

1. **`prepare`** — creates (or reuses) a draft GitHub Release for the tag. A
   `workflow_dispatch` run with no tag is a dry run against a draft that the last
   job deletes.
2. **`linux` / `macos` / `windows`** — build the driver library on each platform
   (Linux x86_64 and aarch64 under `manylinux_2_28`, macOS arm64, Windows x64),
   run `ctest`, build a `py3-none-<platform>` Python wheel with the library
   bundled, repair it (`auditwheel` / `delocate`) with the OS's own `libodbc`
   deliberately **excluded**, and upload the wheel and the bare library
   (`<rid>.tar.gz`) to the Release.
3. **`sdist`, `crate`** — the Python source distribution and the Rust crate
   (`cargo package`, after verifying the bundled C sources are in sync).
4. **`nuget`, `maven`** — download the platform libraries from the Release and
   pack the NuGet package (`runtimes/<rid>/native/…`) and the Maven jar
   (natives inside).
5. **`finish`** — if every build job succeeded, flip the Release out of draft;
   otherwise leave it a draft with whatever built. A dry run deletes its draft.

Files move between jobs through the Release itself, not through Actions artifacts.
A separate `publish-pypi.yml` publishes a tag's wheels to PyPI through trusted
publishing. `v0.1.0` (2026-08-25) shipped four platform libraries, four wheels
plus an sdist, a crate, a NuGet package and a jar.

---

## Licensing

adbcBridge is **Apache-2.0**. By contributing you agree that your contribution is
licensed under those terms. Every new non-vendored source file starts with the
Apache-2.0 header block ending in `SPDX-License-Identifier: Apache-2.0` (use `#`
comments for CMake, Python, YAML and TOML). Copy the block from an existing file.

### Vendored code

`vendor/nanoarrow`, `include/arrow-adbc/adbc.h`, `src/utils.{c,h}` and
`src/options.h` are vendored from Apache Arrow / Apache Arrow ADBC and kept
byte-identical to upstream; they are excluded from the formatting hooks. Do not
patch them locally — send the fix upstream and re-vendor. A new vendored component
is recorded in [`NOTICE`](../../NOTICE).

---

## Giving findings back upstream

Driving 46 databases through one driver across three operating systems turns up
defects that belong to other projects. The practice is to report each one upstream
with a reproduction that needs no adbcBridge in the stack, and to keep the whole
record — filed reports and findings documented but not yet filed — in
[`docs/UPSTREAM.md`](../UPSTREAM.md). When your work uncovers such a defect: record
it there with its first error and the conditions, add a reproduction that stands on
its own, and link the report once filed. Findings already documented and awaiting a
report are marked as such, and a reproduction contributed by anyone is welcome.

---

## Reporting bugs

ODBC drivers differ wildly, so a good report names the moving parts: the ODBC
driver and version and the driver manager; the connection string with secrets
removed; the failing SQL and the schema of the columns involved; and the full ADBC
error including SQLSTATE and the native error code. If you can, add a case to
`tests/test_sqlite.py` that fails before your change. See also the
[FAQ](faq.md) and [`CONTRIBUTING.md`](../../CONTRIBUTING.md).
