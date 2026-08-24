/*
 * Copyright 2026 the adbcbridge authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package org.adbcbridge.bench;

import java.nio.charset.StandardCharsets;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import org.apache.arrow.adbc.core.AdbcConnection;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcDriver;
import org.apache.arrow.adbc.core.AdbcStatement;
import org.apache.arrow.adbc.core.BulkIngestMode;
import org.apache.arrow.adbc.driver.jni.JniDriver;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.DateDayVector;
import org.apache.arrow.vector.Float8Vector;
import org.apache.arrow.vector.IntVector;
import org.apache.arrow.vector.VarCharVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;
import org.apache.arrow.vector.types.Types;
import org.apache.arrow.vector.types.pojo.Field;
import org.apache.arrow.vector.types.pojo.Schema;
import org.apache.arrow.vector.util.Text;

/**
 * Java port of {@code bench/rust/src/main.rs}, one database per run.
 *
 * <p>Four measurements over the same 4-column table {@code (id int32, val float64, txt utf8, dt
 * date32)}, each the median of {@code --reps} timings after one warmup:
 *
 * <ol>
 *   <li><b>adbc ingest</b> — {@code libadbc_driver_odbc.so} through {@code adbc-driver-jni}: bulk
 *       ingest in {@code create} mode, autocommit off, one commit at the end, row count verified
 *       afterwards.
 *   <li><b>adbc fetch</b> — {@code SELECT id, val, txt, dt} drained into Arrow {@link
 *       VectorSchemaRoot} batches through the same driver.
 *   <li><b>jdbc ingest</b> — the same rows through the database's JDBC driver: a prepared {@code
 *       INSERT ... VALUES (?, ?, ?, ?)} with {@code addBatch}/{@code executeBatch}, autocommit off,
 *       one commit. The table is created by an empty ADBC ingest first, so both ingest paths write
 *       into identical DDL.
 *   <li><b>jdbc fetch</b> — the same SELECT read row by row from a {@link ResultSet}, no Arrow: the
 *       plain-Java floor for this read.
 * </ol>
 *
 * <p>The JDBC steps need a JDBC URL, which is derived from the ODBC connection string for SQLite and
 * PostgreSQL and can be given outright in {@code <DB>_JDBC}; without one those two columns read
 * {@code —}.
 *
 * <p>Usage:
 *
 * <pre>
 *   ADBC_ODBC_DRIVER=build/libadbc_driver_odbc.so POSTGRES_CONN='Driver=...;' \
 *       java ... org.adbcbridge.bench.Bench --rows 10000 --fetch-rows 100000 postgres
 * </pre>
 *
 * <p>The database name is only a label and an environment-variable prefix; see {@code run.sh},
 * which resolves the connection strings out of {@code tests/compat}.
 */
public final class Bench {
  /** Rows per {@code executeBatch} on the JDBC side. */
  private static final int ROWSET = 8192;

  private static String driverPath;
  private static String uri;
  private static List<String> setup = List.of();

  private Bench() {}

  // ------------------------------------------------------------------ payload

  /** The benchmark table: the same shape and values every other port of this benchmark uses. */
  private static final Schema SCHEMA =
      new Schema(
          Arrays.asList(
              Field.nullable("id", Types.MinorType.INT.getType()),
              Field.nullable("val", Types.MinorType.FLOAT8.getType()),
              Field.nullable("txt", Types.MinorType.VARCHAR.getType()),
              Field.nullable("dt", Types.MinorType.DATEDAY.getType())));

  /** Build {@code n} rows of the benchmark payload. The caller closes the root. */
  private static VectorSchemaRoot makeRoot(BufferAllocator allocator, int n) {
    VectorSchemaRoot root = VectorSchemaRoot.create(SCHEMA, allocator);
    IntVector ids = (IntVector) root.getVector("id");
    Float8Vector values = (Float8Vector) root.getVector("val");
    VarCharVector texts = (VarCharVector) root.getVector("txt");
    DateDayVector dates = (DateDayVector) root.getVector("dt");
    ids.allocateNew(n);
    values.allocateNew(n);
    texts.allocateNew(n);
    dates.allocateNew(n);
    for (int i = 0; i < n; i++) {
      ids.setSafe(i, i);
      values.setSafe(i, i * 0.5);
      texts.setSafe(i, text(i).getBytes(StandardCharsets.UTF_8));
      dates.setSafe(i, i % 20_000);
    }
    root.setRowCount(n);
    return root;
  }

  private static String text(int i) {
    return String.format(Locale.ROOT, "row-%012d", i);
  }

  // --------------------------------------------------------------------- ADBC

  /** A JNI driver, database and connection opened over {@code libadbc_driver_odbc.so}. */
  /**
   * ADBC_BENCH_AUTOCOMMIT: keep autocommit on even for the ingest steps.
   * MonetDBODBClib's SQLEndTran is a no-op, so a connection with autocommit off never
   * commits anything and the rows are gone by the time the fetch step opens its own
   * connection; on such a driver the only way to measure the ingest at all is to let the
   * bridge batch the stream itself, which is what bench/matrix_bench.py does everywhere.
   */
  private static final boolean FORCE_AUTOCOMMIT =
      System.getenv("ADBC_BENCH_AUTOCOMMIT") != null
          && !System.getenv("ADBC_BENCH_AUTOCOMMIT").isEmpty();

  private static final class Session implements AutoCloseable {
    final BufferAllocator allocator;
    final AdbcDatabase database;
    final AdbcConnection connection;

    /**
     * Open a connection through the driver under test. {@code adbc.odbc.delegate=never} keeps the
     * bridge on its own ODBC path instead of handing the connection to a database's native ADBC
     * driver, so the numbers describe this driver.
     */
    Session(boolean autoCommitIn) throws Exception {
      boolean autoCommit = autoCommitIn || FORCE_AUTOCOMMIT;
      allocator = new RootAllocator();
      Map<String, Object> parameters = new HashMap<>();
      JniDriver.PARAM_DRIVER.set(parameters, driverPath);
      AdbcDriver.PARAM_URI.set(parameters, uri);
      parameters.put("adbc.odbc.delegate", "never");
      AdbcDatabase db = null;
      AdbcConnection cnxn = null;
      try {
        db = new JniDriver(allocator).open(parameters);
        cnxn = db.connect();
        for (String sql : setup) {
          execute(cnxn, sql);
        }
        if (!autoCommit) {
          cnxn.setAutoCommit(false);
        }
      } catch (Exception e) {
        closeQuietly(cnxn, db, allocator);
        throw e;
      }
      database = db;
      connection = cnxn;
    }

    @Override
    public void close() {
      closeQuietly(connection, database, allocator);
    }
  }

  private static void closeQuietly(AutoCloseable... closeables) {
    for (AutoCloseable c : closeables) {
      if (c != null) {
        try {
          c.close();
        } catch (Exception ignored) {
          // Best effort.
        }
      }
    }
  }

  /** Run a statement for its side effect, discarding any row count. */
  private static void execute(AdbcConnection connection, String sql) throws Exception {
    try (AdbcStatement stmt = connection.createStatement()) {
      stmt.setSqlQuery(sql);
      stmt.executeUpdate();
    }
  }

  /**
   * DROP TABLE under every spelling of the name, ignoring failures: the table usually does not exist
   * yet, and a case-folding database answers to only one of the spellings.
   */
  private static void dropTable(Session session, List<String> names, boolean autoCommitIn) {
    boolean autoCommit = autoCommitIn || FORCE_AUTOCOMMIT;
    for (String name : names) {
      boolean dropped = true;
      try {
        execute(session.connection, "DROP TABLE " + name);
      } catch (Exception ignored) {
        // Expected when the table is not there.
        dropped = false;
      }
      if (!autoCommit) {
        try {
          // A failed statement leaves the transaction aborted on PostgreSQL and on
          // MonetDB -- and MonetDB refuses to end that with a COMMIT, insisting on a
          // ROLLBACK ("Current transaction is aborted (please ROLLBACK)"). Commit the
          // spelling that dropped, roll back the ones that did not, so the ingest's
          // CREATE TABLE starts from a clean transaction either way.
          if (dropped) {
            session.connection.commit();
          } else {
            session.connection.rollback();
            // MonetDBODBClib's SQLEndTran does not clear an aborted transaction -- the
            // next statement still fails with "Current transaction is aborted (please
            // ROLLBACK)". A literal ROLLBACK does clear it, and is harmless where the
            // driver manager already ended the transaction properly.
            try {
              execute(session.connection, "ROLLBACK");
            } catch (Exception ignored) {
              // Best effort.
            }
          }
        } catch (Exception ignored) {
          // Best effort.
        }
      }
    }
  }

  /**
   * Bulk-ingest {@code root} into {@code table} in {@code create} mode, commit once, and return the
   * seconds that took (DDL + rows + commit, as {@code matrix_bench.py}).
   */
  private static double adbcIngest(Session session, String table, VectorSchemaRoot root)
      throws Exception {
    long start = System.nanoTime();
    try (AdbcStatement stmt = session.connection.bulkIngest(table, BulkIngestMode.CREATE)) {
      stmt.bind(root);
      stmt.executeUpdate();
    }
    // Nothing to commit when ADBC_BENCH_AUTOCOMMIT put the connection in autocommit.
    if (!FORCE_AUTOCOMMIT) {
      session.connection.commit();
    }
    return (System.nanoTime() - start) / 1e9;
  }

  /** Drain a query into Arrow batches through the bridge and count the rows. */
  private static long adbcFetch(Session session, String sql) throws Exception {
    try (AdbcStatement stmt = session.connection.createStatement()) {
      stmt.setSqlQuery(sql);
      try (AdbcStatement.QueryResult result = stmt.executeQuery()) {
        ArrowReader reader = result.getReader();
        long rows = 0;
        while (reader.loadNextBatch()) {
          rows += reader.getVectorSchemaRoot().getRowCount();
        }
        return rows;
      }
    }
  }

  /** SELECT COUNT(*), tolerant of whatever type the database reports the count as. */
  private static long adbcCount(Session session, String ident) throws Exception {
    try (AdbcStatement stmt = session.connection.createStatement()) {
      stmt.setSqlQuery("SELECT COUNT(*) FROM " + ident);
      try (AdbcStatement.QueryResult result = stmt.executeQuery()) {
        ArrowReader reader = result.getReader();
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          if (root.getRowCount() == 0) {
            continue;
          }
          Object value = root.getVector(0).getObject(0);
          if (value instanceof Number) {
            return ((Number) value).longValue();
          }
          if (value instanceof Text || value instanceof String) {
            return Long.parseLong(value.toString().trim());
          }
          throw new IllegalStateException(
              "unexpected COUNT(*) type " + (value == null ? "null" : value.getClass()));
        }
      }
    }
    throw new IllegalStateException("COUNT(*) returned no rows");
  }

  private static void verify(Session session, String ident, int want) throws Exception {
    long got = adbcCount(session, ident);
    if (got != want) {
      throw new IllegalStateException("wrong row count " + got + " != " + want);
    }
  }

  // --------------------------------------------------------------------- JDBC

  /**
   * The JDBC URL for this database, or null if none can be had. {@code <DB>_JDBC} wins; otherwise
   * the SQLite and PostgreSQL ODBC connection strings are translated, since those are the two
   * databases whose JDBC drivers this project depends on.
   */
  private static String jdbcUrl(String db, String prefix, Properties properties) {
    String explicit = System.getenv(prefix + "_JDBC");
    if (explicit != null && !explicit.isEmpty()) {
      return explicit;
    }
    Map<String, String> odbc = new LinkedHashMap<>();
    for (String part : uri.split(";")) {
      int eq = part.indexOf('=');
      if (eq > 0) {
        odbc.put(part.substring(0, eq).trim().toLowerCase(Locale.ROOT), part.substring(eq + 1));
      }
    }
    if ("sqlite".equals(db)) {
      String file = odbc.get("database");
      return file == null ? null : "jdbc:sqlite:" + file;
    }
    if ("postgres".equals(db)) {
      String host = odbc.getOrDefault("server", "127.0.0.1");
      String port = odbc.getOrDefault("port", "5432");
      String name = odbc.get("database");
      if (name == null) {
        return null;
      }
      if (odbc.containsKey("uid")) {
        properties.setProperty("user", odbc.get("uid"));
      }
      if (odbc.containsKey("pwd")) {
        properties.setProperty("password", odbc.get("pwd"));
      }
      return "jdbc:postgresql://" + host + ":" + port + "/" + name;
    }
    return null;
  }

  private static Connection jdbcConnect(String url, Properties properties) throws Exception {
    Connection connection = DriverManager.getConnection(url, properties);
    for (String sql : setup) {
      try (Statement stmt = connection.createStatement()) {
        stmt.execute(sql);
      }
    }
    return connection;
  }

  /**
   * Read the SELECT row by row from a {@link ResultSet} and count the rows. No Arrow: this is the
   * plain-Java cost of the same read.
   */
  private static long jdbcFetch(Connection connection, String sql) throws Exception {
    try (Statement stmt = connection.createStatement();
        ResultSet rows = stmt.executeQuery(sql)) {
      stmt.setFetchSize(ROWSET);
      long n = 0;
      while (rows.next()) {
        // Touch every column, as the Arrow path must.
        rows.getInt(1);
        rows.getDouble(2);
        rows.getString(3);
        // getString, not getDate: sqlite-jdbc cannot parse the DATE column the
        // ADBC ingest created, and every driver can hand the cell over as text.
        rows.getString(4);
        n++;
      }
      return n;
    }
  }

  /**
   * Send {@code rows} rows to {@code ident} with a batched prepared statement and commit once.
   * Returns the seconds that took; the table must already exist.
   */
  private static double jdbcIngest(Connection connection, String ident, int rows) throws Exception {
    long start = System.nanoTime();
    connection.setAutoCommit(false);
    try (PreparedStatement stmt =
        connection.prepareStatement("INSERT INTO " + ident + " VALUES (?, ?, ?, ?)")) {
      for (int i = 0; i < rows; i++) {
        stmt.setInt(1, i);
        stmt.setDouble(2, i * 0.5);
        stmt.setString(3, text(i));
        stmt.setDate(4, java.sql.Date.valueOf(java.time.LocalDate.ofEpochDay(i % 20_000)));
        stmt.addBatch();
        if ((i + 1) % ROWSET == 0) {
          stmt.executeBatch();
        }
      }
      stmt.executeBatch();
    }
    connection.commit();
    return (System.nanoTime() - start) / 1e9;
  }

  // -------------------------------------------------------------- measurement

  /** One measurement: its median seconds, or why it did not finish. */
  private static final class Step {
    final double seconds;
    final String error;

    private Step(double seconds, String error) {
      this.seconds = seconds;
      this.error = error;
    }

    static Step ok(double seconds) {
      return new Step(seconds, null);
    }

    static Step failed(String message) {
      return new Step(0, trim(message));
    }

    Double rate(int rows) {
      return error == null && seconds > 0 ? rows / seconds : null;
    }
  }

  /** A step's body: returns the seconds it measured. */
  private interface Timing {
    double run() throws Exception;
  }

  /** A body that reports the row count it saw. */
  private interface Counting {
    long run() throws Exception;
  }

  /** Median of {@code reps} timings after one warmup. */
  private static double repeat(int reps, Timing once) throws Exception {
    once.run();
    double[] times = new double[reps];
    for (int i = 0; i < reps; i++) {
      times[i] = once.run();
    }
    Arrays.sort(times);
    return times[times.length / 2];
  }

  /** Time one call of {@code body}, which must report the row count it saw. */
  private static double timed(int expected, Counting body) throws Exception {
    long start = System.nanoTime();
    long got = body.run();
    double seconds = (System.nanoTime() - start) / 1e9;
    if (got != expected) {
      throw new IllegalStateException("read " + got + " rows, expected " + expected);
    }
    return seconds;
  }

  /** Keep a failure from ending the run: every step reports independently. */
  private static Step attempt(Timing body) {
    try {
      return Step.ok(body.run());
    } catch (Throwable error) {
      String message = error.getMessage();
      return Step.failed(message == null ? error.toString() : message);
    }
  }

  private static String trim(String message) {
    String line = message.split("\n", 2)[0].trim();
    return line.length() > 160 ? line.substring(0, 160) : line;
  }

  // ------------------------------------------------------------------- output

  /** 1234567.8 -> "1,234,568". */
  private static String thousands(double value) {
    String digits = String.format(Locale.ROOT, "%.0f", value);
    StringBuilder out = new StringBuilder();
    for (int i = 0; i < digits.length(); i++) {
      if (i > 0 && (digits.length() - i) % 3 == 0) {
        out.append(',');
      }
      out.append(digits.charAt(i));
    }
    return out.toString();
  }

  private static String cell(int rows, Step step) {
    Double rate = step.rate(rows);
    return rate == null ? "—" : thousands(rate);
  }

  private static String jsonEscape(String s) {
    StringBuilder out = new StringBuilder(s.length());
    for (char c : s.toCharArray()) {
      if (c == '"') {
        out.append("\\\"");
      } else if (c == '\\') {
        out.append("\\\\");
      } else if (c < 0x20) {
        out.append(' ');
      } else {
        out.append(c);
      }
    }
    return out.toString();
  }

  private static String jsonStep(String name, int rows, Step step) {
    if (step.error != null) {
      return String.format(
          Locale.ROOT, "\"%s\": {\"error\": \"%s\"}", name, jsonEscape(step.error));
    }
    return String.format(
        Locale.ROOT,
        "\"%s\": {\"secs\": %.6f, \"rate\": %.1f}",
        name,
        step.seconds,
        rows / step.seconds);
  }

  // --------------------------------------------------------------------- main

  private static int intValue(String[] args, int i) {
    if (i + 1 >= args.length) {
      die("missing value for an option");
    }
    return Integer.parseInt(args[i + 1]);
  }

  private static void die(String message) {
    System.err.println("bench_java: " + message);
    System.exit(2);
  }

  public static void main(String[] args) throws Exception {
    int rows = 10_000;
    int fetchRows = 100_000;
    int reps = 3;
    String db = null;
    for (int i = 0; i < args.length; i++) {
      switch (args[i]) {
        case "--rows":
          rows = intValue(args, i++);
          break;
        case "--fetch-rows":
          fetchRows = intValue(args, i++);
          break;
        case "--reps":
          reps = intValue(args, i++);
          break;
        case "-h":
        case "--help":
          System.out.println("bench_java [--rows N] [--fetch-rows N] [--reps N] <dbname>");
          return;
        default:
          if (args[i].startsWith("-")) {
            die("unknown option " + args[i]);
          }
          db = args[i];
      }
    }
    if (db == null) {
      die("expected a database name");
    }

    String prefix = db.toUpperCase(Locale.ROOT);
    driverPath = System.getenv("ADBC_ODBC_DRIVER");
    if (driverPath == null || driverPath.isEmpty()) {
      die("ADBC_ODBC_DRIVER must name libadbc_driver_odbc.so");
    }
    uri = System.getenv(prefix + "_CONN");
    if (uri == null || uri.isEmpty()) {
      die(prefix + "_CONN must hold the ODBC connection string");
    }

    // The bare table name handed to the ADBC ingest.
    String base = System.getenv("ADBC_BENCH_TABLE");
    String suffix = System.getenv("ADBC_MATRIX_SUFFIX");
    final String table =
        (base == null || base.isEmpty() ? "adbc_bench_java" : base) + (suffix == null ? "" : suffix);
    String setupEnv = System.getenv(prefix + "_SETUP");
    setup = new ArrayList<>();
    if (setupEnv != null) {
      for (String line : setupEnv.split("\n")) {
        if (!line.trim().isEmpty()) {
          ((List<String>) setup).add(line);
        }
      }
    }

    // Every spelling of that name later SQL might have to use. The bridge's ingest
    // quotes the table name with the database's own quote character, so the quoted
    // spelling is what usually answers; a case-folding database (Oracle, Db2) can
    // also answer to the upper-cased one, which conn.py passes in <DB>_TABLE from
    // the compat matrix's `ident` hook.
    List<String> candidates = new ArrayList<>();
    for (String candidate :
        new String[] {
          "\"" + table + "\"",
          System.getenv(prefix + "_TABLE") == null ? table : System.getenv(prefix + "_TABLE"),
          table
        }) {
      if (!candidates.contains(candidate)) {
        candidates.add(candidate);
      }
    }

    // Ingest a single row and find out which spelling of the table and column names
    // reaches what that produced, so nothing downstream has to guess.
    final String[] resolved = {candidates.get(0), "SELECT id, val, txt, dt FROM " + candidates.get(0)};
    Step probe =
        attempt(
            () -> {
              try (Session session = new Session(false)) {
                dropTable(session, candidates, false);
                try (VectorSchemaRoot one = makeRoot(session.allocator, 1)) {
                  adbcIngest(session, table, one);
                }
                String found = null;
                for (String candidate : candidates) {
                  try {
                    adbcCount(session, candidate);
                    found = candidate;
                    break;
                  } catch (Exception ignored) {
                    // Try the next spelling.
                  }
                }
                if (found == null) {
                  throw new IllegalStateException(
                      "ingested " + table + " but no spelling of the name selects from it");
                }
                resolved[0] = found;
                for (String sql :
                    new String[] {
                      "SELECT \"id\", \"val\", \"txt\", \"dt\" FROM " + found,
                      "SELECT id, val, txt, dt FROM " + found
                    }) {
                  try {
                    adbcFetch(session, sql);
                    resolved[1] = sql;
                    return 0;
                  } catch (Exception ignored) {
                    // Try the next spelling.
                  }
                }
                throw new IllegalStateException(
                    "the benchmark table has no readable id/val/txt/dt columns");
              }
            });
    if (probe.error != null) {
      System.err.println(db + ": " + probe.error);
    }
    final String ident = resolved[0];
    final String select = resolved[1];

    final int ingestRows = rows;
    final int readRows = fetchRows;
    final int repetitions = reps;

    // 1. adbc ingest: drop, ingest --rows rows, commit, verify the count.
    Step adbcIngestStep =
        attempt(
            () -> {
              try (Session session = new Session(false)) {
                double seconds =
                    repeat(
                        repetitions,
                        () -> {
                          dropTable(session, candidates, false);
                          try (VectorSchemaRoot root = makeRoot(session.allocator, ingestRows)) {
                            return adbcIngest(session, table, root);
                          }
                        });
                verify(session, ident, ingestRows);
                return seconds;
              }
            });

    // 2. JDBC ingest of the same rows into the table ADBC's DDL created.
    Properties jdbcProperties = new Properties();
    String url = jdbcUrl(db, prefix, jdbcProperties);
    Step jdbcIngestStep =
        url == null
            ? Step.failed("no JDBC URL for " + db + "; set " + prefix + "_JDBC")
            : attempt(
                () -> {
                  try (Session session = new Session(false);
                      Connection jdbc = jdbcConnect(url, jdbcProperties)) {
                    double seconds =
                        repeat(
                            repetitions,
                            () -> {
                              dropTable(session, candidates, false);
                              try (VectorSchemaRoot empty = makeRoot(session.allocator, 0)) {
                                adbcIngest(session, table, empty);
                              }
                              return jdbcIngest(jdbc, ident, ingestRows);
                            });
                    verify(session, ident, ingestRows);
                    return seconds;
                  }
                });

    // Load the bigger table the two fetch steps read back.
    Step loaded =
        attempt(
            () -> {
              try (Session session = new Session(false)) {
                dropTable(session, candidates, false);
                try (VectorSchemaRoot root = makeRoot(session.allocator, readRows)) {
                  adbcIngest(session, table, root);
                }
                verify(session, ident, readRows);
                return 0;
              }
            });

    Step adbcFetchStep = loaded;
    Step jdbcFetchStep = loaded;
    if (loaded.error == null) {
      adbcFetchStep =
          attempt(
              () -> {
                try (Session session = new Session(true)) {
                  return repeat(
                      repetitions, () -> timed(readRows, () -> adbcFetch(session, select)));
                }
              });
      jdbcFetchStep =
          url == null
              ? Step.failed("no JDBC URL for " + db + "; set " + prefix + "_JDBC")
              : attempt(
                  () -> {
                    try (Connection jdbc = jdbcConnect(url, jdbcProperties)) {
                      return repeat(
                          repetitions, () -> timed(readRows, () -> jdbcFetch(jdbc, select)));
                    }
                  });
    }

    // Leave nothing of ours behind on a shared server.
    try (Session session = new Session(true)) {
      dropTable(session, candidates, true);
    } catch (Exception ignored) {
      // Best effort.
    }

    System.out.println(
        String.format(
            Locale.ROOT,
            "{\"lang\": \"java\", \"db\": \"%s\", \"rows\": %d, \"fetch_rows\": %d, "
                + "\"reps\": %d, %s, %s, %s, %s}",
            jsonEscape(db),
            rows,
            fetchRows,
            reps,
            jsonStep("adbc_ingest", rows, adbcIngestStep),
            jsonStep("adbc_fetch", fetchRows, adbcFetchStep),
            jsonStep("jdbc_ingest", rows, jdbcIngestStep),
            jsonStep("jdbc_fetch", fetchRows, jdbcFetchStep)));
    System.out.println(
        String.format(
            Locale.ROOT,
            "| java | %s | %s | %s | %s | %s |",
            db,
            cell(rows, adbcIngestStep),
            cell(fetchRows, adbcFetchStep),
            cell(rows, jdbcIngestStep),
            cell(fetchRows, jdbcFetchStep)));

    for (Object[] step :
        new Object[][] {
          {"adbc ingest", adbcIngestStep},
          {"adbc fetch", adbcFetchStep},
          {"jdbc ingest", jdbcIngestStep},
          {"jdbc fetch", jdbcFetchStep}
        }) {
      Step s = (Step) step[1];
      if (s.error != null) {
        System.err.println(db + ": " + step[0] + ": " + s.error);
      }
    }
    System.exit(0);
  }
}
