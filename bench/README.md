<!-- SPDX-License-Identifier: Apache-2.0 -->
# Benchmarks, by operating system

Every number in this directory names the host it was taken on, and the host's
state while it was taken. The files are split by OS because the ODBC driver
manager, the drivers and the memory model differ enough that a Linux figure
says nothing about Windows.

| OS | File | State |
|---|---|---|
| Linux (Linux Mint 22.3, Ubuntu 24.04 base; unixODBC 2.3.12) | [`BENCHMARKS.md`](BENCHMARKS.md) — PostgreSQL vs native, the write path, partitioned reads; [`MATRIX_BENCHMARKS.md`](MATRIX_BENCHMARKS.md) — 46 databases; [`LANGUAGE_BENCHMARKS.md`](LANGUAGE_BENCHMARKS.md) — five languages × 46; [`RUST_BENCHMARKS.md`](RUST_BENCHMARKS.md) | **measured** |
| Windows 11 (x64, the OS's own driver manager) | [`BENCHMARKS-windows.md`](BENCHMARKS-windows.md) — all 46 entries of the matrix under Docker Desktop on WSL2 on a 14-core / 32 GB machine at `a4d6ce5` (Python compat + `matrix_bench.py` per entry), preceded by the first-build campaign on a 4-core / 8 GB laptop; [`LANGUAGE_BENCHMARKS-windows.md`](LANGUAGE_BENCHMARKS-windows.md) — five languages × 46 databases on the 14-core machine (219 of 230 language rows), preceded by the first machine's 142 cells | **measured, two machines, single samples: 46 of 46 results, 45 pass, 1 fail on vendor-driver Unicode handling; the seven entries of the 2026-09-03 batch measured on 2026-09-04 at `8be392d`: 3 pass, 2 fail inside the vendor driver, 2 without a Windows driver — 48 of 53 pass in all** (first build ever on 2026-08-24, ten defects found and fixed the same day; the full re-measure on 2026-08-25 with MySQL Connector/ODBC 26.7.1 retired the "astral `???`" class; no prefetch and no ingest fan-out in the Windows build, so not comparable with Linux) |
| macOS (Apple M4 Max, arm64, unixODBC 2.3.12 from source; iODBC from source for the vendor drivers that only ship iODBC builds) | [`BENCHMARKS-macos.md`](BENCHMARKS-macos.md) — the 46-entry matrix in batches, servers in Docker Desktop, some amd64 images under emulation; [`LANGUAGE_BENCHMARKS-macos.md`](LANGUAGE_BENCHMARKS-macos.md) — five languages on every macOS-passing database (191 cells) | **measured, one machine: 46 of 46 results, 41 pass, 5 without an obtainable driver or runnable server; of the seven 2026-09-03 entries, SingleStore, Exasol and SAP HANA pass over the LAN, three have no arm64 macOS driver, Db2 for i over the plain host-server ports — 45 of 53 pass in all** (PostgreSQL vs native there: 0.60x fetch, 0.50x ingest -- indicative, not the reference) |

Every file carries the exact commands that produced its tables, so a re-run on
any OS is a matter of pasting output. A cell that says "not measured" is telling
the truth; do not promote a CI build to a measurement.

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
