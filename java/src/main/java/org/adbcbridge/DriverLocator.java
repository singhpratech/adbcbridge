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

import java.io.IOException;
import java.io.InputStream;
import java.net.URISyntaxException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.CodeSource;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Finds {@code libadbc_driver_odbc.so} / {@code .dylib} / {@code .dll}.
 *
 * <p>The same order as the Python package's {@code adbcbridge._locate.driver_path()}, with a
 * library-specific override in front and the jar's own copy in the place of the wheel's:
 *
 * <ol>
 *   <li>the {@code adbcbridge.library} system property;
 *   <li>the {@code ADBCBRIDGE_LIBRARY} environment variable;
 *   <li>the {@code ADBC_ODBC_DRIVER} environment variable (what the rest of the repository uses);
 *   <li>a copy packaged in this jar under {@code /org/adbcbridge/native/<os>-<arch>/}, extracted
 *       once per JVM into a temporary directory;
 *   <li>the ADBC driver manifest named {@code odbc} ({@code odbc.toml}) in the directories the
 *       ADBC driver manager searches;
 *   <li>common install locations ({@code /usr/local/lib}, {@code /usr/lib}, {@code
 *       /opt/adbcbridge/lib}, Homebrew, a Python virtualenv or conda prefix, ...);
 *   <li>a CMake {@code build/} tree next to a source checkout, found by walking up from where
 *       this class was loaded and from the working directory.
 * </ol>
 */
final class DriverLocator {
  /** System property checked first. */
  static final String PROPERTY = "adbcbridge.library";

  /** Environment variable checked before {@link #ENV_DRIVER}. */
  static final String ENV_LIBRARY = "ADBCBRIDGE_LIBRARY";

  /** The environment variable the rest of the adbcbridge repository uses. */
  static final String ENV_DRIVER = "ADBC_ODBC_DRIVER";

  /** Where a bundled driver lives inside the jar, followed by {@code <os>-<arch>/<file>}. */
  static final String RESOURCE_ROOT = "/org/adbcbridge/native/";

  /** Name of the ADBC driver manifest installed by {@code cmake --install}. */
  static final String MANIFEST_NAME = "odbc.toml";

  private static final Object LOCK = new Object();
  private static volatile String cached;

  private DriverLocator() {}

  /** One search: the result, or {@code null}, plus everything that was looked at. */
  static final class Search {
    final List<String> searched = new ArrayList<>();
    Path found;
  }

  // ------------------------------------------------------------------ platform

  static String osName() {
    String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
    if (os.contains("win")) {
      return "windows";
    }
    if (os.contains("mac") || os.contains("darwin")) {
      return "macos";
    }
    if (os.contains("linux")) {
      return "linux";
    }
    if (os.contains("freebsd")) {
      return "freebsd";
    }
    if (os.contains("openbsd")) {
      return "openbsd";
    }
    return os.replaceAll("[^a-z0-9]", "");
  }

  /** The uname-style architecture: {@code x86_64}, {@code aarch64}, ... */
  static String machine() {
    String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
    switch (arch) {
      case "amd64":
      case "x86_64":
      case "x64":
        return "x86_64";
      case "aarch64":
      case "arm64":
        return "aarch64";
      case "x86":
      case "i386":
      case "i686":
        return "x86";
      case "ppc64le":
        return "ppc64le";
      default:
        return arch;
    }
  }

  /** The architecture name ADBC manifests use: {@code amd64}, {@code arm64}, ... */
  private static String adbcArch(String machine) {
    switch (machine) {
      case "x86_64":
        return "amd64";
      case "aarch64":
        return "arm64";
      case "ppc64le":
        return "powerpc64le";
      default:
        return machine;
    }
  }

  static boolean isWindows() {
    return "windows".equals(osName());
  }

  static boolean isMac() {
    return "macos".equals(osName());
  }

  /** File names the driver can have on this platform, most usual first. */
  static List<String> libraryNames() {
    if (isWindows()) {
      return Arrays.asList("adbc_driver_odbc.dll", "libadbc_driver_odbc.dll");
    }
    if (isMac()) {
      return Collections.singletonList("libadbc_driver_odbc.dylib");
    }
    return Collections.singletonList("libadbc_driver_odbc.so");
  }

  /** The {@code <os>-<arch>} directory a bundled driver sits in: {@code linux-x86_64}, ... */
  static String bundleDirectory() {
    return osName() + "-" + machine();
  }

  /**
   * Manifest keys accepted for this machine, most canonical first. The manifest CMake installs
   * uses the ADBC spelling ({@code linux_amd64}); a hand-written one may use the uname spelling
   * ({@code linux_x86_64}), so both are accepted. The OS half always has to match.
   */
  static List<String> manifestKeys() {
    String os = osName();
    String machine = machine();
    Set<String> keys = new LinkedHashSet<>();
    keys.add(os + "_" + adbcArch(machine));
    keys.add(os + "_" + machine);
    if ("x86_64".equals(machine)) {
      keys.add(os + "_amd64");
      keys.add(os + "_x64");
    } else if ("aarch64".equals(machine)) {
      keys.add(os + "_arm64");
    }
    return new ArrayList<>(keys);
  }

  // ------------------------------------------------------------------ entry point

  /** The driver's absolute path, searching once per JVM and caching the answer. */
  static String locate() {
    String path = cached;
    if (path != null) {
      return path;
    }
    synchronized (LOCK) {
      if (cached == null) {
        Search search = search();
        if (search.found == null) {
          throw new DriverNotFoundException(
              "could not find " + libraryNames().get(0) + " (the adbcbridge ADBC driver).",
              search.searched);
        }
        cached = search.found.toAbsolutePath().toString();
      }
      return cached;
    }
  }

  /** Run the whole lookup without caching. Package-private so tests can see what was searched. */
  static Search search() {
    Search s = new Search();
    if (fromProperty(s)
        || fromEnv(s, ENV_LIBRARY)
        || fromEnv(s, ENV_DRIVER)
        || fromBundle(s)
        || fromManifest(s)
        || fromInstallDirs(s)
        || fromBuildTrees(s)) {
      return s;
    }
    return s;
  }

  // ------------------------------------------------------------------ 1-3. overrides

  private static boolean fromProperty(Search s) {
    String value = System.getProperty(PROPERTY);
    if (value == null || value.isEmpty()) {
      s.searched.add("system property " + PROPERTY + " (not set)");
      return false;
    }
    return checkOverride(s, "system property " + PROPERTY, value);
  }

  private static boolean fromEnv(Search s, String name) {
    String value = System.getenv(name);
    if (value == null || value.isEmpty()) {
      s.searched.add("environment variable " + name + " (not set)");
      return false;
    }
    return checkOverride(s, "environment variable " + name, value);
  }

  /** An explicit setting that does not point at a file is an error, not a miss. */
  private static boolean checkOverride(Search s, String what, String value) {
    Path path = expandHome(value);
    if (Files.isRegularFile(path)) {
      s.searched.add(what + " = " + value);
      s.found = path;
      return true;
    }
    s.searched.add(what + " = " + value + " (not a file)");
    throw new DriverNotFoundException(what + "=" + value + " does not point at a file.", s.searched);
  }

  private static Path expandHome(String value) {
    if (value.startsWith("~" + java.io.File.separator) || value.startsWith("~/")) {
      String home = System.getProperty("user.home");
      if (home != null && !home.isEmpty()) {
        return Paths.get(home, value.substring(2));
      }
    }
    return Paths.get(value);
  }

  // ------------------------------------------------------------------ 4. bundled in the jar

  private static boolean fromBundle(Search s) {
    String dir = RESOURCE_ROOT + bundleDirectory() + "/";
    for (String name : libraryNames()) {
      String resource = dir + name;
      URL url = DriverLocator.class.getResource(resource);
      if (url == null) {
        s.searched.add("bundled resource " + resource + " (not in the jar)");
        continue;
      }
      try {
        Path extracted = extract(resource, name);
        s.searched.add("bundled resource " + resource + " (extracted to " + extracted + ")");
        s.found = extracted;
        return true;
      } catch (IOException e) {
        s.searched.add("bundled resource " + resource + " (could not extract: " + e + ")");
      }
    }
    return false;
  }

  /**
   * Copy a bundled driver into a fresh temporary directory. The file keeps its own name so the
   * dynamic loader reports something recognisable, and both are deleted when the JVM exits
   * (best effort: Windows will not let a loaded DLL go).
   */
  private static Path extract(String resource, String name) throws IOException {
    Path dir = Files.createTempDirectory("adbcbridge-");
    Path file = dir.resolve(name);
    // deleteOnExit runs in reverse registration order: register the directory first.
    dir.toFile().deleteOnExit();
    file.toFile().deleteOnExit();
    try (InputStream in = DriverLocator.class.getResourceAsStream(resource)) {
      if (in == null) {
        throw new IOException("resource vanished: " + resource);
      }
      Files.copy(in, file, StandardCopyOption.REPLACE_EXISTING);
    }
    if (!isWindows()) {
      try {
        Files.setPosixFilePermissions(file, PosixFilePermissions.fromString("rwxr-xr-x"));
      } catch (UnsupportedOperationException ignored) {
        // Not a POSIX file system; the loader does not need the bit there.
      }
    }
    return file.toAbsolutePath();
  }

  // ------------------------------------------------------------------ 5. ADBC driver manifest

  /** Directories the ADBC driver manager searches for {@code odbc.toml}. */
  static List<Path> manifestDirs() {
    List<Path> dirs = new ArrayList<>();
    String env = System.getenv("ADBC_DRIVER_PATH");
    if (env != null && !env.isEmpty()) {
      for (String p : env.split(java.io.File.pathSeparator)) {
        if (!p.isEmpty()) {
          dirs.add(Paths.get(p));
        }
      }
    }
    for (Path prefix : pythonPrefixes()) {
      dirs.add(prefix.resolve("etc").resolve("adbc").resolve("drivers"));
      dirs.add(prefix.resolve("share").resolve("adbc").resolve("drivers"));
    }
    String home = System.getProperty("user.home", "");
    if (isMac()) {
      if (!home.isEmpty()) {
        dirs.add(Paths.get(home, "Library", "Application Support", "ADBC", "Drivers"));
      }
      dirs.add(Paths.get("/Library/Application Support/ADBC/Drivers"));
    } else if (isWindows()) {
      String local = System.getenv("LOCALAPPDATA");
      if (local != null && !local.isEmpty()) {
        dirs.add(Paths.get(local, "ADBC", "Drivers"));
      }
      String programData = System.getenv("PROGRAMDATA");
      if (programData != null && !programData.isEmpty()) {
        dirs.add(Paths.get(programData, "ADBC", "Drivers"));
      }
    } else {
      String xdg = System.getenv("XDG_CONFIG_HOME");
      Path base =
          (xdg != null && !xdg.isEmpty())
              ? Paths.get(xdg)
              : (home.isEmpty() ? null : Paths.get(home, ".config"));
      if (base != null) {
        dirs.add(base.resolve("adbc").resolve("drivers"));
      }
    }
    if (!isWindows()) {
      dirs.add(Paths.get("/etc/adbc/drivers"));
      dirs.add(Paths.get("/usr/local/etc/adbc/drivers"));
      dirs.add(Paths.get("/usr/share/adbc/drivers"));
      dirs.add(Paths.get("/usr/local/share/adbc/drivers"));
    }
    return dirs;
  }

  /** The Java stand-in for Python's {@code sys.prefix}: an active virtualenv or conda env. */
  private static List<Path> pythonPrefixes() {
    List<Path> prefixes = new ArrayList<>();
    for (String var : new String[] {"VIRTUAL_ENV", "CONDA_PREFIX"}) {
      String value = System.getenv(var);
      if (value != null && !value.isEmpty()) {
        prefixes.add(Paths.get(value));
      }
    }
    return prefixes;
  }

  private static boolean fromManifest(Search s) {
    Set<Path> seen = new LinkedHashSet<>();
    List<String> missing = new ArrayList<>();
    for (Path dir : manifestDirs()) {
      Path manifest = dir.resolve(MANIFEST_NAME);
      if (!seen.add(manifest)) {
        continue;
      }
      if (!Files.isRegularFile(manifest)) {
        missing.add(dir.toString());
        continue;
      }
      String text;
      try {
        text = new String(Files.readAllBytes(manifest), StandardCharsets.UTF_8);
      } catch (IOException e) {
        s.searched.add("manifest " + manifest + " (unreadable: " + e.getMessage() + ")");
        continue;
      }
      String library = manifestLibrary(text, manifestKeys());
      if (library == null) {
        s.searched.add("manifest " + manifest + " (no [Driver.shared] entry for this platform)");
        continue;
      }
      Path path = expandHome(library);
      if (Files.isRegularFile(path)) {
        s.searched.add("manifest " + manifest + " -> " + path);
        s.found = path;
        return true;
      }
      s.searched.add("manifest " + manifest + " -> " + path + " (not a file)");
    }
    if (!missing.isEmpty()) {
      s.searched.add("ADBC driver manifest " + MANIFEST_NAME + " in: " + String.join(", ", missing));
    }
    return false;
  }

  /**
   * Extract the shared-library entry for this platform from a manifest. The manifests that
   * matter are flat enough to scan by hand: a {@code shared = "..."} string in {@code [Driver]},
   * or one {@code <os>_<arch> = "..."} line per platform in {@code [Driver.shared]}.
   */
  static String manifestLibrary(String text, List<String> keys) {
    String table = "";
    for (String raw : text.split("\\r?\\n")) {
      String line = raw.trim();
      if (line.isEmpty() || line.startsWith("#")) {
        continue;
      }
      if (line.startsWith("[")) {
        int end = line.indexOf(']');
        table = end > 0 ? line.substring(1, end).trim() : "";
        continue;
      }
      int eq = line.indexOf('=');
      if (eq < 0) {
        continue;
      }
      String key = stripQuotes(line.substring(0, eq).trim());
      String value = unquote(line.substring(eq + 1).trim());
      if (value == null) {
        continue; // an inline table or a non-string, not a path
      }
      if ("Driver".equals(table) && "shared".equals(key)) {
        return value;
      }
      if ("Driver.shared".equals(table) && keys.contains(key)) {
        return value;
      }
    }
    return null;
  }

  private static String stripQuotes(String key) {
    if (key.length() >= 2
        && ((key.startsWith("\"") && key.endsWith("\""))
            || (key.startsWith("'") && key.endsWith("'")))) {
      return key.substring(1, key.length() - 1);
    }
    return key;
  }

  /** A TOML basic or literal string, with the trailing comment dropped; {@code null} otherwise. */
  private static String unquote(String value) {
    if (value.isEmpty()) {
      return null;
    }
    char quote = value.charAt(0);
    if (quote != '"' && quote != '\'') {
      return null;
    }
    StringBuilder out = new StringBuilder();
    for (int i = 1; i < value.length(); i++) {
      char c = value.charAt(i);
      if (c == quote) {
        return out.toString();
      }
      if (c == '\\' && quote == '"' && i + 1 < value.length()) {
        char next = value.charAt(++i);
        switch (next) {
          case 'n':
            out.append('\n');
            break;
          case 't':
            out.append('\t');
            break;
          default:
            out.append(next); // \\ and \" and anything else literal
        }
        continue;
      }
      out.append(c);
    }
    return null; // unterminated
  }

  // ------------------------------------------------------------------ 6. install directories

  static List<Path> installDirs() {
    List<Path> dirs = new ArrayList<>();
    for (Path prefix : pythonPrefixes()) {
      dirs.add(prefix.resolve("lib"));
      dirs.add(prefix.resolve("lib64"));
      if (isWindows()) {
        dirs.add(prefix.resolve("bin"));
        dirs.add(prefix.resolve("Library").resolve("bin"));
      }
    }
    if (isWindows()) {
      for (String var : new String[] {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        String base = System.getenv(var);
        if (base != null && !base.isEmpty()) {
          dirs.add(Paths.get(base, "adbcbridge", "bin"));
          dirs.add(Paths.get(base, "adbcbridge", "lib"));
        }
      }
      return dirs;
    }
    for (String base : new String[] {"/usr/local", "/usr", "/opt/adbcbridge", "/opt/homebrew"}) {
      dirs.add(Paths.get(base, "lib"));
      dirs.add(Paths.get(base, "lib64"));
    }
    if ("linux".equals(osName())) {
      String triplet = machine() + "-linux-gnu"; // x86_64-linux-gnu, aarch64-linux-gnu, ...
      dirs.add(Paths.get("/usr/lib", triplet));
      dirs.add(Paths.get("/usr/local/lib", triplet));
    }
    return dirs;
  }

  private static boolean fromInstallDirs(Search s) {
    return scanDirs(s, installDirs(), "install directories");
  }

  private static boolean scanDirs(Search s, List<Path> dirs, String label) {
    List<String> missing = new ArrayList<>();
    for (Path dir : dirs) {
      for (String name : libraryNames()) {
        Path candidate = dir.resolve(name);
        if (Files.isRegularFile(candidate)) {
          s.searched.add(label + ": " + candidate);
          s.found = candidate;
          return true;
        }
      }
      missing.add(dir.toString());
    }
    if (!missing.isEmpty()) {
      s.searched.add(libraryNames().get(0) + " in " + label + ": " + String.join(", ", missing));
    }
    return false;
  }

  // ------------------------------------------------------------------ 7. build trees

  /**
   * CMake build trees next to a source checkout: {@code <ancestor>/build[/<config>]} for every
   * ancestor of the place this class was loaded from ({@code java/target/classes} or {@code
   * java/target/adbcbridge-*.jar} during development) and of the working directory.
   */
  static List<Path> buildDirs() {
    Set<Path> dirs = new LinkedHashSet<>();
    List<Path> starts = new ArrayList<>();
    Path code = codeLocation();
    if (code != null) {
      starts.add(code);
    }
    String cwd = System.getProperty("user.dir");
    if (cwd != null && !cwd.isEmpty()) {
      starts.add(Paths.get(cwd).toAbsolutePath());
    }
    for (Path start : starts) {
      Path p = start;
      for (int depth = 0; p != null && depth < 6; depth++, p = p.getParent()) {
        Path build = p.resolve("build");
        dirs.add(build);
        for (String config : new String[] {"Release", "Debug", "RelWithDebInfo", "MinSizeRel"}) {
          dirs.add(build.resolve(config));
        }
      }
    }
    return new ArrayList<>(dirs);
  }

  private static Path codeLocation() {
    try {
      CodeSource source = DriverLocator.class.getProtectionDomain().getCodeSource();
      if (source == null || source.getLocation() == null) {
        return null;
      }
      Path location = Paths.get(source.getLocation().toURI()).toAbsolutePath();
      return Files.isDirectory(location) ? location : location.getParent();
    } catch (URISyntaxException | IllegalArgumentException | SecurityException e) {
      return null;
    }
  }

  private static boolean fromBuildTrees(Search s) {
    List<Path> dirs = buildDirs();
    for (Path dir : dirs) {
      for (String name : libraryNames()) {
        Path candidate = dir.resolve(name);
        if (Files.isRegularFile(candidate)) {
          s.searched.add("build tree: " + candidate);
          s.found = candidate;
          return true;
        }
      }
    }
    // Report one entry per build/ directory; the per-configuration subdirectories are implied.
    List<String> roots = new ArrayList<>();
    for (Path dir : dirs) {
      if (dir.getFileName() != null && "build".equals(dir.getFileName().toString())) {
        roots.add(dir.toString());
      }
    }
    s.searched.add(
        libraryNames().get(0)
            + " in build trees (each also under Release, Debug, RelWithDebInfo, MinSizeRel): "
            + String.join(", ", roots));
    return false;
  }
}
