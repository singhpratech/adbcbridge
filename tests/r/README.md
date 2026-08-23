<!--
Copyright 2026 the adbcbridge authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

SPDX-License-Identifier: Apache-2.0
-->

# R smoke test

Proves that `libadbc_driver_odbc.so` loads and works through the *R* ADBC
driver manager, not just the Python and Rust ones — the driver is a plain C
shared library, so every ADBC binding reaches it the same way.

Two CRAN packages, no repo-specific R code to install:

| package | why |
|---|---|
| [`adbcdrivermanager`](https://cran.r-project.org/package=adbcdrivermanager) 0.17 | `adbc_driver()`, which `dlopen`s the `.so` and calls `AdbcDriverInit`, plus `read_adbc()` / `execute_adbc()` / `write_adbc()` |
| [`nanoarrow`](https://cran.r-project.org/package=nanoarrow) 0.6 | the array streams the results arrive in, and `as.data.frame()` on them |

## Running

The test runs in docker, because R plus a unixODBC driver manager is a lot to
ask of a dev machine. Build the driver on the host first — the container mounts
the repo read-only and loads `build/libadbc_driver_odbc.so` from it.

```sh
# from the repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j

docker build -t adbcbridge-r tests/r

docker run --rm \
  -v "$PWD:/repo:ro" \
  -v /path/to/odbc/drivers:/odbc:ro \
  -e ADBC_ODBC_DRIVER=/repo/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so \
  adbcbridge-r Rscript /repo/tests/r/smoke.R
```

`/path/to/odbc/drivers` is whatever directory holds `libsqlite3odbc.so` on the
host — on Debian/Ubuntu, `/usr/lib/x86_64-linux-gnu/odbc` after
`apt-get install libsqliteodbc`.

Expected output ends with:

```
[5] the "Use from R" README snippet
  ok  readme_snippet.R has exactly one snippet marker
  ok  README.md has exactly one 'Use from R' code block
  ok  ... byte-identical to readme_snippet.R
  ok  the README snippet runs end to end
  ok  ... inserting its two bound rows
  ok  ... and creating my_copy via write_adbc()

R SMOKE OK (25 checks)
```

The script exits non-zero on the first failed check, so `docker run` fails the
build without any output parsing.

### Without the Dockerfile

The image only preinstalls what the test needs, so a stock `rocker/r-ver:4.4`
works too — it just reinstalls everything each run (about two minutes against
the Posit binary repository):

```sh
docker run --rm \
  -v "$PWD:/repo:ro" \
  -v /path/to/odbc/drivers:/odbc:ro \
  -e ADBC_ODBC_DRIVER=/repo/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so \
  rocker/r-ver:4.4 bash -c '
    apt-get update -qq &&
    apt-get install -y -qq --no-install-recommends unixodbc libsqlite3-0 &&
    install2.r --error --skipinstalled adbcdrivermanager nanoarrow &&
    Rscript /repo/tests/r/smoke.R'
```

### Outside docker

Nothing in `smoke.R` needs a container. With R, unixODBC and the two CRAN
packages already present:

```sh
ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so \
SQLITE_ODBC_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so \
  Rscript tests/r/smoke.R
```

## Environment

| variable | default | meaning |
|---|---|---|
| `ADBC_ODBC_DRIVER` | `../../build/libadbc_driver_odbc.so` relative to `smoke.R` | the driver under test. Set it to test an installed copy instead of the one in `build/`. |
| `SQLITE_ODBC_DRIVER` | `SQLite3` | the SQLite ODBC driver to bridge to. Either an absolute path to `libsqlite3odbc.so` or a driver name registered in `odbcinst.ini`. Passed through verbatim as `Driver=...` in the ODBC connection string. |

No DSN, no `odbc.ini` entry and no server are needed: the test creates its own
SQLite database file under `tempdir()` and connects with a full connection
string.

## What the test covers

`smoke.R`, in five sections:

1. **A literal** — `SELECT 1 AS one` comes back as a one-column, one-row data
   frame through `read_adbc()` and `as.data.frame()`.
2. **NULLs and UTF-8 text** — SQL `NULL` becomes `NA` in both an `INTEGER` and
   a `TEXT` column, and stays distinct from the empty string, which survives as
   `""`. A string containing `é` and `🚀` round trips, checked *by its bytes*
   (`charToRaw()`) rather than by comparison, so a mojibake or `?`
   transliteration cannot pass, and `Encoding()` must report `UTF-8`.
3. **Parameterised queries** — `adbc_statement_bind()` binds one three-row data
   frame of parameters to `INSERT INTO people (id, name) VALUES (?, ?)`,
   executed once; the bind reports 3 rows affected and the `NA` name lands as
   SQL `NULL`. Then a bound `SELECT ... WHERE id = ?` returns only the matching
   row, both through the low-level statement API and through `read_adbc(bind=)`.
4. **`write_adbc()` bulk ingest** — a 2500-row data frame (more than the
   bridge's default 1024-row batch, so the ingest crosses batch boundaries) of
   integer, double and text columns is written and read back column for column,
   `NA` and UTF-8 included. Then `mode = "append"` adds a row to the table it
   just created rather than replacing it.
5. **The README snippet** — see below.

## The README snippet

`readme_snippet.R` holds the "Use from R" snippet from the top-level
`README.md` verbatim, below a marker comment. Section 5 of `smoke.R` does two
things with it:

- **Compares** it line for line against the fenced `r` block in `README.md`, so
  editing one without the other fails the test. (Verified by hand: adding a
  trailing comment to one copy makes the check fail.)
- **Runs** it — with the two placeholder paths and the database file name
  swapped for this test's real ones — by `eval()`ing it in a fresh environment
  whose parent is `globalenv()`, so it cannot lean on anything `smoke.R`
  defined. The documented snippet is therefore executed, not just eyeballed,
  and the docs cannot drift into an API that no longer works.

The comparison is skipped, with a `skip` line, if `README.md` is not mounted —
running `smoke.R` against a bare copy of `tests/r` still works.

## Notes

- The driver is loaded with `adbc_driver(path, entrypoint = "AdbcDriverInit")`.
  `libadbc_driver_odbc.so` also exports `AdbcDriverOdbcInit`; either works.
- `libodbc.so.2` is a hard `NEEDED` of `libadbc_driver_odbc.so`, so the
  container installs `unixodbc` even though nothing in R uses ODBC directly.
  `libsqlite3-0` is what the SQLite ODBC driver itself loads.
- The host is Ubuntu-family (glibc 2.39) and so is `rocker/r-ver:4.4`
  (Ubuntu 24.04), which is why the `.so` built on the host loads in the
  container. Building on a newer distro and running against an older image
  would fail on `GLIBC_...` version symbols; build inside the container in that
  case.
- adbcdrivermanager tracks object lifetimes: a result stream is a *child* of
  its statement, so it must be released (`nanoarrow_pointer_release()`) before
  `adbc_statement_release()`, which otherwise refuses with "has 1 unreleased
  child object". `read_adbc()` handles this for you; the low-level path in
  section 3 does it by hand.
