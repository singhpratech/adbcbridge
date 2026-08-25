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

## Windows: non-ASCII text arrives mangled, or pyarrow raises `UnicodeDecodeError`

Symptoms seen on Windows 11 with builds before `d364312` (2026-08-24):

- a literal in statement text stored double-encoded — `INSERT ... VALUES ('héllo')` put
  `hÃ©llo` (`68c383c2a96c6c6f`) in the table, `WHERE s = 'héllo'` matched nothing, a
  `CREATE TABLE "tabelle_ä"` created `tabelle_Ã¤`, and `日本語` was best-fit mapped to other
  characters and lost;
- a column named `prix_€` came back as the byte `0x80`, and `fetch_arrow_table()` raised
  `UnicodeDecodeError: 'utf-8' codec can't decode byte 0x80`;
- with psqlodbc against a UTF8 database, `SELECT 'héllo'::varchar` raised
  `UnicodeDecodeError: byte 0xe9` and `SELECT '日本語'::text` returned `???`.

Cause: the Windows driver manager transcodes every *narrow* (`SQLCHAR*`, `SQL_C_CHAR`)
string between the driver and the process's ANSI code page (1252 on a Western install).
unixODBC and iODBC pass narrow bytes through untouched, so on Linux and macOS the narrow
entry points and the `SQL_C_CHAR` fetch path carry UTF-8 end to end, and adbcbridge was
written on that assumption. Bound parameters and `SQL_WCHAR` columns were always correct
(they are UTF-16), which is what hid both halves for as long as it did.

Fix, in the driver: on `_WIN32` statement text, catalog names, column and type names and
diagnostics go through the `W` entry points (`src/odbc_text.c`, `44c4926`), and every
character column — whatever the driver calls it — is read as `SQL_C_WCHAR` and converted,
with the `wchar_as_utf8` quirk (whose premise is the narrow-path-is-UTF-8 assumption)
forced off there (`9c07f78`). Verified byte-exact against PostgreSQL 16 on Windows 11 for
`héllo` and `日本語` through both `varchar` and `text`.

How to check a build: `python tests\test_windows_text.py` (needs the SQLite ODBC driver)
must print `all passed`; the compat workload's statement-literal step
(`WHERE s LIKE 'héllo%'`) fails on any entry whose text path is wrong.

A driver that reports `column_size` in *bytes* rather than characters would over-allocate
the wide buffer harmlessly; one that under-reports would truncate — none of SQLite,
DuckDB, msodbcsql or psqlodbc does either. If a text column on Windows comes back
truncated, that is where to look.


## macOS: a vendor driver built for iODBC (empty diagnostics, or "Lost connection during query")

### What you see

Through a bridge built against unixODBC, a driver such as MySQL Connector/ODBC for
macOS fails every call with an *empty* diagnostic — `[H000] [ (0) (SQLDriverConnect)` —
and pyodbc fails the same way. Through a bridge built against iODBC (before `60b05e8`)
it connects, then the first statement with a non-ASCII character fails server-side:
`[08S01] (2013) Lost connection to MySQL server during query`, the server logging
something like `Utf8Error { valid_up_to: 63 }`.

### What is actually happening

macOS has two ODBC driver managers with **different `SQLWCHAR` widths**: unixODBC's is
two bytes (UTF-16), iODBC's is `wchar_t`, four bytes, one code point per unit. A driver
compiled against one cannot be loaded through the other, whatever `install_name_tool`
says: the wide entry points exchange text in the wrong unit size, and the diagnostic
strings come back unreadable — hence the empty error. MySQL Connector/ODBC 26.7.1 for
macOS arm64 links `@rpath/libiodbcinst.dylib`; it is an iODBC driver. (So are some
others — OpenSearch's macOS pkg links `/usr/lib/libiodbc`.) Relinking such a driver to
unixODBC is **not** a valid recipe.

The second symptom was the bridge's own bug: its UTF-8 ↔ `SQLWCHAR` codecs assumed
UTF-16 and wrote surrogate pairs into four-byte slots. Since `60b05e8` every codec
honours `sizeof(SQLWCHAR)` — the tests are built a second time with a four-byte
`SQLWCHAR` (`test_utf16_wchar32`, `test_multirow_wchar32`) to keep it that way.

### What works

Build the bridge against iODBC and use the vendor driver through it:

```sh
# iODBC 3.52.16 from the openlink/iODBC tag: ./configure --disable-gui && make install
cmake -S . -B build-iodbc -DCMAKE_BUILD_TYPE=Release \
  -DODBC_INCLUDE_DIR=<iodbc>/include -DODBC_LIBRARY=<iodbc>/lib/libiodbc.dylib
cmake --build build-iodbc && ctest --test-dir build-iodbc
# The connector as downloaded needs its quarantine flag cleared, rpaths for
# libiodbcinst.dylib and its bundled libssl, and a fresh ad-hoc signature:
xattr -c <connector>/lib/*.so <connector>/lib/*.dylib <connector>/lib/plugin/*.so
install_name_tool -add_rpath <iodbc>/lib -add_rpath <connector>/lib <connector>/lib/libmyodbc26w.so
codesign -f -s - <connector>/lib/libmyodbc26w.so
```

The connector itself: Oracle's download page is JavaScript-only and its `/get/` URL refuses
`curl`; the "No thanks, just start my download" link fetches
`mysql-connector-odbc-26.7.1-macos15-arm64.tar.gz`. Its bound wide parameters are not read the
way its columns are written, so on a four-byte build the bridge binds text for this connector
through the narrow UTF-8 path (`5a16131`) — at no measurable cost: 2.0M rows/s fetch on
Databend either way.

Then `Driver=<connector>/lib/libmyodbc26w.so;...` in the connection string, with
`ADBC_ODBC_DRIVER` pointing at `build-iodbc/libadbc_driver_odbc.dylib`. One bridge build
per driver manager: a unixODBC-built bridge for unixODBC drivers (psqlodbc, sqliteodbc,
the MariaDB connector, ...), an iODBC-built one for iODBC drivers.

## macOS: the process dies with SIGABRT on the first SQL error, no message

### What you see

Connect works, `SELECT` works, and the first statement that fails on the server — a `DROP` of a
missing table, a typo — kills the process: SIGABRT, nothing on stderr, no crash report. unixODBC's
own `isql` dies the same way. Seen with Virtuoso (Homebrew 7.2.17) and the Arrow Flight SQL ODBC
driver 0.9.7 for Apple Silicon (which also fronts InfluxDB 3 and Dremio).

### What is actually happening

Those drivers are built to iODBC's `SQLWCHAR` (`wchar_t`, four bytes). unixODBC's driver manager
(`SQLWCHAR` two bytes) reads their wide diagnostic on the first `SQL_ERROR` into a fixed 12-byte
stack array — `SQLWCHAR sqlstate[6]` in `DriverManager/__info.c`, `extract_diag_error_w` — the
driver writes 24, and the stack protector aborts. It is the driver manager, not the driver and
not the bridge; the same call through iODBC returns the proper `42S02`. Reported upstream with a
driver-independent reproduction (a fake driver compiled with `SQL_WCHART_CONVERT` does it on Linux
too, on 2.3.12 and 2.3.14): [lurcher/unixODBC#239](https://github.com/lurcher/unixODBC/issues/239); the drivers' undocumented width:
[openlink/virtuoso-opensource#1469](https://github.com/openlink/virtuoso-opensource/issues/1469), [dremio/warpdrive#16](https://github.com/dremio/warpdrive/issues/16).

### What works

Build the bridge against iODBC and use those drivers through it (the recipe two sections up):
Flight SQL, InfluxDB 3 and Dremio then pass the compat workload. Or keep unixODBC and never let a
statement fail — which is not a workaround anyone should ship.
