<!-- SPDX-License-Identifier: Apache-2.0 -->
# adbcBridge documentation

adbcBridge is a single C library that turns **any ODBC driver on your machine
into an Apache Arrow ADBC driver**. Point it at a database you already reach
through ODBC — Db2, Oracle, SQL Server, SQLite, PostgreSQL, ClickHouse, Vertica,
SAP HANA, or anything else with an ODBC driver — and read the results as Arrow
record batches from Python, Rust, C#, Java, Go, R or C, with no per-database
code of your own.

Two acronyms show up on every page, so here they are once:

- **ODBC** (Open Database Connectivity) — the decades-old C API almost every
  database ships a driver for. You talk to a *driver manager* (a small system
  library), which loads the *vendor driver* for your database.
- **ADBC** (Arrow Database Connectivity) — a newer C API whose result sets are
  Apache Arrow columnar batches instead of ODBC's row-at-a-time buffers. ADBC
  clients exist for many languages.

adbcBridge is the piece in between.

## What it is

One shared library — `libadbc_driver_odbc.so` on Linux,
`libadbc_driver_odbc.dylib` on macOS, `libadbc_driver_odbc.dll` on Windows —
written in plain C11. It exposes the ADBC **ABI** (Application Binary Interface,
the exact function-pointer layout an ADBC client expects) on top, and calls the
ODBC driver manager underneath. It is exactly one file. The driver manager and
the vendor ODBC drivers are already yours; adbcBridge does not replace them or
bundle them.

```
   Python · Rust · C# · Java · Go · R · C          your application + its ADBC client
                    │  ADBC ABI (1.0.0 / 1.1.0)
                    ▼
        libadbc_driver_odbc.{so,dylib,dll}          adbcBridge — one shared library
                    │  ODBC API
                    ▼
   unixODBC · iODBC · the Windows driver manager     the driver manager (yours)
                    │
                    ▼
   psqlodbc · msodbcsql · Db2 CLI · Oracle · …        the vendor ODBC driver (yours)
                    │
                    ▼
              your database
```

A `SELECT` becomes Arrow record batches; parameters, prepared statements,
transactions, bulk ingest, and catalog metadata all map onto the ODBC calls
underneath. Where a database already has a first-class native ADBC driver
(PostgreSQL, SQLite, DuckDB, and others), adbcBridge can hand the whole
connection over to it — see [Native delegation](how-it-works/delegation.md).

## Who it is for

- You need Arrow-native access to a database that has **no** native ADBC driver,
  and you would rather not hand-roll ODBC-to-Arrow conversion.
- You already have ODBC drivers installed and configured, and want every
  language's ADBC client to use them.
- You are building a data tool and want one code path that reaches hundreds of
  databases instead of one integration per vendor.

You do not need to know ODBC to use adbcBridge — the getting-started pages walk
you through installing a driver manager and a driver from scratch — but the
[reference pages](#which-page-do-i-need) explain what is happening whenever you
want to look under the hood.

## How it works

1. Your language's ADBC client (for example `adbc_driver_manager` in Python)
   loads `libadbc_driver_odbc` and calls its ADBC entry point, `AdbcDriverInit`.
2. You pass an ODBC connection string as the ADBC `uri` option, for example
   `Driver=SQLite3;Database=my.db;`. adbcBridge hands it to the ODBC driver
   manager, which loads the vendor driver and connects.
3. adbcBridge reads each result set through ODBC's block-fetch and column-wise
   binding calls and builds Arrow record batches, which flow back to your
   application through the ADBC ABI.

The library is discoverable **by name**: an install writes a small ADBC *driver
manifest* (`odbc.toml`) into a directory the ADBC driver manager already
searches, so every binding can load it as `driver="odbc"` with nothing else set.
See [Connection strings](reference/connection-strings.md) and
[Options](reference/options.md) for the details.

## Current status

adbcBridge is early software — version **0.1.0**, published on
[GitHub Releases](https://github.com/singhpratech/adbcbridge/releases/tag/v0.1.0).

| | Verified today |
|---|---|
| Databases (one workload: types, NULLs, Unicode, parameters, bulk ingest, batched reads, metadata, errors) | **53 on Linux**, **44 on macOS**, **45 on Windows** |
| Language packages | **five** — Python, Rust, C#, Java, Go (R is smoke-tested) |
| ADBC ABI | 1.0.0 and 1.1.0 |
| Release | v0.1.0: four prebuilt libraries, four wheels + sdist, crate, NuGet package, jar |

Each verified database is listed, with the exact driver and any quirks handled,
in [COMPATIBILITY.md](COMPATIBILITY.md). Reachability is far wider than
verification: adbcBridge can talk to anything with an ODBC driver, and the ODBC
ecosystem covers a few hundred data sources. What the numbers above count is what
has actually been run end to end. See [ROADMAP.md](ROADMAP.md) for what is
planned.

> **Tip:** The three per-operating-system numbers differ because of driver and
> server availability on each machine, not because the bridge behaves
> differently. Where a database is missing on one platform it is almost always
> because no obtainable ODBC driver or runnable server exists there yet.

## Which page do I need?

| I want to… | Page |
|---|---|
| Install on Linux | [getting-started/install-linux.md](getting-started/install-linux.md) |
| Install on macOS | [getting-started/install-macos.md](getting-started/install-macos.md) |
| Install on Windows | [getting-started/install-windows.md](getting-started/install-windows.md) |
| Run my first query, in any language | [getting-started/first-query.md](getting-started/first-query.md) |
| Use it from Python | [languages/python.md](languages/python.md) |
| Use it from Rust | [languages/rust.md](languages/rust.md) |
| Use it from C# | [languages/csharp.md](languages/csharp.md) |
| Use it from Java | [languages/java.md](languages/java.md) |
| Use it from Go | [languages/go.md](languages/go.md) |
| Use it from C | [languages/c.md](languages/c.md) |
| Look up a driver option (`adbc.odbc.*`) | [reference/options.md](reference/options.md) |
| Write an ODBC connection string | [reference/connection-strings.md](reference/connection-strings.md) |
| Understand the Arrow type mapping | [reference/types.md](reference/types.md) |
| Understand a per-driver quirk | [reference/quirks.md](reference/quirks.md) |
| Understand native delegation | [how-it-works/delegation.md](how-it-works/delegation.md) |
| Build from source | [reference/building.md](reference/building.md) |
| Use the command line | [languages/python.md](languages/python.md#the-command-line-tool) |
| Use environment variables | [reference/options.md](reference/options.md#environment-variables) |
| Fix an error | [TROUBLESHOOTING.md](TROUBLESHOOTING.md) |
| See exactly which databases are verified | [COMPATIBILITY.md](COMPATIBILITY.md) |
| See what is planned | [ROADMAP.md](ROADMAP.md) |
| See the upstream bugs this project reported | [UPSTREAM.md](UPSTREAM.md) |
| Contribute | [community/contributing.md](community/contributing.md) |
| Read frequently asked questions | [community/faq.md](community/faq.md) |

## License

adbcBridge is Apache-2.0.
