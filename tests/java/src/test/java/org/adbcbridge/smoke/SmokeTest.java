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

package org.adbcbridge.smoke;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.apache.arrow.adbc.core.AdbcConnection;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcDriver;
import org.apache.arrow.adbc.core.AdbcException;
import org.apache.arrow.adbc.core.AdbcStatement;
import org.apache.arrow.adbc.driver.jni.JniDriver;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.BigIntVector;
import org.apache.arrow.vector.VarCharVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;
import org.apache.arrow.vector.types.Types;
import org.apache.arrow.vector.types.pojo.Field;
import org.apache.arrow.vector.types.pojo.Schema;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/**
 * Smoke test for {@code libadbc_driver_odbc.so} driven from Java.
 *
 * <p>The driver is a plain C shared library, so the Java ADBC driver manager reaches it the same
 * way the Python, Go and Rust ones do: {@code adbc-driver-jni} wraps the native ADBC driver
 * manager, which {@code dlopen}s the {@code .so} named by {@code jni.driver} and calls {@code
 * AdbcDriverInit}. The {@code uri} option is passed straight through to the driver, which uses it
 * as the ODBC connection string.
 *
 * <p>Two environment variables steer the test:
 *
 * <ul>
 *   <li>{@code ADBC_ODBC_DRIVER} — path to the driver under test (default: {@code
 *       ../../build/libadbc_driver_odbc.so} relative to this project).
 *   <li>{@code SQLITE_ODBC_DRIVER} — the SQLite ODBC driver to bridge to, either an absolute path
 *       to {@code libsqlite3odbc.so} or a name registered in {@code odbcinst.ini} (default: {@code
 *       SQLite3}).
 * </ul>
 *
 * <p>No server and no DSN are needed: every test gets its own SQLite database file.
 */
class SmokeTest {
  /** The driver under test. */
  private static String driverPath() {
    String fromEnv = System.getenv("ADBC_ODBC_DRIVER");
    if (fromEnv != null && !fromEnv.isEmpty()) {
      return fromEnv;
    }
    return Path.of("..", "..", "build", "libadbc_driver_odbc.so").toAbsolutePath().normalize()
        .toString();
  }

  /** The ODBC driver adbcbridge bridges to. A bare name is looked up in {@code odbcinst.ini}. */
  private static String sqliteOdbcDriver() {
    String fromEnv = System.getenv("SQLITE_ODBC_DRIVER");
    return (fromEnv == null || fromEnv.isEmpty()) ? "SQLite3" : fromEnv;
  }

  @TempDir Path tempDir;

  private BufferAllocator allocator;
  private AdbcDatabase database;
  private AdbcConnection connection;

  @BeforeEach
  void connect() throws Exception {
    allocator = new RootAllocator();

    Map<String, Object> parameters = new HashMap<>();
    JniDriver.PARAM_DRIVER.set(parameters, driverPath());
    AdbcDriver.PARAM_URI.set(
        parameters,
        "Driver=" + sqliteOdbcDriver() + ";Database=" + tempDir.resolve("smoke.db") + ";");

    database = new JniDriver(allocator).open(parameters);
    connection = database.connect();
  }

  @AfterEach
  void disconnect() throws Exception {
    // Close in reverse order of creation; the allocator complains if anything leaked.
    if (connection != null) {
      connection.close();
    }
    if (database != null) {
      database.close();
    }
    if (allocator != null) {
      allocator.close();
    }
  }

  @Test
  void createInsertSelect() throws Exception {
    assertEquals(0, update("CREATE TABLE t (id INTEGER, name TEXT)"));
    assertEquals(1, update("INSERT INTO t VALUES (1, 'ada')"));
    // A NULL, and a string that is multi-byte in UTF-8 and non-BMP in UTF-16.
    assertEquals(1, update("INSERT INTO t VALUES (2, NULL)"));
    assertEquals(1, update("INSERT INTO t VALUES (3, 'héllo 🎉')"));

    try (AdbcStatement stmt = connection.createStatement()) {
      stmt.setSqlQuery("SELECT id, name FROM t ORDER BY id");
      try (AdbcStatement.QueryResult result = stmt.executeQuery()) {
        ArrowReader reader = result.getReader();

        List<Object> ids = new ArrayList<>();
        List<Object> names = new ArrayList<>();
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          assertEquals(
              Arrays.asList("id", "name"),
              root.getSchema().getFields().stream().map(Field::getName).toList());
          for (int row = 0; row < root.getRowCount(); row++) {
            ids.add(root.getVector("id").getObject(row));
            Object name = root.getVector("name").getObject(row);
            // VarCharVector hands back org.apache.arrow.vector.util.Text, not String.
            names.add(name == null ? null : name.toString());
          }
        }

        assertEquals(3, ids.size(), "three rows");
        assertEquals(1L, ((Number) ids.get(0)).longValue());
        assertEquals(2L, ((Number) ids.get(1)).longValue());
        assertEquals(3L, ((Number) ids.get(2)).longValue());

        assertEquals("ada", names.get(0));
        assertNull(names.get(1), "SQL NULL stays null through the bridge");
        assertEquals("héllo 🎉", names.get(2), "UTF-8 round-trips, emoji included");
        // Sanity check that the expected string really is the code points we think it is.
        assertEquals(
            "68c3a96c6c6f20f09f8e89",
            hex("héllo 🎉".getBytes(StandardCharsets.UTF_8)),
            "test string is 'héllo ' + U+1F389 PARTY POPPER");
      }
    }
  }

  @Test
  void parameterisedInsertAndSelect() throws Exception {
    assertEquals(0, update("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT)"));

    Schema paramSchema =
        new Schema(
            Arrays.asList(
                Field.nullable("id", Types.MinorType.BIGINT.getType()),
                Field.nullable("name", Types.MinorType.VARCHAR.getType())));

    // One bound batch, three rows, one execution: (1, 'ada'), (2, 'grace'), (3, NULL).
    try (AdbcStatement stmt = connection.createStatement();
        VectorSchemaRoot params = VectorSchemaRoot.create(paramSchema, allocator)) {
      BigIntVector ids = (BigIntVector) params.getVector("id");
      VarCharVector names = (VarCharVector) params.getVector("name");
      ids.setSafe(0, 1);
      ids.setSafe(1, 2);
      ids.setSafe(2, 3);
      names.setSafe(0, "ada".getBytes(StandardCharsets.UTF_8));
      names.setSafe(1, "grace".getBytes(StandardCharsets.UTF_8));
      names.setNull(2);
      params.setRowCount(3);

      stmt.setSqlQuery("INSERT INTO people (id, name) VALUES (?, ?)");
      stmt.prepare();
      stmt.bind(params);
      assertEquals(3, stmt.executeUpdate().getAffectedRows(), "three rows inserted");
    }

    // Parameterised SELECT: bind a single-row batch as the WHERE value.
    Schema argSchema =
        new Schema(List.of(Field.nullable("id", Types.MinorType.BIGINT.getType())));
    try (AdbcStatement stmt = connection.createStatement();
        VectorSchemaRoot arg = VectorSchemaRoot.create(argSchema, allocator)) {
      ((BigIntVector) arg.getVector("id")).setSafe(0, 2);
      arg.setRowCount(1);

      stmt.setSqlQuery("SELECT name FROM people WHERE id = ?");
      stmt.prepare();
      stmt.bind(arg);

      try (AdbcStatement.QueryResult result = stmt.executeQuery()) {
        ArrowReader reader = result.getReader();
        List<Object> names = new ArrayList<>();
        while (reader.loadNextBatch()) {
          VectorSchemaRoot root = reader.getVectorSchemaRoot();
          for (int row = 0; row < root.getRowCount(); row++) {
            Object name = root.getVector("name").getObject(row);
            names.add(name == null ? null : name.toString());
          }
        }
        assertEquals(List.of("grace"), names, "only the row with id = 2");
      }
    }
  }

  @Test
  void errorCarriesAMessage() throws Exception {
    try (AdbcStatement stmt = connection.createStatement()) {
      stmt.setSqlQuery("SELECT * FROM no_such_table");
      AdbcException error = assertThrows(AdbcException.class, stmt::executeQuery);
      assertTrue(
          error.getMessage().toLowerCase().contains("no_such_table"),
          "the ODBC diagnostic should name the missing table, got: " + error.getMessage());
    }
  }

  /** Run {@code sql} as DML and return the reported number of affected rows. */
  private long update(String sql) throws Exception {
    try (AdbcStatement stmt = connection.createStatement()) {
      stmt.setSqlQuery(sql);
      return stmt.executeUpdate().getAffectedRows();
    }
  }

  private static String hex(byte[] bytes) {
    StringBuilder sb = new StringBuilder(bytes.length * 2);
    for (byte b : bytes) {
      sb.append(String.format("%02x", b));
    }
    return sb.toString();
  }
}
