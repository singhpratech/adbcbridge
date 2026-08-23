<!-- SPDX-License-Identifier: Apache-2.0 -->
# Troubleshooting

## `Can't open lib '<path>' : file not found`, for a file that is there

unixODBC loads driver libraries through libltdl, which tries a list of candidate
names and, when none of them opens, reports `file not found` — the `dlerror()`
saying *why* the loader refused the file is discarded before the driver manager
sees it. So a driver that is present, readable and perfectly valid is reported
as missing, which sends you looking for the wrong problem.

adbcbridge opens the same path itself when a connection fails that way, and puts
the real reason in the ADBC error:

```
UNKNOWN: [ODBC] SQLDriverConnect failed
  [01000] (0) [unixODBC][Driver Manager]Can't open lib '/opt/mysql-connector-odbc/lib/libmyodbc9w.so' : file not found
  [adbcbridge] the file is there and readable -- the driver manager says "file not found"
  for any load failure.  dlopen(): /lib/x86_64-linux-gnu/libstdc++.so.6: cannot allocate
  memory in static TLS block
  [adbcbridge] that library was pinned to dynamic TLS before this driver loaded -- importing
  pyarrow does that to libstdc++ -- and glibc cannot move it to static TLS afterwards.  Load
  the ODBC driver before pyarrow, or LD_PRELOAD it.  Raising glibc.rtld.optional_static_tls
  does not help.  See docs/TROUBLESHOOTING.md
```

A file that genuinely is not there still says so (`No such file or directory`),
and one that cannot be read says `Permission denied` rather than `file not
found`. The most common real reasons are the static TLS one below, a missing
dependency of the driver (`ldd <driver.so>`), and a 32-bit driver in a 64-bit
process.

## `cannot allocate memory in static TLS block`

### What you see

From Python, after importing pyarrow — with adbcbridge, with pyodbc, or with a
bare `ctypes.CDLL`; adbcbridge is not involved in causing it:

```
OSError: /lib/x86_64-linux-gnu/libstdc++.so.6: cannot allocate memory in static TLS block
```

Through unixODBC it arrives as the misleading `Can't open lib ... : file not
found` above. It is deterministic: the driver never loads again in that process.

### What is actually happening

It is not, despite the wording, about running out of the loader's static TLS
surplus. Measured on this host (glibc 2.39, default
`glibc.rtld.optional_static_tls` = 0x200), a process that has imported pyarrow
has exactly as much surplus left as one that has not — enough for a 1 KiB
initial-exec thread-local, not enough for 4 KiB, either way.

What importing pyarrow changes is the *state of libstdc++*. libarrow reaches
libstdc++'s thread-locals through the dynamic TLS model, and the first such
access to a `dlopen`ed library with no static TLS offset makes glibc record it
as dynamic-TLS-only for the rest of the process' life. A library can never be
moved into the static TLS block afterwards, at any surplus.

MySQL Connector/ODBC reaches those same thread-locals (`_ZSt11__once_call`,
`_ZSt15__once_callable`) through the *initial-exec* model, which requires
libstdc++ to have a static TLS offset. After pyarrow that is no longer possible,
so the driver cannot be loaded at all.

`import pandas` and `import adbc_driver_manager.dbapi` both import pyarrow, so
they do the same thing. `import numpy` and `import adbc_driver_manager` do not.

### Which drivers are affected

A driver is affected if it has initial-exec TLS relocations against a symbol
that a *shared* library defines:

```
readelf -r <driver.so> | grep R_X86_64_TPOFF64
```

Of the 18 distinct ODBC driver libraries in this repository's compatibility
matrix, exactly one has any:

| Driver library | Affected | After `import pyarrow` |
|---|---|---|
| MySQL Connector/ODBC 9.4 (`libmyodbc9w.so`) | yes (3 initial-exec relocations into libstdc++) | **fails to load** |
| MariaDB Connector/ODBC (`libmaodbc.so`) | no | loads |
| psqlodbc (`psqlodbcw.so`) | no | loads |
| SQLite ODBC (`libsqlite3odbc.so`) | no | loads |
| DuckDB ODBC (`libduckdb_odbc.so`) | no | loads |
| msodbcsql 18 (`libmsodbcsql-18.6.so`) | no | loads |
| Oracle Instant Client (`libsqora.so.23.1`) | no | loads |
| IBM Db2 clidriver (`libdb2.so`) | no | loads |
| clickhouse-odbc (`libclickhouseodbcw.so`) | no | loads |
| Vertica (`libverticaodbc.so`) | no | loads |
| MonetDB (`libMonetODBC.so`) | no | loads |
| Firebird (`libOdbcFb.so`) | no | loads |
| Virtuoso (`virtodbc.so`) | no | loads |
| MDB Tools (`libmdbodbcW.so`) | no | loads |
| TDengine (`libtaos_odbc.so`) | no | loads |
| OpenSearch (`libsqlodbc.so`) | no | loads |
| Arrow Flight SQL / Dremio (`libarrow-odbc.so`) | no | loads |
| Apache Ignite (`libignite-odbc.so`) | no | loads |

The one affected library is shared by eleven matrix entries, because every
MySQL-wire database is driven through it: MySQL, TiDB, Dolt, Doris, StarRocks,
Percona, MatrixOne, GreptimeDB, Databend, OceanBase and MongoDB (BI connector).

### What works

**Load the ODBC driver before anything imports pyarrow.** Opening the driver
first is what gives libstdc++ its static TLS offset; everything afterwards —
pyarrow included — then uses that offset. The adbcbridge Python package does
this for you: `import adbcbridge` deliberately does not import pyarrow, and
`adbcbridge.connect()` opens the ODBC driver named in the connection string
before it imports `adbc_driver_manager.dbapi`. Nothing is needed from you.

Set `ADBCBRIDGE_PRELOAD=0` to switch that off.

If you use `adbc_driver_manager` directly, or another ODBC library such as
pyodbc, do it yourself as the first thing in the program — before pyarrow,
pandas, or `adbc_driver_manager.dbapi`:

```python
import adbcbridge

adbcbridge.preload_odbc_driver("MySQL ODBC 9.4 Unicode Driver")  # or a full path

import adbc_driver_manager.dbapi  # imports pyarrow; now harmless
```

`preload_odbc_driver` accepts a path, a driver name from `odbcinst.ini`, or
`uri=`/`dsn=`, and is best-effort by default: pass `strict=True` to have it
raise when it cannot resolve or cannot load the library. Any `dlopen` of the
driver does as well — `ctypes.CDLL("/path/to/libmyodbc9w.so")` before the first
`import pyarrow` is enough.

**Or preload libstdc++ from outside the process**, which puts it in the initial
static TLS block before anything can pin it to dynamic TLS:

```
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 python your_program.py
```

`LD_PRELOAD` pointing at the ODBC driver itself works equally well. This is the
option to reach for when you do not control the import order — a notebook
kernel, a worker started by a framework that imports pandas first.

### What does not work

* **`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=N`.** Tried at N = 512,
  1024, 4096, 65536, 1048576 and 4194304, in decimal and hex, alone and with
  `glibc.rtld.nns`: the driver still fails to load, identically. The tunable is
  not being ignored — with it set to 4194304 a 1 MiB initial-exec thread-local
  `dlopen`s where the default surplus refuses 4 KiB — it simply addresses a
  different problem. Surplus was never what ran out.
* **Loading libstdc++ *after* pyarrow**, e.g.
  `ctypes.CDLL("/lib/x86_64-linux-gnu/libstdc++.so.6")`. It is already loaded;
  the damage is its recorded TLS model, not its absence.
* **`ctypes.CDLL("/lib/x86_64-linux-gnu/libstdc++.so.6")` *before* pyarrow.**
  A `dlopen`ed library gets a static TLS offset only when something demands one,
  and a plain `dlopen` does not. Preload the *driver*, or use `LD_PRELOAD`.
* **Loading the driver again after the first failure.** There is no retry that
  helps, which is why adbcbridge does not attempt one.

Nothing here can be fixed from inside adbcbridge for a process that has already
imported pyarrow. The durable fix belongs upstream, in a Connector/ODBC build
that does not use initial-exec TLS.

### Reproducing it without a MySQL driver

`tests/test_driver_load_errors.py` builds the same condition out of two small
fixture libraries (`tests/c/tls_dep.c`, `tests/c/tls_user.c`) — one standing in
for libstdc++, one for a driver that needs it in static TLS — so the diagnostic
can be tested on any glibc host without depending on which drivers or Python
packages happen to be installed.
