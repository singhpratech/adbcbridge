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

import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import org.apache.arrow.adbc.core.AdbcDatabase;
import org.apache.arrow.adbc.core.AdbcDriver;
import org.apache.arrow.adbc.core.AdbcException;
import org.apache.arrow.adbc.driver.jni.JniDriver;
import org.apache.arrow.memory.BufferAllocator;

/**
 * Entry point: Arrow ADBC over any ODBC driver.
 *
 * <p>Three calls:
 *
 * <ul>
 *   <li>{@link #driverPath()} finds {@code libadbc_driver_odbc} (see {@link DriverLocator} for the
 *       order it looks in);
 *   <li>{@link #driver(BufferAllocator)} wraps it in an {@link AdbcDriver} backed by the ADBC
 *       driver manager ({@link JniDriver});
 *   <li>{@link #open(BufferAllocator, String, Map)} opens an {@link AdbcDatabase} on an ODBC
 *       connection string.
 * </ul>
 *
 * <pre>{@code
 * try (BufferAllocator allocator = new RootAllocator();
 *     AdbcDatabase database = AdbcBridge.open(allocator, "DSN=warehouse;UID=me;PWD=secret;", null);
 *     AdbcConnection connection = database.connect();
 *     AdbcStatement statement = connection.createStatement()) {
 *   statement.setSqlQuery("SELECT * FROM t");
 *   try (AdbcStatement.QueryResult result = statement.executeQuery()) {
 *     ArrowReader reader = result.getReader();
 *     while (reader.loadNextBatch()) {
 *       VectorSchemaRoot batch = reader.getVectorSchemaRoot();
 *       // ...
 *     }
 *   }
 * }
 * }</pre>
 *
 * <p>The ODBC connection string goes in {@link AdbcDriver#PARAM_URI} ({@code uri}); any other
 * entry of the options map is passed to the driver as a database option, for example {@code
 * adbc.odbc.delegate}.
 *
 * <p>On JDK 17 and later the Arrow allocator needs {@code
 * --add-opens=java.base/java.nio=ALL-UNNAMED} on the JVM command line.
 */
public final class AdbcBridge {
  /** The name this driver is registered under in ADBC driver manifests ({@code odbc.toml}). */
  public static final String DRIVER_NAME = "odbc";

  /** Environment variable naming the shared library outright; checked first. */
  public static final String ENV_LIBRARY = DriverLocator.ENV_LIBRARY;

  /** The environment variable the rest of the adbcbridge repository uses; checked second. */
  public static final String ENV_DRIVER = DriverLocator.ENV_DRIVER;

  /** System property equivalent of {@link #ENV_LIBRARY}, checked before it. */
  public static final String PROPERTY_LIBRARY = DriverLocator.PROPERTY;

  private AdbcBridge() {}

  /**
   * The absolute path of the adbcbridge shared library ({@code libadbc_driver_odbc.so}, {@code
   * .dylib} or {@code .dll}).
   *
   * <p>Looked up in this order, and cached for the life of the JVM:
   *
   * <ol>
   *   <li>the {@code adbcbridge.library} system property;
   *   <li>the {@code ADBCBRIDGE_LIBRARY} environment variable;
   *   <li>the {@code ADBC_ODBC_DRIVER} environment variable;
   *   <li>a copy bundled in this jar under {@code /org/adbcbridge/native/<os>-<arch>/}, extracted
   *       once to a temporary directory;
   *   <li>the ADBC driver manifest named {@code odbc} ({@code odbc.toml}) in the directories the
   *       ADBC driver manager searches;
   *   <li>common install locations ({@code /usr/local/lib}, {@code /usr/lib}, ...) and a CMake
   *       {@code build/} tree next to a source checkout.
   * </ol>
   *
   * An explicit setting (property or environment variable) that does not point at a file is an
   * error rather than a miss.
   *
   * @return the absolute path of the shared library
   * @throws DriverNotFoundException if none of those has it; the message lists every place
   *     searched
   */
  public static String driverPath() {
    return DriverLocator.locate();
  }

  /**
   * An {@link AdbcDriver} backed by the ADBC driver manager and wired to {@link #driverPath()}.
   *
   * <p>Its {@link AdbcDriver#open(Map)} needs {@link AdbcDriver#PARAM_URI} set to the ODBC
   * connection string; everything else is optional. The driver library is found now, so a
   * missing driver fails here rather than at the first {@code open}.
   *
   * @param allocator the allocator every database, connection and result of this driver uses
   * @return the driver
   * @throws DriverNotFoundException if the shared library cannot be found
   */
  public static AdbcDriver driver(BufferAllocator allocator) {
    Objects.requireNonNull(allocator, "allocator");
    return new BridgeDriver(allocator, driverPath());
  }

  /**
   * Open a database on an ODBC connection string.
   *
   * @param allocator the allocator the database and everything opened from it uses
   * @param connectionString an ODBC connection string, {@code "DSN=..."} or {@code
   *     "Driver=...;Server=...;"}; becomes {@link AdbcDriver#PARAM_URI}
   * @param options extra database options, or {@code null}; keys are ADBC option names such as
   *     {@code adbc.odbc.delegate}, values are usually strings. A {@code uri} entry here is
   *     overridden by {@code connectionString}.
   * @return the open database; close it when done
   * @throws AdbcException if the driver manager or the driver refuses the parameters
   * @throws DriverNotFoundException if the shared library cannot be found
   */
  public static AdbcDatabase open(
      BufferAllocator allocator, String connectionString, Map<String, Object> options)
      throws AdbcException {
    Objects.requireNonNull(connectionString, "connectionString");
    Map<String, Object> parameters = new HashMap<>();
    if (options != null) {
      parameters.putAll(options);
    }
    AdbcDriver.PARAM_URI.set(parameters, connectionString);
    return driver(allocator).open(parameters);
  }

  /** {@link JniDriver} with the driver library filled in. */
  private static final class BridgeDriver implements AdbcDriver {
    private final JniDriver delegate;
    private final String path;

    BridgeDriver(BufferAllocator allocator, String path) {
      this.delegate = new JniDriver(allocator);
      this.path = path;
    }

    @Override
    public AdbcDatabase open(Map<String, Object> parameters) throws AdbcException {
      Map<String, Object> withDriver = new HashMap<>();
      if (parameters != null) {
        withDriver.putAll(parameters);
      }
      // A caller who names another library deliberately gets it; otherwise ours.
      if (!withDriver.containsKey(JniDriver.PARAM_DRIVER.getKey())) {
        JniDriver.PARAM_DRIVER.set(withDriver, path);
      }
      return delegate.open(withDriver);
    }

    @Override
    public String toString() {
      return "AdbcBridge.driver(" + path + ")";
    }
  }
}
