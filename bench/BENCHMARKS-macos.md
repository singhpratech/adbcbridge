<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks — macOS

**Status: the driver builds on `macos-latest` in CI (`.github/workflows/ci.yml`);
nothing in this file has been measured yet.** The tables below are empty on purpose.
Fill them from a real machine and replace this paragraph with the host description.

## Host

| | |
|---|---|
| OS | _e.g. macOS 15 on Apple Silicon_ |
| CPU / RAM | _e.g. 8 GB_ |
| Driver manager | unixODBC from Homebrew (`brew install unixodbc`) |
| Build | `cmake -S . -B build && cmake --build build --config Release` with `-DCMAKE_PREFIX_PATH=$(brew --prefix unixodbc)` |
| Python | _3.12.x_, `adbc-driver-manager`, `pyarrow`, `pyodbc` |
| ODBC drivers | sqliteodbc (`brew install sqliteodbc`) _version_; psqlodbc (`brew install psqlodbc`) _version_ |
| Load | _idle; one server at a time on an 8 GB machine_ |

## Setup

```sh
brew install cmake unixodbc sqliteodbc psqlodbc
git clone https://github.com/singhpratech/adbcbridge && cd adbcbridge
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix unixodbc)"
cmake --build build
python3 -m pip install "adbc-driver-manager>=1.7" pyarrow pyodbc pytest
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.dylib
export SQLITE_ODBC_DRIVER=$(brew --prefix sqliteodbc)/lib/libsqlite3odbc.dylib
python3 tests/test_sqlite.py
python3 tests/compat/test_matrix.py sqlite
```

PostgreSQL: `brew install postgresql@16` or Docker; `POSTGRES_ODBC_DRIVER=$(brew --prefix psqlodbc)/lib/psqlodbcw.so`.

## Results

### Compat workload

| database | result | vendor string |
|---|---|---|
| sqlite | | |
| postgres | | |
| mssql | | |

### Python: adbcbridge vs pyodbc (`bench/matrix_bench.py --rows 10000 --fetch-rows 100000`)

| database | ADBC ingest rows/s | pyodbc ingest | ADBC fetch rows/s | pyodbc fetch |
|---|---:|---:|---:|---:|
| sqlite | | | | |
| postgres | | | | |

### PostgreSQL vs the native ADBC driver (`bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8`)

| axis | ours (mean of 3) | native | ratio | load before / after |
|---|---:|---:|---:|---|
| fetch | | | | |
| ingest | | | | |

macOS notes to watch for: Homebrew's unixODBC reads `~/.odbcinst.ini`, so a
driver can be given by path (as above) or registered there; iODBC is also
present on the system and must not be the one linked (`otool -L
build/libadbc_driver_odbc.dylib` should show `libodbc.2.dylib` from Homebrew);
Apple Silicon needs arm64 builds of every ODBC driver.
