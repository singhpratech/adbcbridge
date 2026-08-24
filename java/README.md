# adbcbridge for Java

Arrow ADBC over any ODBC driver, from Java. `org.adbcbridge:adbcbridge` finds
the native `libadbc_driver_odbc` library, hands it to the ADBC driver manager
(`adbc-driver-jni`), and gives you an `AdbcDatabase` on an ODBC connection
string. Everything after that is the plain
[ADBC Java API](https://arrow.apache.org/adbc/current/java/api/): connections,
statements, `ArrowReader`, `VectorSchemaRoot`.

## Dependency

```xml
<dependency>
  <groupId>org.adbcbridge</groupId>
  <artifactId>adbcbridge</artifactId>
  <version>0.1.0</version>
</dependency>
```

The jar pulls in `adbc-core`, `adbc-driver-jni` (which bundles the native ADBC
driver manager for Linux, macOS and Windows), `arrow-vector`, and
`arrow-memory-netty` at run time as the allocator backend. Java 11 or later.

## The three calls

```java
import org.adbcbridge.AdbcBridge;
import org.apache.arrow.adbc.core.*;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

// 1. Where is the driver? Absolute path, or DriverNotFoundException listing every place searched.
String path = AdbcBridge.driverPath();

try (RootAllocator allocator = new RootAllocator()) {
  // 2. An AdbcDriver (the ADBC driver manager) wired to that path, for callers that build
  //    their own parameter maps: PARAM_URI is the ODBC connection string.
  AdbcDriver driver = AdbcBridge.driver(allocator);

  // 3. Or open a database on a connection string outright. The map holds extra database
  //    options (ADBC option names, string values) and may be null.
  try (AdbcDatabase database =
          AdbcBridge.open(allocator, "DSN=warehouse;UID=me;PWD=secret;", null);
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
}
```

Any ODBC connection string works: a DSN (`DSN=...;`) or a DSN-less one
(`Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=my.db;`).
The driver's own options (`adbc.odbc.delegate`, and the rest documented in the
repository README) go in the options map.

## `--add-opens` on JDK 17 and later

Arrow's off-heap allocator reaches into `java.nio`. Start the JVM with

```
--add-opens=java.base/java.nio=ALL-UNNAMED
```

(and, with the Netty allocator, `-Dio.netty.tryReflectionSetAccessible=true`).
Without it the first allocation fails with an `InaccessibleObjectException`
or a `RuntimeException: Failed to initialize MemoryUtil`. This project's own
tests pass the flag through Surefire's `argLine`; put it in `JAVA_TOOL_OPTIONS`,
your launcher script, or `<argLine>` for your own builds.

## Where the native driver comes from

`AdbcBridge.driverPath()` looks, in order, at

1. the `adbcbridge.library` system property;
2. the `ADBCBRIDGE_LIBRARY` environment variable;
3. the `ADBC_ODBC_DRIVER` environment variable (what the rest of this
   repository uses);
4. a copy bundled inside the jar under
   `/org/adbcbridge/native/<os>-<arch>/` (`linux-x86_64`, `linux-aarch64`,
   `macos-aarch64`, `windows-x86_64`), extracted once per JVM to a temporary
   directory;
5. the ADBC driver manifest named `odbc` (`odbc.toml`, installed by
   `cmake --install`) in the directories the ADBC driver manager searches
   (`ADBC_DRIVER_PATH`, `~/.config/adbc/drivers`, `/etc/adbc/drivers`, ...);
6. common install locations (`/usr/local/lib`, `/usr/lib`,
   `/opt/adbcbridge/lib`, Homebrew, an active virtualenv or conda prefix) and a
   CMake `build/` tree next to a source checkout.

An explicit setting (1-3) that does not point at a file is an error. When
nothing is found, `DriverNotFoundException` lists every place that was tried.

### Bundling the native driver in the jar

The published jar can carry the driver for several platforms. Lay the built
libraries out as `<os>-<arch>/libadbc_driver_odbc.*`:

```
natives/
  linux-x86_64/libadbc_driver_odbc.so
  linux-aarch64/libadbc_driver_odbc.so
  macos-aarch64/libadbc_driver_odbc.dylib
  windows-x86_64/adbc_driver_odbc.dll
```

and point the build at that directory:

```
mvn -Dadbcbridge.natives=/path/to/natives package
```

`java/natives/` is the default location and is gitignored. Without the
directory the jar is pure Java and relies on the lookup above. The extracted
copy keeps its file name, so `lsof`/`ldd`-style output still says
`libadbc_driver_odbc.so`.

## Building and testing

```
mvn package            # jar + sources jar + javadoc jar in target/
mvn test               # needs SQLITE_ODBC_DRIVER, skipped otherwise
mvn javadoc:jar
```

The test connects to a temporary SQLite database through the SQLite ODBC
driver named in `SQLITE_ODBC_DRIVER` (a path to `libsqlite3odbc.so` or a name
from `odbcinst.ini`), runs `SELECT 1`, and reads the Arrow batch. Set
`ADBC_ODBC_DRIVER` to a fresh build of the driver to test that one.

## Releasing

`mvn -Prelease deploy` signs every artifact with `maven-gpg-plugin` and uploads
the bundle to the Maven Central Portal with `central-publishing-maven-plugin`.
The profile is inert unless activated and needs a GPG key on the machine plus a
`<server id="central">` with a Portal token in `settings.xml`; nothing is
published automatically (`autoPublish` is off).
