<!-- SPDX-License-Identifier: Apache-2.0 -->

# Java

adbcBridge is a plain-C11 ADBC driver that talks to any ODBC data source. ADBC
(Arrow Database Connectivity) is a database API whose result sets are Apache
Arrow batches; ODBC (Open Database Connectivity) is the older, row-based C API
that almost every database ships a driver for. The driver is one shared
library — `libadbc_driver_odbc.so` on Linux, `.dylib` on macOS,
`libadbc_driver_odbc.dll` on Windows.

The `org.adbcbridge:adbcbridge` Maven artifact is a thin wrapper around that
library. It **finds** the shared library on the machine (or unpacks the copy
bundled inside the jar), then hands it to the ADBC driver manager
(`adbc-driver-jni`) and gives you an `AdbcDatabase` on an ODBC connection
string. Everything after that is the plain
[Arrow ADBC Java API](https://arrow.apache.org/adbc/current/java/api/):
connections, statements, `ArrowReader`, `VectorSchemaRoot`.

This page assumes you are comfortable in Java but new to ODBC and ADBC.

---

## Table of contents

- [What the jar contains](#what-the-jar-contains)
- [Requirements](#requirements)
- [The `--add-opens` flag](#the---add-opens-flag)
- [Install](#install)
- [The three entry points](#the-three-entry-points)
- [Without the wrapper: `adbc-driver-jni` directly](#without-the-wrapper-adbc-driver-jni-directly)
- [How the library is located](#how-the-library-is-located)
- [Connection strings](#connection-strings)
- [Driver options](#driver-options)
- [Running a query](#running-a-query)
- [Bulk ingest](#bulk-ingest)
- [Parameters](#parameters)
- [Metadata](#metadata)
- [Errors](#errors)
- [JDBC comparison (measured)](#jdbc-comparison-measured)
- [Known limitations](#known-limitations)
- [Complete worked example](#complete-worked-example)

---

## What the jar contains

| Item | Value |
|---|---|
| groupId | `org.adbcbridge` |
| artifactId | `adbcbridge` |
| version | `0.1.0` |
| Automatic-Module-Name | `org.adbcbridge` |
| Minimum Java | 11 |
| License | Apache-2.0 |

The jar always contains the wrapper classes. It **may** also bundle the native
driver, depending on how it was built:

- The jar attached to a GitHub Release bundles the native library for several
  platforms under `/org/adbcbridge/native/<os>-<arch>/`. The layouts built for
  releases are `linux-x86_64`, `linux-aarch64`, `macos-aarch64` and
  `windows-x86_64`. At run time the wrapper extracts the one matching this JVM
  into a temporary directory, once per JVM, keeping its file name.
- A jar built without a natives directory is pure Java; it relies on finding the
  library elsewhere on the machine (see
  [How the library is located](#how-the-library-is-located)).

The jar pulls in these dependencies at run time:

| Dependency | Version | Role |
|---|---|---|
| `org.apache.arrow.adbc:adbc-core` | `0.24.0` | The ADBC Java API. |
| `org.apache.arrow.adbc:adbc-driver-jni` | `0.24.0` | The ADBC driver manager; bundles the native manager for Linux, macOS and Windows. |
| `org.apache.arrow:arrow-vector` | `19.0.0` | Arrow vectors and `VectorSchemaRoot`. |
| `org.apache.arrow:arrow-memory-netty` | `19.0.0` | The off-heap allocator backend (`runtime` scope). |

Note: `adbc-driver-jni 0.24.0` and Arrow `19.0.0` are built for Java 11, and so
is this library. The Arrow version must match the one ADBC `0.24.0` was built
against (`19.0.0`).

Tip: Netty is the default allocator backend. If you prefer the "unsafe"
allocator, exclude `arrow-memory-netty` and add `arrow-memory-unsafe` instead.

---

## Requirements

1. **An ODBC driver manager**, which the native library links against:
   - Linux: unixODBC (provides `libodbc.so.2`); on Debian/Ubuntu, `unixodbc`.
   - macOS: unixODBC or iODBC.
   - Windows: built into the operating system.
2. **The ODBC driver for your database** (for example the SQLite ODBC driver
   `libsqlite3odbc.so`). adbcBridge bridges to whichever ODBC drivers are
   installed; it does not contain them.
3. A JDK, version 11 or later (17+ needs the flag below).

---

## The `--add-opens` flag

Arrow's off-heap allocator reaches into `java.nio`. On **JDK 17 and later** you
must start the JVM with:

```
--add-opens=java.base/java.nio=ALL-UNNAMED
```

and, with the Netty allocator (the default), also:

```
-Dio.netty.tryReflectionSetAccessible=true
```

Without them the first allocation fails with an `InaccessibleObjectException`
or `RuntimeException: Failed to initialize MemoryUtil`.

Put the flag in your launcher script, in `JAVA_TOOL_OPTIONS`, or in your build
plugin's argument list. For example, with the Surefire test plugin:

```xml
<plugin>
  <groupId>org.apache.maven.plugins</groupId>
  <artifactId>maven-surefire-plugin</artifactId>
  <configuration>
    <argLine>--add-opens=java.base/java.nio=ALL-UNNAMED -Dio.netty.tryReflectionSetAccessible=true</argLine>
  </configuration>
</plugin>
```

On the command line:

```sh
java --add-opens=java.base/java.nio=ALL-UNNAMED \
     -Dio.netty.tryReflectionSetAccessible=true \
     -cp "app.jar:$(cat classpath.txt)" com.example.Main
```

---

## Install

### From Maven Central

```xml
<dependency>
  <groupId>org.adbcbridge</groupId>
  <artifactId>adbcbridge</artifactId>
  <version>0.1.0</version>
</dependency>
```

`adbc-core`, `adbc-driver-jni`, `arrow-vector` and `arrow-memory-netty` come in
transitively; the jar carries the native driver for Linux x86_64 and aarch64,
macOS arm64 and Windows x64. Published from the release tag by the
**Publish to Maven Central** workflow (`java/PUBLISHING.md`); the same jar, with
its sources and javadoc jars, is attached to the
[GitHub release](https://github.com/singhpratech/adbcbridge/releases) as well.

### From a GitHub Release jar (offline, or a build you made yourself)

`mvn install:install-file` puts a downloaded jar into the local repository, but
generates a dependency-less POM, so declare the runtime dependencies alongside:

```sh
mvn install:install-file \
    -Dfile=adbcbridge-0.1.0.jar \
    -DgroupId=org.adbcbridge \
    -DartifactId=adbcbridge \
    -Dversion=0.1.0 \
    -Dpackaging=jar
```

```xml
<dependencies>
  <dependency>
    <groupId>org.adbcbridge</groupId>
    <artifactId>adbcbridge</artifactId>
    <version>0.1.0</version>
  </dependency>
  <!-- Only needed for a locally installed jar; Maven Central resolves them transitively. -->
  <dependency>
    <groupId>org.apache.arrow.adbc</groupId>
    <artifactId>adbc-driver-jni</artifactId>
    <version>0.24.0</version>
  </dependency>
  <dependency>
    <groupId>org.apache.arrow</groupId>
    <artifactId>arrow-memory-netty</artifactId>
    <version>19.0.0</version>
    <scope>runtime</scope>
  </dependency>
</dependencies>
```

### Building the jar yourself

From a source checkout:

```sh
cd java
mvn package                                    # jar + sources + javadoc jars in target/
mvn test                                        # needs SQLITE_ODBC_DRIVER, skipped otherwise
mvn package -Dadbcbridge.natives=/path/to/natives   # bundle native drivers into the jar
```

`adbcbridge.natives` points at a directory laid out as
`<os>-<arch>/libadbc_driver_odbc.*`:

```
natives/
  linux-x86_64/libadbc_driver_odbc.so
  linux-aarch64/libadbc_driver_odbc.so
  macos-aarch64/libadbc_driver_odbc.dylib
  windows-x86_64/libadbc_driver_odbc.dll
```

`java/natives/` is the default location and is gitignored. Without such a
directory the jar is pure Java and finds the driver on the machine instead.
[`java/README.md`](../../java/README.md) covers the wrapper's own build and
the Maven Central publishing profile.

---

## The three entry points

Everything the wrapper adds lives on the final class `org.adbcbridge.AdbcBridge`:

```java
import org.adbcbridge.AdbcBridge;
import org.apache.arrow.adbc.core.*;
import org.apache.arrow.memory.RootAllocator;

// 1. Where is the native library? Absolute path, or DriverNotFoundException.
String path = AdbcBridge.driverPath();

try (RootAllocator allocator = new RootAllocator()) {
    // 2. An AdbcDriver (backed by the ADBC driver manager) wired to that path.
    AdbcDriver driver = AdbcBridge.driver(allocator);

    // 3. Or open a database on a connection string outright.
    try (AdbcDatabase database =
             AdbcBridge.open(allocator, "DSN=warehouse;UID=me;PWD=secret;", null)) {
        // ... database.connect(), etc.
    }
}
```

| Method | Returns | Notes |
|---|---|---|
| `AdbcBridge.driverPath()` | `String` | Absolute path of the native library, cached for the life of the JVM. Throws `DriverNotFoundException`. |
| `AdbcBridge.driver(BufferAllocator allocator)` | `AdbcDriver` | An `AdbcDriver` backed by the ADBC driver manager (`JniDriver`) and wired to `driverPath()`. Find failures surface here, not at first `open`. |
| `AdbcBridge.open(BufferAllocator allocator, String connectionString, Map<String,Object> options)` | `AdbcDatabase` | Opens a database with `connectionString` as the ODBC `uri`; `options` may be `null`. Throws `AdbcException`. |

Public constants:

| Constant | Value |
|---|---|
| `AdbcBridge.DRIVER_NAME` | `odbc` |
| `AdbcBridge.ENV_LIBRARY` | `ADBCBRIDGE_LIBRARY` |
| `AdbcBridge.ENV_DRIVER` | `ADBC_ODBC_DRIVER` |
| `AdbcBridge.PROPERTY_LIBRARY` | `adbcbridge.library` |

`AdbcBridge.open` sets `AdbcDriver.PARAM_URI` (`uri`) to the connection string
and passes every other entry of the options map to the driver as a database
option. A `uri` entry in `options` is overridden by `connectionString`.

---

## Without the wrapper: `adbc-driver-jni` directly

The wrapper is a convenience, not a requirement. `adbc-driver-jni` bundles the
native ADBC driver manager, so Java loads `libadbc_driver_odbc.so` the same way
every other binding does — with only the upstream artifacts on the classpath:

```xml
<!-- pom.xml -->
<dependency>
  <groupId>org.apache.arrow.adbc</groupId>
  <artifactId>adbc-driver-jni</artifactId>   <!-- ADBC >= 0.21 -->
  <version>0.24.0</version>
</dependency>
<dependency>
  <groupId>org.apache.arrow</groupId>
  <artifactId>arrow-memory-netty</artifactId> <!-- must match ADBC's Arrow -->
  <version>19.0.0</version>
  <scope>runtime</scope>
</dependency>
```

Point `JniDriver.PARAM_DRIVER` (`jni.driver`) at the library and put the ODBC
connection string in `AdbcDriver.PARAM_URI` (`uri`); from `open` onwards it is
the same API as above:

```java
import java.util.HashMap;
import java.util.Map;
import org.apache.arrow.adbc.core.*;
import org.apache.arrow.adbc.driver.jni.JniDriver;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

Map<String, Object> parameters = new HashMap<>();
JniDriver.PARAM_DRIVER.set(parameters, "/path/to/libadbc_driver_odbc.so");
AdbcDriver.PARAM_URI.set(
    parameters, "Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;");

try (BufferAllocator allocator = new RootAllocator();
    AdbcDatabase database = new JniDriver(allocator).open(parameters);
    AdbcConnection connection = database.connect();
    AdbcStatement statement = connection.createStatement()) {
  statement.setSqlQuery("SELECT * FROM my_table");
  try (AdbcStatement.QueryResult result = statement.executeQuery()) {
    ArrowReader reader = result.getReader();
    while (reader.loadNextBatch()) {
      VectorSchemaRoot root = reader.getVectorSchemaRoot();
      System.out.println(root.getRowCount() + " rows");
    }
  }
}
```

- `PARAM_DRIVER` takes a path, a bare library name, or a driver-manifest name
  (`"odbc"` — the `odbc.toml` manifest that `cmake --install` writes, step 5 of
  [How the library is located](#how-the-library-is-located)); the native driver
  manager resolves all three. `AdbcBridge.driver(allocator)` is exactly this
  wiring with `driverPath()` as the path.
- `PARAM_URI` is only interpreted by the driver manager when it looks like
  `scheme://…` and no driver is set. An ODBC connection string is neither, so it
  reaches adbcBridge untouched.
- The JVM still needs [the `--add-opens` flag](#the---add-opens-flag) on JDK 17+.

The snippet above is compiled — never run, its paths are placeholders — as
`tests/java/src/test/java/org/adbcbridge/smoke/ReadmeSnippet.java`, so it
cannot silently rot; `SmokeTest.java` beside it runs the same sequence against
SQLite. [`tests/java/README.md`](../../tests/java/README.md) has the container
command that runs those tests with no JDK, Maven or unixODBC on the host.

---

## How the library is located

`AdbcBridge.driverPath()` returns the first hit, in this order, and caches it:

| # | Where it looks | Detail |
|---|---|---|
| 1 | `adbcbridge.library` system property | The library's path. Set but not a file is an **error**, not a fall-through. |
| 2 | `ADBCBRIDGE_LIBRARY` environment variable | Same rule. |
| 3 | `ADBC_ODBC_DRIVER` environment variable | The variable the rest of the repository uses. Same rule. |
| 4 | A copy bundled in the jar | `/org/adbcbridge/native/<os>-<arch>/`, extracted once per JVM to a temporary directory (deleted on exit, best effort). |
| 5 | ADBC driver manifest `odbc.toml` | In the directories the ADBC driver manager searches: `ADBC_DRIVER_PATH`, an active `VIRTUAL_ENV`/`CONDA_PREFIX` (`etc/adbc/drivers`, `share/adbc/drivers`), `~/.config/adbc/drivers` (`$XDG_CONFIG_HOME/adbc/drivers`), `/etc/adbc/drivers`, `/usr/local/etc/adbc/drivers`, `/usr/share/adbc/drivers`, `/usr/local/share/adbc/drivers`; the macOS and Windows equivalents on those platforms. Written by `cmake --install`. |
| 6 | Common install locations | `/usr/local/lib`, `/usr/lib`, `/opt/adbcbridge/lib`, Homebrew, `lib`/`lib64` of an active virtualenv or conda prefix, and the `<arch>-linux-gnu` multiarch directories on Linux. |
| 7 | A CMake `build/` tree | `build/` and `build/{Release,Debug,RelWithDebInfo,MinSizeRel}` walked up from where the class was loaded and from the working directory — so a source checkout works right after `cmake --build build`. |

When nothing matches, `driverPath()` throws
`org.adbcbridge.DriverNotFoundException`. Its message lists every place searched,
in order, and `getSearched()` returns the same list (`List<String>`).

Troubleshooting: if `driverPath()` throws, read `getSearched()` — it names every
location tried. The quickest fix is to set `ADBC_ODBC_DRIVER` (or the
`adbcbridge.library` system property) to the absolute path of your
`libadbc_driver_odbc.*`, or install the driver with `cmake --install build` so
the `odbc.toml` manifest is written.

---

## Connection strings

The second argument to `AdbcBridge.open` is a plain ODBC connection string,
which becomes the ADBC database's `uri`. Two forms work:

- **DSN-less** (names the ODBC driver directly):

  ```text
  Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;
  ```

  `Driver=` takes a driver **name** from `odbcinst.ini` (`Driver=SQLite3;`) or
  the **path** of the ODBC driver library.

- **DSN** (names a data source from `odbc.ini`):

  ```text
  DSN=warehouse;UID=me;PWD=secret;
  ```

Real connection strings from the project's compatibility matrix:

| Database | Connection string |
|---|---|
| SQLite | `Driver=<libsqlite3odbc.so>;Database=/path/to/m.db;` |
| PostgreSQL | `Driver=<psqlodbcw.so>;Server=127.0.0.1;Port=15432;Database=adbc;Uid=adbc;Pwd=adbc;` |
| MySQL | `Driver=<libmyodbc.so>;Server=127.0.0.1;Port=13307;Database=adbc;User=adbc;Password=adbc;` |
| SQL Server | `Driver=<msodbcsql>;Server=127.0.0.1,14331;Database=master;Uid=sa;Pwd=…;TrustServerCertificate=yes;` |
| Oracle | `Driver=<liboraodbc.so>;DBQ=127.0.0.1:11521/FREEPDB1;UID=adbc;PWD=adbc;` |

Substitute the driver name or path for the value in angle brackets.

---

## Driver options

Pass driver options as the third argument to `AdbcBridge.open`, a
`Map<String,Object>` whose keys are ADBC option names and whose values are
usually strings:

```java
import java.util.Collections;

try (AdbcDatabase database = AdbcBridge.open(
        allocator,
        "Driver=SQLite3;Database=my.db;",
        Collections.singletonMap("adbc.odbc.batch_size", "4096"))) {
    // ...
}
```

The database also understands the generic ADBC options `uri` (the full ODBC
connection string), `dsn`, and `username` / `password`.

The `adbc.odbc.*` options below come from the driver:

| Option | Meaning |
|---|---|
| `adbc.odbc.batch_size` | Rows per Arrow batch (default `1024`). |
| `adbc.odbc.max_bind_bytes` | Widest value bound at the width the driver declares for it, in bytes (default `32768`). Wider values use `long_bind_bytes` or `SQLGetData`. |
| `adbc.odbc.long_bind_bytes` | Width, in bytes, at which to bind a column whose declared width is not a real bound — `TEXT`/`NVARCHAR(MAX)`/`LONGTEXT`/`bytea` and similar (default `2048`). Longer values are re-read in full, trading only speed. |
| `adbc.odbc.rowset_bytes` | Ceiling on a reader's bound rowset buffers, in bytes (default `8388608`). |
| `adbc.odbc.decimal_as_string` | `true` to return `DECIMAL`/`NUMERIC` as strings. |
| `adbc.odbc.partitions` | Partitions to split a query into for `executePartitions` — `0` (default) chooses from the table's size, `1` never splits. Set on the statement. |
| `adbc.odbc.prefetch` | Rowsets kept in flight on a background fetch thread — `0` (default) off, `1` double buffering, up to `8`. Settable on the database, connection, or statement. |
| `adbc.odbc.delegate` | `auto` (default) / `never` / `always` — see [Native delegation](#native-delegation-a-note). |
| `adbc.odbc.delegate.driver` | Force a specific native driver: a bare name (`postgresql`) or manifest name; a path only with `allow_paths`. |
| `adbc.odbc.delegate.search_path` | Extra directories to search for native drivers (`:`-separated); needs `allow_paths`. |
| `adbc.odbc.delegate.allow_paths` | `true` to let the two options above name filesystem paths (default `false`). |
| `adbc.odbc.delegate.last_error` | Read-only: why delegation did not happen. |
| `adbc.odbc.delegated_to` | Read-only: the native driver serving this database/connection, or `odbc`. |
| `adbc.odbc.tune` | `true` (default) / `false` — may the driver add ODBC connection keywords of its own where it recognises the target driver? `false` sends your connection string through untouched. |
| `adbc.odbc.sqllen_32bit` | `true`/`false` to force the 32-bit-`SQLLEN` driver quirk on or off. Autodetected (on for IBM Db2), so you normally never set it. Also settable on the connection and statement. |

### Native delegation (a note)

Some databases have a first-class native ADBC driver (PostgreSQL, SQLite,
DuckDB, Snowflake, BigQuery, Flight SQL). When `adbc.odbc.delegate` is `auto`
(the default) and adbcBridge recognises such a target, it hands the whole
database over to that native driver for native speed. Set
`adbc.odbc.delegate=never` to force the ODBC path, or `always` to make a missing
native driver an error. Delegation is not implemented on Windows — there `auto`
always takes the ODBC path.

---

## Running a query

`AdbcBridge.open` gives you an `AdbcDatabase`. From there, the flow is the plain
ADBC Java API: connect, create a statement, set its SQL, execute, and read Arrow
batches through an `ArrowReader`:

```java
import org.apache.arrow.adbc.core.*;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

try (RootAllocator allocator = new RootAllocator();
     AdbcDatabase database = AdbcBridge.open(allocator, "Driver=SQLite3;Database=my.db;", null);
     AdbcConnection connection = database.connect();
     AdbcStatement statement = connection.createStatement()) {

    statement.setSqlQuery("SELECT id, name FROM customers");
    try (AdbcStatement.QueryResult result = statement.executeQuery()) {
        ArrowReader reader = result.getReader();
        while (reader.loadNextBatch()) {
            VectorSchemaRoot batch = reader.getVectorSchemaRoot();
            System.out.println(batch.contentToTSVString());
        }
    }
}
```

- `statement.executeQuery()` returns an `AdbcStatement.QueryResult`
  (close it — `try`-with-resources).
- `result.getReader()` is an `ArrowReader`; `loadNextBatch()` returns `false`
  when the result set is exhausted, and `getVectorSchemaRoot()` is the current
  batch.
- For DML that returns no rows, call `statement.executeUpdate()` instead.

Transactions: call `connection.setAutoCommit(false)`, then
`connection.commit()` / `connection.rollback()` to control transactions
yourself. Autocommit is on by default.

---

## Bulk ingest

Bulk ingest writes an Arrow `VectorSchemaRoot` into a table.
`connection.bulkIngest` returns a bound statement; provide the data and execute:

```java
import org.apache.arrow.adbc.core.BulkIngestMode;
import org.apache.arrow.vector.VectorSchemaRoot;

try (VectorSchemaRoot root = /* build your batch */;
     AdbcStatement statement = connection.bulkIngest("my_table", BulkIngestMode.CREATE)) {
    statement.bind(root);
    statement.executeUpdate();
}
// With autocommit off, commit when the ingest is done:
connection.commit();
```

- `BulkIngestMode.CREATE` creates the table from the Arrow schema and inserts.
- The driver generates the `CREATE TABLE` DDL, then sends one multi-row `INSERT`
  per batch of rows inside a single transaction. It works on every ODBC driver
  that can bind a parameter. `adbc.odbc.rows_per_insert` overrides the
  rows-per-`INSERT` the driver chooses.

---

## Parameters

The driver supports parameter binding (`Bind` / `BindStream`). Parameters are
bound as a `VectorSchemaRoot`, one column per `?` and one row per execution:
prepare, bind the root, then `executeQuery()` or `executeUpdate()`:

```java
statement.setSqlQuery("SELECT * FROM customers WHERE id = ?");
statement.prepare();
statement.bind(parameterRoot);        // one column per '?', one row per execution
try (AdbcStatement.QueryResult result = statement.executeQuery()) {
    // ...
}
```

Binding a multi-row root applies the statement once per row — as a column-wise ODBC
parameter array where the driver handles them (`adbc.odbc.array_binding`, on by default),
row by row otherwise. Bulk ingest takes its own route, a multi-row `INSERT` rewrite by
default — see [bulk ingest](../how-it-works/performance.md#bulk-ingest). The placeholder syntax (`?`, `$1`,
`:name`, …) is whatever the underlying ODBC driver and database accept.

---

## Metadata

The ADBC connection exposes the standard metadata calls, backed by the ODBC
driver's catalog functions: `getInfo`, `getObjects` (catalogs, schemas, tables,
columns), `getTableTypes`, and `getTableSchema`. Each returns Arrow data through
the `AdbcConnection` you already have. Consult the
[ADBC Java API](https://arrow.apache.org/adbc/current/java/api/) for the exact
signatures on your version (`0.24.0`).

---

## Errors

Operations throw `org.apache.arrow.adbc.core.AdbcException` (`AdbcBridge.open`
declares it). The driver maps each ODBC diagnostic into a structured error
carrying the **SQLSTATE** (the five-character ODBC status code) and the driver's
**native error code**, so you can branch on the database's own error identity
rather than parsing message text. `AdbcException` also carries an ADBC status
code; the exact accessor names belong to the `adbc-core` API, so consult it for
the structured fields. Catch the exception and inspect it:

```java
try (AdbcConnection connection = database.connect();
     AdbcStatement statement = connection.createStatement()) {
    statement.setSqlQuery("SELECT * FROM does_not_exist");
    statement.executeQuery();
} catch (AdbcException ex) {
    System.err.println(ex.getMessage());   // includes SQLSTATE + native code
    // ex also exposes the ADBC status code and vendor/SQLSTATE fields; see adbc-core.
}
```

`org.adbcbridge.DriverNotFoundException` is a separate `RuntimeException`, thrown
only when the native library itself cannot be located (see
[How the library is located](#how-the-library-is-located)), before any database
work begins.

---

## JDBC comparison (measured)

The project's benchmark harness (`bench/java/`) runs the same workload two ways
from Java: over adbcBridge (through `adbc-driver-jni`), and over the database's
ordinary JDBC driver as a no-Arrow floor. The JDBC path uses a prepared `INSERT`
with `addBatch`/`executeBatch` for ingest and a plain `ResultSet` for reads. A
JDBC URL is derived automatically for SQLite (`sqlite-jdbc`) and PostgreSQL
(`postgresql`); other databases need a `<DB>_JDBC` environment variable and
otherwise show `—`.

Figures below are from `bench/LANGUAGE_BENCHMARKS.md` on the project's Linux
reference host: 10,000-row ingest and 100,000-row fetch of
`(id int32, val float64, txt utf8, dt date32)`, rows per second, median of 3
timings after a warmup, with native delegation off (`adbc.odbc.delegate=never`)
so every row really travels over ODBC.

| Database | adbcBridge ingest | JDBC ingest | adbcBridge fetch | JDBC fetch |
|---|---:|---:|---:|---:|
| SQLite | 743,890 | 265,672 | 1,309,780 | 1,371,841 |
| PostgreSQL | 450,072 | 222,359 | 1,617,704 | 3,261,189 |

Read these as ratios, not absolutes: the servers run locally, the host is not
idle, and the JDBC column is a different code path (its own bulk API), not a
like-for-like Arrow comparison. On this host and workload, adbcBridge ingest was
faster than batched JDBC on both databases; fetch was close on SQLite and slower
than the PostgreSQL JDBC driver's row reader. Your numbers will differ with
hardware, driver, and load.

Note: the "JDBC" here is only the *native comparison* client. adbcBridge itself
speaks ODBC, exactly as the Python, Rust and Go bindings do; it does not use
JDBC to reach the database.

---

## Known limitations

- **The ODBC stack is not bundled.** You still need an ODBC driver manager
  (unixODBC/iODBC on Unix, built in on Windows) and the ODBC driver for your
  database. The native library links against the driver manager
  (`libodbc.so.2` on Linux).
- **`--add-opens` is required on JDK 17+.** See
  [The `--add-opens` flag](#the---add-opens-flag).
- **Bundled natives are present only if the jar was built with them.** The
  release jar carries `linux-x86_64`, `linux-aarch64`, `macos-aarch64` and
  `windows-x86_64`. Any other platform, or a pure-Java jar, must find the library
  through the property, an environment variable, a manifest, or an install
  directory.
- **Extracted DLL cleanup on Windows.** The bundled library is extracted to a
  temporary directory and deleted on JVM exit as a best effort; Windows will not
  let a loaded DLL be removed, so a copy may remain.
- **`install:install-file` produces a dependency-less POM.** When installing from
  the release jar, declare the runtime dependencies yourself (see
  [Install](#install)); they come transitively once the artifact is on Maven
  Central.
- **Native delegation is not available on Windows** — `auto` always takes the
  ODBC path there and `always` fails with a message.
- **Version pinning.** The jar is built against ADBC `0.24.0` and Arrow
  `19.0.0`; the Arrow version must match the one ADBC was built against.

---

## Complete worked example

A minimal program that finds the driver, connects to a temporary SQLite database
through the SQLite ODBC driver, runs a query, and reads the Arrow result.

### `pom.xml` (dependencies and the classpath file the run step reads)

```xml
<dependencies>
  <dependency>
    <groupId>org.adbcbridge</groupId>
    <artifactId>adbcbridge</artifactId>
    <version>0.1.0</version>
  </dependency>

  <!-- Only needed when adbcbridge was installed from a downloaded jar;
       from Maven Central they resolve transitively. -->
  <dependency>
    <groupId>org.apache.arrow.adbc</groupId>
    <artifactId>adbc-driver-jni</artifactId>
    <version>0.24.0</version>
  </dependency>
  <dependency>
    <groupId>org.apache.arrow</groupId>
    <artifactId>arrow-memory-netty</artifactId>
    <version>19.0.0</version>
    <scope>runtime</scope>
  </dependency>
</dependencies>

<!-- Writes the runtime classpath to target/classpath.txt during `mvn package`,
     which the run command below reads. -->
<build>
  <plugins>
    <plugin>
      <groupId>org.apache.maven.plugins</groupId>
      <artifactId>maven-dependency-plugin</artifactId>
      <version>3.6.1</version>
      <executions>
        <execution>
          <id>build-classpath</id>
          <phase>package</phase>
          <goals>
            <goal>build-classpath</goal>
          </goals>
          <configuration>
            <outputFile>${project.build.directory}/classpath.txt</outputFile>
            <includeScope>runtime</includeScope>
          </configuration>
        </execution>
      </executions>
    </plugin>
  </plugins>
</build>
```

### `Main.java`

```java
// SPDX-License-Identifier: Apache-2.0
package com.example;

import java.nio.file.Files;
import org.adbcbridge.AdbcBridge;
import org.adbcbridge.DriverNotFoundException;
import org.apache.arrow.adbc.core.AdbcConnection;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcException;
import org.apache.arrow.adbc.core.AdbcStatement;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

public final class Main {
  public static void main(String[] args) throws Exception {
    // The SQLite ODBC driver: a name from odbcinst.ini (e.g. "SQLite3") or the
    // path of libsqlite3odbc.so. Point SQLITE_ODBC_DRIVER at it.
    String sqlite = System.getenv().getOrDefault("SQLITE_ODBC_DRIVER", "SQLite3");
    String dbFile = Files.createTempFile("adbcbridge-", ".db").toString();
    String uri = "Driver=" + sqlite + ";Database=" + dbFile + ";";

    try {
      // 1. Locate the native library up front (optional; open() does it too).
      System.out.println("driver: " + AdbcBridge.driverPath());

      try (BufferAllocator allocator = new RootAllocator();
           AdbcDatabase database = AdbcBridge.open(allocator, uri, null);
           AdbcConnection connection = database.connect();
           AdbcStatement statement = connection.createStatement()) {

        // 2. Run a query.
        statement.setSqlQuery("SELECT 42 AS answer, 'hi' AS greeting");

        // 3. Read the Arrow result.
        try (AdbcStatement.QueryResult result = statement.executeQuery()) {
          ArrowReader reader = result.getReader();
          while (reader.loadNextBatch()) {
            VectorSchemaRoot batch = reader.getVectorSchemaRoot();
            System.out.println("rows: " + batch.getRowCount());
            System.out.println(batch.contentToTSVString());
          }
        }
      }
    } catch (DriverNotFoundException e) {
      System.err.println("adbcBridge library not found:");
      System.err.println(e.getMessage());   // lists every place searched
      System.exit(1);
    } catch (AdbcException e) {
      System.err.println("database error: " + e.getMessage());
      System.exit(1);
    }
  }
}
```

### Run it (JDK 17+)

```sh
mvn package                                    # compiles and writes target/classpath.txt
export SQLITE_ODBC_DRIVER=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so
java --add-opens=java.base/java.nio=ALL-UNNAMED \
     -Dio.netty.tryReflectionSetAccessible=true \
     -cp "target/classes:$(cat target/classpath.txt)" \
     com.example.Main
```

Expected output (the temporary path and library path will differ):

```text
driver: /path/to/libadbc_driver_odbc.so
rows: 1
answer	greeting
42	hi
```
