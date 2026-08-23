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

import java.util.HashMap;
import java.util.Map;
import org.apache.arrow.adbc.core.AdbcConnection;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcDriver;
import org.apache.arrow.adbc.core.AdbcStatement;
import org.apache.arrow.adbc.driver.jni.JniDriver;
import org.apache.arrow.memory.BufferAllocator;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;
import org.apache.arrow.vector.ipc.ArrowReader;

/**
 * The "Use from Java" snippet from the top-level {@code README.md}.
 *
 * <p>Never executed — the paths in it are placeholders — but Maven compiles it, so the snippet
 * cannot silently rot. {@link SmokeTest} runs the same sequence against a real database.
 */
final class ReadmeSnippet {
  private ReadmeSnippet() {}

  static void snippet() throws Exception {
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
  }
}
