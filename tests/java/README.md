# Java smoke test

Proves that `libadbc_driver_odbc.so` loads and works through the *Java* ADBC
driver manager, not just the Python, Go and Rust ones — the driver is a plain C
shared library, so every ADBC binding reaches it the same way.

Java gets there through
[`adbc-driver-jni`](https://central.sonatype.com/artifact/org.apache.arrow.adbc/adbc-driver-jni),
which ships the native ADBC driver manager inside the jar (`libadbc_driver_jni.so`
for linux-x86_64/aarch64, plus macOS and Windows) and JNI bindings for it. Point
its `jni.driver` parameter at a `.so` and it `dlopen`s it and calls
`AdbcDriverInit`; everything else — `uri` included — is passed straight through
to the driver, which uses it as the ODBC connection string.

The Maven project is standalone (not a module of anything) and depends only on
published artifacts:

| artifact | version | why |
|---|---|---|
| `org.apache.arrow.adbc:adbc-core` | 0.24.0 | the `AdbcDatabase` / `AdbcConnection` / `AdbcStatement` interfaces |
| `org.apache.arrow.adbc:adbc-driver-jni` | 0.24.0 | `JniDriver` — the native driver manager, which loads our `.so` |
| `org.apache.arrow:arrow-vector` | 19.0.0 | `VectorSchemaRoot`, the vectors the tests assert on |
| `org.apache.arrow:arrow-memory-netty` | 19.0.0 | a concrete allocator behind `RootAllocator` |
| `org.junit.jupiter:junit-jupiter` | 5.11.4 | the test harness |

The Arrow version must match the one ADBC 0.24.0 was built against (19.0.0), and
`adbc-driver-jni` exists only in ADBC >= 0.21.

## Running

Everything runs in a container, so no JDK, no Maven and no `unixodbc` are needed
on the host — only the driver you are testing. Build it first, then, **from the
repo root**:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j

docker run --rm \
  -v "$PWD":/work \
  -v /usr/lib/x86_64-linux-gnu/odbc:/odbc:ro \
  -v adbcbridge-m2:/root/.m2 \
  -w /work/tests/java \
  -e ADBC_ODBC_DRIVER=/work/build/libadbc_driver_odbc.so \
  -e SQLITE_ODBC_DRIVER=/odbc/libsqlite3odbc.so \
  maven:3-eclipse-temurin-21 \
  bash -c 'apt-get update -qq && apt-get install -y -qq unixodbc && mvn -B test'
```

- `-v /usr/lib/x86_64-linux-gnu/odbc:/odbc:ro` — the directory holding
  `libsqlite3odbc.so` on the host (Debian/Ubuntu: `apt install libsqliteodbc`).
  Replace it with wherever your copy lives; the container only ever reads it.
- `apt-get install unixodbc` — the image has no driver manager, and both
  `libadbc_driver_odbc.so` and `libsqlite3odbc.so` need `libodbc.so.2`. The
  `-dev` package is not needed: nothing is compiled against ODBC here.
- `-v adbcbridge-m2:/root/.m2` — optional, caches the ~40 MB of Maven downloads
  between runs (`docker volume rm adbcbridge-m2` to drop it).

Expected output:

```
[INFO] Running org.adbcbridge.smoke.SmokeTest
[INFO] Tests run: 3, Failures: 0, Errors: 0, Skipped: 0
[INFO] BUILD SUCCESS
```

Maven runs as root in the container, so `tests/java/target/` comes out
root-owned (it is gitignored). Remove it the same way it was made:

```sh
docker run --rm -v "$PWD":/work maven:3-eclipse-temurin-21 rm -rf /work/tests/java/target
```

The driver `.so` is built on the host but loaded inside the container, so the
host's glibc must be no newer than the image's (both are Ubuntu 24.04 /
glibc 2.39 here). If they differ, build the driver in the container too — add
`apt-get install -y cmake g++ unixodbc-dev` and a `cmake` invocation to the
`bash -c` line.

## Environment

| variable | default | meaning |
|---|---|---|
| `ADBC_ODBC_DRIVER` | `../../build/libadbc_driver_odbc.so` (relative to this project) | the driver under test. Inside the container it must be a container path — hence `/work/build/...`. |
| `SQLITE_ODBC_DRIVER` | `SQLite3` | the SQLite ODBC driver to bridge to: an absolute path to `libsqlite3odbc.so`, or a name registered in `odbcinst.ini`. Passed through verbatim as `Driver=...`. |

No DSN, no `odbc.ini` and no server are needed: each test gets its own SQLite
database file under a JUnit `@TempDir`, and connects with a full connection
string — `Driver=/odbc/libsqlite3odbc.so;Database=/tmp/.../smoke.db;`.

## What the tests cover

`src/test/java/org/adbcbridge/smoke/SmokeTest.java`:

- **`createInsertSelect`** — `CREATE TABLE`, three `INSERT`s (each reporting one
  affected row), then `SELECT id, name FROM t ORDER BY id` read out of the
  `VectorSchemaRoot` batch by batch. Asserts the column names, the integers, a
  SQL `NULL` that stays `null`, and `"héllo 🎉"` surviving the ODBC
  UTF-16 → Arrow UTF-8 conversion intact (including the non-BMP code point,
  which Java stores as a surrogate pair).
- **`parameterisedInsertAndSelect`** — prepares
  `INSERT INTO people (id, name) VALUES (?, ?)`, binds one three-row
  `VectorSchemaRoot` of parameters (including a NULL name) and checks that
  `executeUpdate` reports 3 rows; then binds a single-row root to
  `SELECT name FROM people WHERE id = ?` and asserts only the matching row comes
  back.
- **`errorCarriesAMessage`** — querying a missing table throws `AdbcException`
  whose message names the table, i.e. the ODBC diagnostic record survives the
  trip through JNI rather than being swallowed.

Every test closes its `RootAllocator` in `@AfterEach`, which throws if any Arrow
buffer was leaked — that is what catches ownership mistakes on the C data
interface boundary, in the driver as well as in the test.

`ReadmeSnippet.java` is the "Use from Java" snippet from the top-level
`README.md`. It is never run — the paths in it are placeholders — but Maven
compiles it, so the snippet cannot silently rot.

## Notes

- `JniDriver.PARAM_DRIVER` (`jni.driver`) takes a path, a bare library name, or
  a driver-manifest name; the native driver manager resolves all three. A path
  is used here so the build tree is tested.
- `AdbcDriver.PARAM_URI` (`uri`) is only interpreted by the driver manager when
  it looks like `scheme://…` and no driver is set. An ODBC connection string is
  neither, so it reaches adbcbridge untouched.
- Arrow's off-heap allocator needs `--add-opens=java.base/java.nio=ALL-UNNAMED`
  on JDK 17+; the pom passes it to Surefire, and applications need it too.
- `target/` is gitignored. There is no `pom.lock`; the versions above are pinned
  in `pom.xml`.
