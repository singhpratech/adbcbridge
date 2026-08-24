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

package org.adbcbridge;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Collections;
import org.apache.arrow.adbc.core.AdbcConnection;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcStatement;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.FieldVector;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/**
 * Loads the driver, connects to SQLite through its ODBC driver, runs {@code SELECT 1} and reads
 * the Arrow batch back.
 *
 * <p>Needs {@code SQLITE_ODBC_DRIVER}: the SQLite ODBC driver to bridge to, either an absolute
 * path to {@code libsqlite3odbc.so} or a name registered in {@code odbcinst.ini}. Skipped when it
 * is unset. The adbcbridge driver itself is found by {@link AdbcBridge#driverPath()}, so {@code
 * ADBC_ODBC_DRIVER} (or {@code ADBCBRIDGE_LIBRARY}) is the usual way to point the test at a
 * fresh build.
 */
class AdbcBridgeTest {
  @TempDir Path tempDir;

  private static String sqliteOdbcDriver() {
    String value = System.getenv("SQLITE_ODBC_DRIVER");
    assumeTrue(value != null && !value.isEmpty(), "SQLITE_ODBC_DRIVER is not set");
    return value;
  }

  @Test
  void selectOneThroughSqlite() throws Exception {
    String sqlite = sqliteOdbcDriver();

    // 1. Locate the driver: an absolute path to a real file, and the same answer every time.
    String path = AdbcBridge.driverPath();
    assertTrue(Paths.get(path).isAbsolute(), "driverPath() is absolute: " + path);
    assertTrue(Files.isRegularFile(Paths.get(path)), "driverPath() is a file: " + path);
    assertEquals(path, AdbcBridge.driverPath(), "driverPath() is cached");

    // 2. Connect to a fresh SQLite database file through the SQLite ODBC driver.
    // The connection string is spelled the way the rest of the repository spells it.
    String uri = "Driver=" + sqlite + ";Database=" + tempDir.resolve("test.db") + ";";
    try (BufferAllocator allocator = new RootAllocator()) {
      assertNotNull(AdbcBridge.driver(allocator));
      try (AdbcDatabase database =
              AdbcBridge.open(
                  allocator, uri, Collections.singletonMap("adbc.odbc.delegate", "never"));
          AdbcConnection connection = database.connect();
          AdbcStatement statement = connection.createStatement()) {
        // 3. SELECT 1 and read the Arrow batch.
        statement.setSqlQuery("SELECT 1 AS one");
        try (AdbcStatement.QueryResult result = statement.executeQuery()) {
          ArrowReader reader = result.getReader();
          assertTrue(reader.loadNextBatch(), "one batch");
          VectorSchemaRoot batch = reader.getVectorSchemaRoot();
          assertEquals(1, batch.getRowCount(), "one row");
          assertEquals(1, batch.getFieldVectors().size(), "one column");
          assertEquals("one", batch.getSchema().getFields().get(0).getName());
          FieldVector column = batch.getVector(0);
          Object value = column.getObject(0);
          assertTrue(value instanceof Number, "numeric value, got " + value);
          assertEquals(1L, ((Number) value).longValue());
          assertFalse(reader.loadNextBatch(), "no second batch");
        }
      }
    }
  }
}
