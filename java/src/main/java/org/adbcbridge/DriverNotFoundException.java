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

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Thrown when {@code libadbc_driver_odbc} cannot be found anywhere {@link AdbcBridge#driverPath()}
 * looks.
 *
 * <p>The message lists every place that was searched, in order, so the fix is usually obvious from
 * reading it: set {@code ADBCBRIDGE_LIBRARY} (or {@code ADBC_ODBC_DRIVER}) to the shared library,
 * install the driver, or use a jar that bundles it for this platform.
 */
public class DriverNotFoundException extends RuntimeException {
  private static final long serialVersionUID = 1L;

  private final List<String> searched;

  /**
   * Create the exception.
   *
   * @param message what went wrong, without the list of searched places (that is appended)
   * @param searched the places that were searched, in order; each entry is one line
   */
  public DriverNotFoundException(String message, List<String> searched) {
    super(describe(message, searched));
    this.searched = Collections.unmodifiableList(new ArrayList<>(searched));
  }

  /**
   * The places that were searched, in order.
   *
   * @return one human-readable entry per place searched; never {@code null}
   */
  public List<String> getSearched() {
    return searched;
  }

  private static String describe(String message, List<String> searched) {
    StringBuilder sb = new StringBuilder(message);
    if (!searched.isEmpty()) {
      sb.append(System.lineSeparator()).append("Searched, in order:");
      for (String place : searched) {
        sb.append(System.lineSeparator()).append("  - ").append(place);
      }
    }
    sb.append(System.lineSeparator())
        .append("Set ")
        .append(DriverLocator.ENV_LIBRARY)
        .append("=/path/to/")
        .append(DriverLocator.libraryNames().get(0))
        .append(", install the driver (cmake --install build), use a jar that bundles it, ")
        .append("or build it with cmake --build build in a source checkout.");
    return sb.toString();
  }
}
