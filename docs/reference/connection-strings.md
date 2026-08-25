<!-- SPDX-License-Identifier: Apache-2.0 -->
# Connection strings

adbcBridge connects to a database by handing an **ODBC connection string** to the
ODBC (Open Database Connectivity) driver manager's `SQLDriverConnect`. The string
is whatever your data source's ODBC driver expects; adbcBridge passes it through
almost verbatim. This page explains how the string is assembled and quoted, then
gives a working template for every database in the compatibility matrix.

The connection string is set as the ADBC `uri` option (or its alias
`adbc.odbc.connection_string`). See [Options](options.md) for the option
mechanics.

---

## How the string reaches the driver

adbcBridge builds the final string in `OdbcOpenHdbc` (`src/odbc_driver.c`) by
concatenating, in order:

1. the `uri` / `adbc.odbc.connection_string` value, with a trailing `;` added if
   it lacks one;
2. `DSN=<value>;` if the `dsn` option was set;
3. `UID=<value>;` if the `username` option was set;
4. `PWD=<value>;` if the `password` option was set;
5. any keywords added by connection tuning (`adbc.odbc.tune`, on by default),
   which never overrides a keyword you already set.

The result is passed to `SQLDriverConnect` with `SQL_DRIVER_NOPROMPT` (no
interactive dialog). If the narrow call fails with only a truncation diagnostic,
adbcBridge retries with the wide (`SQLDriverConnectW`) entry point — this is what
lets some drivers on iODBC connect at all.

### `Driver=` versus `DSN=`

An ODBC connection string identifies the driver in one of two ways:

- **`Driver=…`** names the ODBC driver directly — either a driver *name*
  registered in `odbcinst.ini` (`Driver=SQLite3`) or the *path* to the driver's
  shared library (`Driver=/usr/lib/.../libsqlite3odbc.so`). The rest of the
  keywords (`Server`, `Database`, `Uid`, …) are driver-specific. This is the form
  every template below uses.
- **`DSN=…`** names a Data Source Name — a driver plus a saved set of keywords —
  defined in `odbc.ini`. Use the `dsn` option, or put `DSN=name` in the `uri`.

The two can be combined: `DSN=name;Database=other;` starts from the DSN and
overrides individual keywords.

### Keywords and quoting

- Keywords are `Key=Value` pairs separated by `;`. Keys are case-insensitive.
- Order does not matter, except that with a bare `DSN=`/`Driver=` the driver
  manager reads keys left to right, so later keys override earlier ones.
- A value containing `;`, `]`, or a leading/trailing space must be wrapped in
  braces: `PWD={p;a}`. Braces are the ODBC quoting mechanism; adbcBridge does not
  add or strip them, so quote such values yourself.
- adbcBridge builds `UID=`/`PWD=` from the `username`/`password` options without
  quoting, so if a password needs braces, set it inside the `uri` yourself rather
  than via the `password` option.

---

## The compatibility templates

The templates below are the exact connection strings adbcBridge's compatibility
matrix (`tests/compat/test_matrix.py`) uses for each database, one per entry, 46
in total. In them:

- **`{drv}`** is the path to that database's vendor ODBC driver `.so`/`.dll`. In
  the matrix it is filled from a per-database environment variable
  (`<NAME>_ODBC_DRIVER`); in your own use, put the driver name or path there.
- **`{no_ssps}`** expands to `NO_SSPS=1;` **only on Windows**, and to nothing
  elsewhere. It turns off MySQL Connector/ODBC's server-side prepared statements,
  which that connector needs on Windows to reach a non-MySQL server.
- **`{plugin_dir}`** and **`{plugin}`** are two spellings of the same value: they
  expand to `PLUGIN_DIR=<driver-dir>/plugin;` when that directory exists (so
  MySQL Connector/ODBC can find its `mysql_native_password` client plugin), and
  to nothing otherwise.

A literal `NO_SSPS=1;` written into a template (as opposed to the `{no_ssps}`
placeholder) is unconditional on every platform — several MySQL-wire servers
reject server-side prepares outright and always need it.

---

## PostgreSQL wire

These all use **psqlodbc** (the PostgreSQL ODBC driver, `psqlodbcw.so`), which
serves every server that speaks the PostgreSQL wire protocol. adbcBridge tells
the real PostgreSQL apart from the wire-compatible forks with a connect-time
server probe (see [Driver quirks](quirks.md)).

| Database | Template |
|---|---|
| ArcadeDB | `Driver={drv};Server=127.0.0.1;Port=15441;Database=adbc;Uid=root;Pwd=Adbc2026;BoolsAsChar=0;` |
| Citus | `Driver={drv};Server=127.0.0.1;Port=15436;Database=adbc;Uid=adbc;Pwd=adbc;` |
| Cloudberry | `Driver={drv};Server=127.0.0.1;Port=15443;Database=adbc;Uid=gpadmin;Pwd=adbc;` |
| CockroachDB | `Driver={drv};Server=127.0.0.1;Port=16257;Database=defaultdb;Uid=root;` |
| CrateDB | `Driver={drv};Server=127.0.0.1;Port=15440;Database=doc;Uid=crate;` |
| Materialize | `Driver={drv};Server=127.0.0.1;Port=16875;Database=materialize;Uid=materialize;Protocol=7.4-0;` |
| openGauss | `Driver={drv};Server=127.0.0.1;Port=15438;Database=adbc;Uid=adbc;Pwd=Adbc@2026;` |
| PostgreSQL | `Driver={drv};Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| QuestDB | `Driver={drv};Server=127.0.0.1;Port=18812;Database=qdb;Uid=admin;Pwd=quest;BoolsAsChar=0;Protocol=7.4-0;` |
| RisingWave | `Driver={drv};Server=127.0.0.1;Port=14566;Database=dev;Uid=root;UseServerSidePrepare=0;` |
| Google Cloud Spanner | `Driver={drv};Server=127.0.0.1;Port=15442;Database=test-database;Uid=adbc;` |
| TimescaleDB | `Driver={drv};Server=127.0.0.1;Port=15434;Database=adbc;Uid=adbc;Pwd=adbc;` |
| YDB | `Driver={drv};Server=127.0.0.1;Port=15444;Database=local;Uid=adbcuser;Pwd=Ydb!Bridge2026;BoolsAsChar=0;UseServerSidePrepare=0;` |
| YugabyteDB | `Driver={drv};Server=127.0.0.1;Port=15433;Database=yugabyte;Uid=yugabyte;` |

Per-database notes:

- **Materialize** sets `Protocol=7.4-0` to disable psqlodbc's per-statement
  `SAVEPOINT` (Materialize has no savepoints).
- **QuestDB** and **ArcadeDB** and **YDB** set `BoolsAsChar=0` so booleans are
  described as a boolean type rather than a `char`.
- **RisingWave** and **YDB** set `UseServerSidePrepare=0`.
- **Google Cloud Spanner** is reached through PGAdapter, Google's PostgreSQL-wire
  proxy.
- **CrateDB**: the decimal type psqlodbc reports depends on the driver build —
  psqlodbc 16 (typical on Linux) describes the matrix's decimal column as
  `decimal128(28, 6)`, psqlodbc 18 (typical on macOS) as `decimal128(28, 3)`.
  Both are the driver's fallback rendering; the matrix accepts either.

---

## MySQL wire

These use **MySQL Connector/ODBC** (`libmyodbc9w.so`). The `{plugin_dir}` /
`{no_ssps}` placeholders and the unconditional `NO_SSPS=1;` are as described
above; MySQL-wire analytic warehouses without prepared-statement support need
`NO_SSPS=1` on every platform.

| Database | Template |
|---|---|
| Databend | `Driver={drv};Server=127.0.0.1;Port=13311;Database=default;User=root;Password=adbc;NO_SSPS=1;{plugin}` |
| Dolt | `Driver={drv};Server=127.0.0.1;Port=13310;Database=adbc;User=root;{plugin_dir}{no_ssps}` |
| Apache Doris | `Driver={drv};Server=127.0.0.1;Port=19031;User=root;NO_SSPS=1;{plugin_dir}` |
| GreptimeDB (MySQL wire) | `Driver={drv};Server=127.0.0.1;Port=14002;Database=public;User=greptime;NO_SSPS=1;{plugin_dir}` |
| MatrixOne | `Driver={drv};Server=127.0.0.1;Port=16001;User=dump;Password=111;{plugin_dir}{no_ssps}` |
| MongoDB BI Connector | `Driver={drv};Server=127.0.0.1;Port=13315;Database=adbc;User=adbc;NO_SSPS=1;{plugin_dir}` |
| MySQL | `Driver={drv};Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;` |
| OceanBase | `Driver={drv};Server=127.0.0.1;Port=12881;Database=adbc;User=root@test;Password=adbc;{plugin_dir}` |
| Percona Server | `Driver={drv};Server=127.0.0.1;Port=13312;Database=adbc;User=adbc;Password=adbc;` |
| StarRocks | `Driver={drv};Server=127.0.0.1;Port=19030;User=root;NO_SSPS=1;{plugin_dir}` |
| TiDB | `Driver={drv};Server=127.0.0.1;Port=14000;Database=test;User=root;{plugin_dir}{no_ssps}` |

Per-database notes:

- **Databend**, **Apache Doris**, **GreptimeDB**, **StarRocks** and the
  **MongoDB BI Connector** set an unconditional `NO_SSPS=1;` — their servers do
  not support `COM_STMT_PREPARE`.
- **MatrixOne** and **OceanBase** carry no `Database=` in the template
  (the schema is created during test setup); OceanBase's `User=root@test` puts
  the tenant in the login name.
- **MySQL** and **MariaDB** are set to `ANSI_QUOTES` mode during test setup so
  identifiers can be double-quoted.

---

## MariaDB Connector/ODBC

These use **MariaDB Connector/ODBC** (`libmaodbc.so`) rather than MySQL's
connector. `{no_ssps}` applies (Windows only).

| Database | Template |
|---|---|
| MariaDB | `Driver={drv};Server=127.0.0.1;Port=13306;Database=adbc;User=adbc;Password=adbc;{no_ssps}` |
| MariaDB ColumnStore | `Driver={drv};Server=127.0.0.1;Port=13313;Database=adbc;User=adbc;Password=Adbc!Bridge2026;{no_ssps}` |

ColumnStore is a storage engine inside a MariaDB server; the matrix creates its
tables with `ENGINE=Columnstore`.

---

## SQL Server

These use the **Microsoft ODBC Driver 18 for SQL Server** (`msodbcsql`).

| Database | Template |
|---|---|
| Microsoft SQL Server | `Driver={drv};Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;` |
| Azure SQL Edge | `Driver={drv};Server=127.0.0.1,14332;Database=master;Uid=sa;Pwd=Adbc!Bridge2026;TrustServerCertificate=yes;` |

SQL Server uses `Server=host,port` (a comma, not a colon).
`TrustServerCertificate=yes` accepts the server's self-signed certificate.

---

## IBM DRDA (Db2 and Informix)

Both are reached through IBM's **Db2 CLI driver** (`libdb2.so`), which speaks DRDA
to Db2 and to Informix's DRDA listener. adbcBridge asks the server which one it is
at connect time. The freely downloadable CLI driver is a 32-bit-`SQLLEN` build,
which adbcBridge autodetects (see `adbc.odbc.sqllen_32bit` in
[Options](options.md)).

| Database | Template |
|---|---|
| IBM Db2 | `Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=50000;Protocol=TCPIP;Uid=db2inst1;Pwd=Adbc2026;` |
| IBM Informix | `Driver={drv};Database=adbc;Hostname=127.0.0.1;Port=19089;Protocol=TCPIP;Uid=informix;Pwd=in4mix;` |

---

## Arrow Flight SQL

These use the **Arrow Flight SQL ODBC driver** (Dremio's), the one ODBC driver
for any Arrow Flight SQL server.

| Database | Template |
|---|---|
| Flight SQL (sqlflite) | `Driver={drv};Host=127.0.0.1;Port=31337;UID=sqlflite_username;PWD=adbc;useEncryption=false;` |
| InfluxDB 3 | `Driver={drv};Host=127.0.0.1;Port=18181;useEncryption=false;database=adbc;` |
| Dremio | `Driver={drv};Host=127.0.0.1;Port=32010;UID=adbc;PWD=Adbc2026pass;useEncryption=false;schema=$scratch;` |

---

## File-based (no server)

| Database | Template |
|---|---|
| SQLite | `Driver={drv};Database=<path>/m.db;` |
| DuckDB | `Driver={drv};Database=<path>/duck.db;` |
| Microsoft Access | `Driver={drv};DBQ=<path>/access.mdb;` |

SQLite uses **SQLiteODBC**, DuckDB its own ODBC driver, Access **MDB Tools**
(`libmdbodbcW.so`). Access uses the `DBQ=` keyword for the file path, not
`Database=`. In the matrix `<path>` is a temporary directory; substitute your own
file path.

---

## Databases with their own native protocol and ODBC driver

| Database | Driver | Template |
|---|---|---|
| Oracle | Oracle Instant Client ODBC (SQORA) | `Driver={drv};DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;` |
| ClickHouse | clickhouse-odbc | `Driver={drv};Url=http://127.0.0.1:18123;Database=adbc;UID=adbc;PWD=adbc;` |
| MonetDB | libMonetODBC | `Driver={drv};Host=127.0.0.1;Port=15000;Database=adbc;Uid=monetdb;Pwd=adbc;` |
| Firebird | OdbcFb (`libOdbcFb.so`) | `Driver={drv};DBNAME=inet://127.0.0.1:13050//var/lib/firebird/data/adbc.fdb;UID=adbc;PWD=adbc;CHARSET=UTF8;` |
| Vertica | Vertica client ODBC | `Driver={drv};Server=127.0.0.1;Port=15433;Database=VMart;UID=dbadmin;PWD=;` |
| OpenLink Virtuoso | virtodbc (ANSI) | `Driver={drv};HOST=127.0.0.1:11111;UID=dba;PWD=adbc;` |
| Apache Ignite | libignite-odbc | `Driver={drv};ADDRESS=127.0.0.1:11800;SCHEMA=PUBLIC;` |
| OpenSearch | OpenSearch SQL ODBC | `Driver={drv};host=127.0.0.1;port=19200;auth=NONE;useSSL=0;` |
| TDengine | taos-odbc | `Driver={drv};SERVER=127.0.0.1:16030;UID=root;PWD=taosdata;TIMESTAMP_AS_IS=1;` |

Per-database notes:

- **Oracle** uses `DBQ=host:port/service`, and the matrix additionally sets the
  environment variable `NLS_LANG=.AL32UTF8` for UTF-8. Vertica's template shows an
  empty password (`PWD=;`).
- **Firebird** carries the full database path inside `DBNAME=inet://…` and sets
  `CHARSET=UTF8`.
- **Virtuoso** puts host and port together in `HOST=host:port` and has no
  `Database=` keyword. The matrix uses the ANSI driver `virtodbc.so`, not the
  Unicode `virtodbcu.so`.
- **Ignite** uses `ADDRESS=host:port` and `SCHEMA=`.
- **OpenSearch** uses lower-case `host`/`port` keyword names and is read-only.
- **TDengine** uses `SERVER=host:port` and sets `TIMESTAMP_AS_IS=1`; the database
  is created during setup, so the template has no `DB=`.

---

Two databases the project documents as having no working ODBC route — H2 and
libSQL — are not part of the matrix and have no template here.
