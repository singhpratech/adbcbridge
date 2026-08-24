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

/**
 * Arrow ADBC over any ODBC driver, for Java.
 *
 * <p>{@link org.adbcbridge.AdbcBridge} finds the native {@code libadbc_driver_odbc} library
 * (bundled in the jar, named in the environment, installed on the machine, or in a CMake build
 * tree) and hands it to the ADBC driver manager, so any database with an ODBC driver answers
 * queries as Arrow batches.
 */
package org.adbcbridge;
