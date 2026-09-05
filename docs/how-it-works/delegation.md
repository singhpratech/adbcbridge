<!-- SPDX-License-Identifier: Apache-2.0 -->
# Native delegation

Where a native ADBC driver exists for the database behind your connection string, adbcBridge hands the whole driver over to it.

Native speed where a native ADBC driver exists, ODBC everywhere else, one
install.

Some databases already have a first-class ADBC driver: PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL. Those drivers talk the wire protocol
and build Arrow directly, so they are faster than anything that has to go
through ODBC's row-oriented API — 1,000,000 PostgreSQL rows take about 0.2 s through
`adbc_driver_postgresql` and 0.5 s through adbcBridge on one connection over psqlodbc
(`bench/BENCHMARKS.md`).

So adbcBridge gets out of the way. When `AdbcDatabaseInit` recognizes a target
that a native driver handles, it loads that driver, initializes it with the
translated options, and from then on forwards every database, connection and
statement call straight to it. Result sets are the native driver's own
`ArrowArrayStream`, handed to the caller untouched: delegation costs one
function-pointer hop per ADBC call and nothing at all per row, and delegated
fetches measure the same as calling the native driver directly (the same
0.2 s for the million rows above as the native driver, against 0.5 s over psqlodbc).

| target | delegated to |
|---|---|
| `uri=postgresql://…` / `postgres://…` | `postgresql` |
| `uri=sqlite:…`, `duckdb:…` | `sqlite`, `duckdb` |
| `uri=snowflake://…`, `bigquery://…` | `snowflake`, `bigquery` |
| `uri=grpc://…`, `grpc+tls://…` | `flightsql` |
| `uri=Driver=…psqlodbcw.so;Server=…` | `postgresql` (URI rebuilt from the ODBC keywords) |
| `uri=Driver=…sqlite3odbc.so;Database=…` | `sqlite` (the `Database=` path) |
| `dsn=…` | whatever the DSN's `Driver=` in `odbc.ini` maps to |
| anything else (Db2, Oracle, SQL Server, Teradata, …) | nobody — plain ODBC |

## The delegated connection is the connection you configured

Rebuilding a native URI out of an ODBC connection string is only safe if every
keyword is accounted for. Dropping `SSLmode=verify-full` would turn a verified
TLS session into libpq's default (`sslmode=prefer`, no certificate check)
without a word to anyone, so adbcBridge does not drop anything:

* **Consumed**: `Driver`, `DSN`, `Server`/`Servername`/`Host`, `Port`,
  `Database`/`DB`/`Dbname`, `Uid`/`User`/`Username`, `Pwd`/`Password`.
* **Forwarded** as libpq URI parameters: `sslmode`, `sslrootcert`, `sslcert`,
  `sslkey`, `sslcrl`, `sslcompression`, `sslsni`, `gssencmode`,
  `channel_binding`, `krbsrvname`/`pgkrbsrvname`, `connect_timeout`,
  `application_name`, `options`, `target_session_attrs`, `require_auth`,
  `load_balance_hosts`, and every libpq setting inside psqlodbc's
  `pqopt={…}` block.
* **Ignored**: keywords that only steer the ODBC driver's own client-side
  behaviour (`UseDeclareFetch`, `Fetch`, `Protocol`, `BoolsAsChar`,
  `TextAsLongVarchar`, `RowVersioning`, sqliteodbc's `StepAPI`/`LongNames`, …)
  and driver-manager bookkeeping (`Description`, `Trace`, `UsageCount`, …).
* **Anything else stops delegation.** `ReadOnly=1`, `ConnSettings=…`,
  sqliteodbc's `FKSupport`/`JournalMode`/`LoadExt`, DuckDB's `access_mode`: in
  `auto` the connection quietly stays on ODBC (with the keyword named in
  `adbc.odbc.delegate.last_error`), in `always` it is an error. The same goes
  for a DSN whose keywords the driver manager cannot enumerate.

Values are percent-encoded into the URI, so a `Database=` or `Server=` that
contains `?`, `&` or `=` stays a database name instead of becoming extra libpq
parameters. `Server=/var/run/postgresql` becomes `?host=/var/run/postgresql`,
`Server=::1` becomes `[::1]`, and the ODBC `}}` escape inside a brace-quoted
value is a literal `}`.

## When delegation does not happen

Delegation is a best-effort optimization: if no native driver is installed, if
it cannot be loaded, or if the target cannot be represented, `auto` falls back
to ODBC and records why in `adbc.odbc.delegate.last_error` (`always` turns the
same situation into an error). Two cases deliberately do *not* fall back
silently:

* A **native URI that the native driver rejected** reports the native driver's
  own error. ODBC cannot parse `postgresql://…` at all, so falling back would
  replace "password authentication failed" with unixODBC's `[IM002] Data source
  name not found`.
* A **native URI on the ODBC path** (`delegate=never`, or no native driver
  installed) is translated into an ODBC connection string for an installed ODBC
  driver of the same family, or, if there is none, refused with a message that
  says so.

Options meant for a native driver (`adbc.*` other than `adbc.odbc.*`) are held
until that decision is made and passed on to the native driver; if delegation
does not happen, `AdbcDatabaseInit` reports the option as unknown rather than
dropping it. Connection options work the same way: a connection does not know
who will serve it until `AdbcConnectionInit`, so an option ODBC does not
recognize (Flight SQL's `adbc.flight.sql.rpc.call_header.*`, a Snowflake
connection setting, …) is held there too — replayed on the native connection,
through the same typed setter it was set with, or reported by
`AdbcConnectionInit` if the connection ends up on ODBC. ODBC-specific options
(`adbc.odbc.batch_size`, …) are meaningless to a native driver and are rejected
by it. All `adbc.odbc.delegate*` options are frozen once `AdbcDatabaseInit` has
run: setting them afterwards is `INVALID_STATE`, not a silent no-op.

## Finding the native driver

adbcBridge never links against the ADBC driver manager (that would be a
circular dependency); it resolves `AdbcLoadDriver` from whichever manager is
already in the process and looks in, in order: driver manifests
(`<name>.toml`) under `ADBC_DRIVER_PATH`, `$XDG_CONFIG_HOME/adbc/drivers` (or
`~/.config/adbc/drivers`) and `/etc/adbc/drivers`; Python wheel layouts
(`site-packages/adbc_driver_postgresql/`) next to whatever ADBC object is
already loaded; the directories in `adbc.odbc.delegate.search_path` /
`ADBC_ODBC_DELEGATE_PATH`; and finally `libadbc_driver_<name>.so` on the
dynamic loader's own search path. Only `AdbcLoadDriver` is used: the newer
`AdbcFindLoadDriver` changed its signature between driver manager releases.

Because a database option that names a shared library is a code-execution
primitive for any host that forwards caller-supplied options (BI tools, DBAPI
`db_kwargs`, connector configuration), `adbc.odbc.delegate.driver` accepts only
a bare driver name — letters, digits, `_` and `-` — by default, and
`adbc.odbc.delegate.search_path` is refused outright. Paths are opt-in:

```python
db_kwargs = {
    "uri": uri,
    "adbc.odbc.delegate.allow_paths": "true",       # or ADBC_ODBC_DELEGATE_ALLOW_PATHS=1
    "adbc.odbc.delegate.driver": "/opt/drivers/libadbc_driver_postgresql.so",
}
```

`ADBC_ODBC_DELEGATE_PATH` and `ADBC_DRIVER_PATH` are ignored for setuid/setcap
processes, adbcBridge refuses to delegate to itself, and a bare
`libadbc_driver_<name>.so` is never picked up from a directory that was merely
derived from a loaded object's parent (only the exact wheel layout is).

```python
# Off, for this database:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "never"})
# Off, for a whole deployment:
#   export ADBC_ODBC_DELEGATE=never
# Required, so that a missing native driver is an error instead of a slow path:
dbapi.connect(driver="odbc", db_kwargs={"uri": uri, "adbc.odbc.delegate": "always"})
```

Who served a connection is visible through `adbc_get_info()["driver_name"]`
(`ADBC PostgreSQL Driver` vs `ADBC ODBC Driver`) and through
`adbc.odbc.delegated_to`, which names the native driver (`postgresql`) or
answers `odbc`, on both the database and the connection.

Delegation needs the driver manager's loader to be reachable in the process,
and it is not implemented on Windows — there `auto` always takes the ODBC path
and `always` fails with a clear message. The native-URI-to-ODBC fallback is
unavailable on Windows for the same reason: it picks the ODBC driver to fall
back to by enumerating `odbcinst.ini`, which is not implemented there either, so
a native URI on the ODBC path is refused with an explanation instead.
