# Contributing to adbcbridge

Thanks for your interest in adbcbridge — an [ADBC](https://arrow.apache.org/adbc/)
driver that bridges any ODBC data source to Apache Arrow. Website:
<https://adbcbridge.org>.

Contributions of all sizes are welcome: bug reports, driver-specific quirk
fixes, type-mapping improvements, docs, and tests against ODBC drivers we do not
have coverage for yet.

## Getting set up

You need a C11 compiler, CMake >= 3.16, and an ODBC driver manager:

```sh
sudo apt install unixodbc-dev cmake      # Debian/Ubuntu
brew install unixodbc cmake              # macOS
# Windows: the ODBC driver manager ships with the OS
```

Build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
# -> build/libadbc_driver_odbc.so
```

The build must stay **warning-free** with `-Wall -Wextra`.

## Running the tests

The test suite talks to a real database through the SQLite ODBC driver, so no
server is needed:

```sh
sudo apt install libsqliteodbc                    # Debian/Ubuntu
python -m venv .venv
.venv/bin/pip install adbc-driver-manager pyarrow
SQLITE_ODBC_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so \
  .venv/bin/python tests/test_sqlite.py
```

The script prints `ALL OK` and `PHASE2 OK` when it passes. `ADBC_ODBC_DRIVER`
overrides the path to the driver under test (default:
`build/libadbc_driver_odbc.so`).

If you are fixing a bug, add a case to `tests/test_sqlite.py` that fails before
your change.

## Code style

C sources are formatted with `clang-format` using the checked-in
[`.clang-format`](.clang-format): Google style, 100-column lines, pointers bound
to the type (`char* p`). Install the hooks and let them do the work:

```sh
pip install pre-commit
pre-commit install
```

The hooks run `clang-format` plus end-of-file and trailing-whitespace fixes on
the files you touch. To check everything at once:

```sh
pre-commit run --all-files
```

Note that the existing sources predate the config and are not yet byte-identical
to it, so a first touch of a file may reformat nearby lines. Keep such reformat
noise in a separate commit from the behavior change.

Other conventions:

- Plain C11 — no compiler extensions, no C++.
- Everything driver-internal is `static` or declared in `src/odbc_internal.h`;
  only `AdbcDriverInit` (and the ADBC 1.0.0 entrypoint) is exported.
- Errors go through the ODBC diagnostic helpers so that SQLSTATE and the native
  error code reach the caller; do not invent error strings for ODBC failures.
- Every allocation needs a matching release path, including on error returns;
  running the test suite under valgrind before a PR is a good habit.

## Vendored code

`vendor/nanoarrow`, `include/arrow-adbc/adbc.h`, `src/utils.{c,h}` and
`src/options.h` are vendored from Apache Arrow / Apache Arrow ADBC and are kept
byte-identical to upstream. They are excluded from formatting hooks. Do not
patch them locally — send the fix upstream and re-vendor.

## Licensing

adbcbridge is Apache-2.0. By contributing you agree that your contribution is
licensed under those terms.

Every new non-vendored source file starts with the license header:

```c
// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...
//
// SPDX-License-Identifier: Apache-2.0
```

Copy the block from an existing file (use `#` comments for CMake, Python, YAML
and TOML). If you add a vendored third-party component, record it in `NOTICE`.

## Commits and pull requests

- One logical change per commit; write a subject line that says what changed
  and why it matters (`odbc_reader: handle SQL_NO_TOTAL for wide columns`).
- Make sure `cmake --build build` is warning-free and `tests/test_sqlite.py`
  passes before opening a PR.
- CI (`.github/workflows/ci.yml`) builds on ubuntu-latest, macos-latest and
  windows-latest, and runs the SQLite test suite plus a driver-manifest
  discovery check on Linux.

## Reporting bugs

ODBC drivers differ wildly, so please include:

- the ODBC driver and version (and the driver manager: unixODBC, iODBC, Windows),
- the connection string with secrets removed,
- the failing SQL and the schema of the columns involved,
- the full ADBC error, including SQLSTATE and the native error code.
