<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks, by operating system

Every number in this directory names the host it was taken on, and the host's
state while it was taken. The files are split by OS because the ODBC driver
manager, the drivers and the memory model differ enough that a Linux figure
says nothing about Windows.

| OS | File | State |
|---|---|---|
| Linux (Ubuntu 24.04, unixODBC 2.3.12) | [`BENCHMARKS.md`](BENCHMARKS.md) — PostgreSQL vs native, the write path, partitioned reads; [`MATRIX_BENCHMARKS.md`](MATRIX_BENCHMARKS.md) — 46 databases; [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) — five languages × 46; [`RUST_BENCHMARKS.md`](RUST_BENCHMARKS.md) | **measured** |
| Windows (x64, the OS's own driver manager) | [`BENCHMARKS-windows.md`](BENCHMARKS-windows.md) | **builds in CI; not yet run by a person** |
| macOS (Apple Silicon, unixODBC from Homebrew) | [`BENCHMARKS-macos.md`](BENCHMARKS-macos.md) | **builds in CI; not yet run by a person** |

The two unmeasured files carry the exact commands to produce their tables, so
that the first run on each OS is a matter of pasting output. A file that says
"not yet run" is telling the truth; do not promote a CI build to a measurement.

## What every file records

- host: OS and version, CPU, RAM, driver manager and version, the ODBC driver
  and version per database, Python/Rust/Go/Java/.NET versions where used;
- state: `uptime`-style load before and after, how many other servers were
  running, whether the CPU governor was `performance` or `powersave`;
- method: rows, repetitions, what is inside the timer, how the result was
  verified (row count, checksum);
- the numbers, with the spread (min–max), not only the mean.

## Reproducing on any OS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
export ADBC_ODBC_DRIVER=$PWD/build/libadbc_driver_odbc.so     # .dll on Windows, .dylib on macOS
export SQLITE_ODBC_DRIVER=...  POSTGRES_ODBC_DRIVER=...        # per OS, see the OS file
python bench/matrix_bench.py --rows 10000 --fetch-rows 100000 sqlite postgres
python bench/native_threshold.py --database postgres --rows 1000000 --runs 3 --partitions 8
bench/rust/run.sh sqlite postgres; bench/go/run.sh ...; bench/java/run.sh ...; bench/csharp/run.sh ...
```
