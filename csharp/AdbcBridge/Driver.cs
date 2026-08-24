// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Apache.Arrow.Adbc;
using Apache.Arrow.Adbc.C;
using IOPath = System.IO.Path;

namespace AdbcBridge
{
    /// <summary>
    /// Finds and loads adbcbridge (<c>libadbc_driver_odbc</c>), the ADBC driver
    /// for any ODBC data source, through Apache.Arrow.Adbc's native driver
    /// importer.
    /// </summary>
    public static class Driver
    {
        /// <summary>Environment variable checked first: the full path of the library.</summary>
        public const string LibraryVariable = "ADBCBRIDGE_LIBRARY";

        /// <summary>Environment variable checked second, shared with the Python package and the test suite.</summary>
        public const string DriverVariable = "ADBC_ODBC_DRIVER";

        /// <summary>Extra directories the ADBC driver manager searches for manifests (path-separated).</summary>
        public const string ManifestPathVariable = "ADBC_DRIVER_PATH";

        /// <summary>Name of the ADBC driver manifest <c>cmake --install</c> / <c>install.sh</c> write.</summary>
        public const string ManifestName = "odbc.toml";

        private const string BaseName = "libadbc_driver_odbc";

        private static readonly ConditionalWeakTable<AdbcConnection, Owner> Owners =
            new ConditionalWeakTable<AdbcConnection, Owner>();

        /// <summary>
        /// The absolute path of the adbcbridge shared library, looked up in this order:
        /// <list type="number">
        /// <item><c>ADBCBRIDGE_LIBRARY</c>;</item>
        /// <item><c>ADBC_ODBC_DRIVER</c>;</item>
        /// <item>the copy shipped inside this package, under <c>runtimes/&lt;rid&gt;/native/</c>
        /// next to the AdbcBridge assembly (or flattened beside it by a RID-specific publish);</item>
        /// <item>the ADBC driver manifest named <c>odbc</c> (<c>odbc.toml</c>) in the directories the
        /// ADBC driver manager searches, <c>ADBC_DRIVER_PATH</c> first;</item>
        /// <item>common install directories (<c>~/.local/lib</c>, <c>/usr/local/lib</c>, <c>/usr/lib</c>, ...);</item>
        /// <item>a CMake <c>build/</c> tree next to a source checkout, above the application or the
        /// current directory.</item>
        /// </list>
        /// </summary>
        /// <exception cref="DriverNotFoundException">none of those has the library.</exception>
        public static string Path()
        {
            List<string> searched = new List<string>();
            string? found = FromEnvironment(LibraryVariable, searched)
                ?? FromEnvironment(DriverVariable, searched)
                ?? FromPackage(searched)
                ?? FromManifest(searched)
                ?? FromInstallDirectories(searched)
                ?? FromBuildTree(searched);
            if (found is null)
            {
                throw new DriverNotFoundException(LibraryNames()[0], searched, null);
            }

            return found;
        }

        /// <summary>
        /// Load the driver found by <see cref="Path"/> with
        /// <c>CAdbcDriverImporter.Load(path)</c>. Dispose the result when done.
        /// </summary>
        public static AdbcDriver Load()
        {
            return CAdbcDriverImporter.Load(Path());
        }

        /// <summary>
        /// Open an ADBC connection to an ODBC data source: loads the driver, opens an
        /// <see cref="AdbcDatabase"/> with <c>uri</c> set to <paramref name="connectionString"/>
        /// (plus <paramref name="options"/>, e.g. <c>adbc.odbc.batch_size</c>), and connects.
        /// The database and driver stay alive for as long as the returned connection does.
        /// </summary>
        /// <param name="connectionString">An ODBC connection string, e.g.
        /// <c>Driver=SQLite3;Database=my.db;</c>. <c>Driver=</c> takes a registered driver name or
        /// the path of the ODBC driver library.</param>
        /// <param name="options">Further database options; a <c>uri</c> entry here is overridden by
        /// <paramref name="connectionString"/>.</param>
        public static AdbcConnection Connect(string connectionString, IReadOnlyDictionary<string, string>? options = null)
        {
            if (connectionString is null)
            {
                throw new ArgumentNullException(nameof(connectionString));
            }

            Dictionary<string, string> parameters = new Dictionary<string, string>(StringComparer.Ordinal);
            if (options is not null)
            {
                foreach (KeyValuePair<string, string> option in options)
                {
                    parameters[option.Key] = option.Value;
                }
            }

            parameters["uri"] = connectionString;

            AdbcDriver driver = Load();
            AdbcDatabase? database = null;
            try
            {
                database = driver.Open(parameters);
                AdbcConnection connection = database.Connect(null);
                Owners.Add(connection, new Owner(driver, database));
                return connection;
            }
            catch
            {
                database?.Dispose();
                driver.Dispose();
                throw;
            }
        }

        // --- 1. and 2. environment variables ---------------------------------

        private static string? FromEnvironment(string variable, List<string> searched)
        {
            string? value = Environment.GetEnvironmentVariable(variable);
            if (string.IsNullOrEmpty(value))
            {
                searched.Add(variable + ": not set");
                return null;
            }

            string path = ExpandHome(value!);
            if (!File.Exists(path))
            {
                searched.Add(variable + "=" + value + ": not a file");
                throw new DriverNotFoundException(
                    LibraryNames()[0], searched, variable + "=" + value + " does not point at a file.");
            }

            searched.Add(variable + "=" + value);
            return IOPath.GetFullPath(path);
        }

        // --- 3. the copy inside this package ---------------------------------

        private static string? FromPackage(List<string> searched)
        {
            List<string> bases = new List<string>();
            string? assemblyDirectory = AssemblyDirectory();
            if (assemblyDirectory is not null)
            {
                bases.Add(assemblyDirectory);
            }

            string? baseDirectory = Normalize(AppContext.BaseDirectory);
            if (baseDirectory is not null && !bases.Contains(baseDirectory))
            {
                bases.Add(baseDirectory);
            }

            List<string> candidates = new List<string>();
            foreach (string root in bases)
            {
                foreach (string rid in RuntimeIdentifiers())
                {
                    // runtimes/<rid>/native/ copied next to the app by a framework-dependent build,
                    // and the same tree above lib/<tfm>/ when the assembly runs straight out of a
                    // package directory.
                    candidates.Add(IOPath.Combine(root, "runtimes", rid, "native"));
                    candidates.Add(IOPath.Combine(root, "..", "..", "runtimes", rid, "native"));
                }

                // A RID-specific publish flattens runtimes/<rid>/native/ beside the assembly.
                candidates.Add(root);
            }

            return Probe(candidates, "package asset", searched);
        }

        // --- 4. the ADBC driver manifest named "odbc" ------------------------

        /// <summary>Directories the ADBC driver manager searches for <c>odbc.toml</c>.</summary>
        internal static List<string> ManifestDirectories()
        {
            List<string> directories = new List<string>();
            string? extra = Environment.GetEnvironmentVariable(ManifestPathVariable);
            if (!string.IsNullOrEmpty(extra))
            {
                foreach (string entry in extra!.Split(IOPath.PathSeparator))
                {
                    if (entry.Length > 0)
                    {
                        directories.Add(entry);
                    }
                }
            }

            string home = Home();
            if (IsWindows())
            {
                Add(directories, Environment.GetEnvironmentVariable("LOCALAPPDATA"), "ADBC", "Drivers");
                Add(directories, Environment.GetEnvironmentVariable("PROGRAMDATA"), "ADBC", "Drivers");
            }
            else if (IsMacOS())
            {
                Add(directories, home, "Library", "Application Support", "ADBC", "Drivers");
                directories.Add("/Library/Application Support/ADBC/Drivers");
            }
            else
            {
                string? config = Environment.GetEnvironmentVariable("XDG_CONFIG_HOME");
                if (string.IsNullOrEmpty(config))
                {
                    Add(directories, home, ".config", "adbc", "drivers");
                }
                else
                {
                    Add(directories, config, "adbc", "drivers");
                }
            }

            if (!IsWindows())
            {
                directories.Add("/etc/adbc/drivers");
                directories.Add("/usr/local/etc/adbc/drivers");
                directories.Add("/usr/share/adbc/drivers");
                directories.Add("/usr/local/share/adbc/drivers");
            }

            return directories;
        }

        private static string? FromManifest(List<string> searched)
        {
            HashSet<string> seen = new HashSet<string>(StringComparer.Ordinal);
            List<string> keys = ManifestPlatformKeys();
            foreach (string directory in ManifestDirectories())
            {
                string manifest = IOPath.Combine(directory, ManifestName);
                if (!seen.Add(manifest))
                {
                    continue;
                }

                string text;
                try
                {
                    if (!File.Exists(manifest))
                    {
                        searched.Add("manifest " + manifest + ": missing");
                        continue;
                    }

                    text = File.ReadAllText(manifest);
                }
                catch (IOException)
                {
                    searched.Add("manifest " + manifest + ": unreadable");
                    continue;
                }
                catch (UnauthorizedAccessException)
                {
                    searched.Add("manifest " + manifest + ": unreadable");
                    continue;
                }

                string? library = ManifestLibrary(text, keys);
                if (library is null)
                {
                    searched.Add("manifest " + manifest + ": no [Driver.shared] entry for " + keys[0]);
                    continue;
                }

                string path = ExpandHome(library);
                if (!IOPath.IsPathRooted(path))
                {
                    path = IOPath.Combine(directory, path);
                }

                if (File.Exists(path))
                {
                    searched.Add("manifest " + manifest + " -> " + path);
                    return IOPath.GetFullPath(path);
                }

                searched.Add("manifest " + manifest + " -> " + path + ": not a file");
            }

            return null;
        }

        /// <summary>
        /// The library a manifest names for this platform: <c>[Driver.shared]</c>'s entry for one of
        /// <paramref name="keys"/>, or a bare <c>shared = '...'</c> under <c>[Driver]</c>. The
        /// manifests that matter are flat, so a line scanner does instead of a TOML parser.
        /// </summary>
        internal static string? ManifestLibrary(string text, IReadOnlyList<string> keys)
        {
            string section = "";
            foreach (string raw in text.Split('\n'))
            {
                string line = StripComment(raw).Trim();
                if (line.Length == 0)
                {
                    continue;
                }

                if (line[0] == '[')
                {
                    section = line.TrimEnd().TrimEnd(']').TrimStart('[').Trim();
                    continue;
                }

                int equals = line.IndexOf('=');
                if (equals <= 0)
                {
                    continue;
                }

                string key = line.Substring(0, equals).Trim().Trim('"', '\'');
                string? value = Unquote(line.Substring(equals + 1).Trim());
                if (value is null || value.Length == 0)
                {
                    continue;
                }

                if (section == "Driver" && key == "shared")
                {
                    return value;
                }

                if (section == "Driver.shared")
                {
                    foreach (string candidate in keys)
                    {
                        if (string.Equals(candidate, key, StringComparison.OrdinalIgnoreCase))
                        {
                            return value;
                        }
                    }
                }
            }

            return null;
        }

        private static string StripComment(string line)
        {
            bool single = false, twin = false;
            for (int i = 0; i < line.Length; i++)
            {
                char c = line[i];
                if (c == '\'' && !twin)
                {
                    single = !single;
                }
                else if (c == '"' && !single)
                {
                    twin = !twin;
                }
                else if (c == '#' && !single && !twin)
                {
                    return line.Substring(0, i);
                }
            }

            return line;
        }

        private static string? Unquote(string value)
        {
            if (value.Length >= 2 && (value[0] == '\'' || value[0] == '"'))
            {
                int end = value.IndexOf(value[0], 1);
                if (end > 0)
                {
                    string inner = value.Substring(1, end - 1);
                    return value[0] == '"' ? inner.Replace("\\\\", "\\") : inner;
                }

                return null;
            }

            // An inline table or anything else that is not a string.
            return value[0] == '{' ? null : value;
        }

        /// <summary>
        /// The <c>[Driver.shared]</c> keys accepted for this machine, canonical ADBC spelling
        /// (<c>linux_amd64</c>) first, uname spellings (<c>linux_x86_64</c>) after.
        /// </summary>
        internal static List<string> ManifestPlatformKeys()
        {
            string os = IsWindows() ? "windows"
                : IsMacOS() ? "macos"
                : IsFreeBSD() ? "freebsd"
                : "linux";
            List<string> keys = new List<string>();
            foreach (string arch in ArchitectureNames())
            {
                keys.Add(os + "_" + arch);
            }

            return keys;
        }

        // --- 5. common install directories -----------------------------------

        private static string? FromInstallDirectories(List<string> searched)
        {
            List<string> directories = new List<string>();
            string home = Home();
            if (IsWindows())
            {
                Add(directories, Environment.GetEnvironmentVariable("LOCALAPPDATA"), "adbcbridge", "bin");
                Add(directories, Environment.GetEnvironmentVariable("ProgramFiles"), "adbcbridge", "bin");
            }
            else
            {
                Add(directories, home, ".local", "lib");
                Add(directories, home, ".local", "lib64");
                foreach (string prefix in new[] { "/usr/local", "/usr", "/opt/adbcbridge", "/opt/homebrew" })
                {
                    directories.Add(prefix + "/lib");
                    directories.Add(prefix + "/lib64");
                }

                if (IsLinux())
                {
                    string machine = UnameMachine();
                    directories.Add("/usr/lib/" + machine + "-linux-gnu");
                    directories.Add("/usr/local/lib/" + machine + "-linux-gnu");
                }
            }

            return Probe(directories, "install dir", searched);
        }

        // --- 6. a CMake build tree next to a checkout ------------------------

        private static string? FromBuildTree(List<string> searched)
        {
            List<string> starts = new List<string>();
            string? assemblyDirectory = AssemblyDirectory();
            if (assemblyDirectory is not null)
            {
                starts.Add(assemblyDirectory);
            }

            string? baseDirectory = Normalize(AppContext.BaseDirectory);
            if (baseDirectory is not null && !starts.Contains(baseDirectory))
            {
                starts.Add(baseDirectory);
            }

            try
            {
                string? current = Normalize(Directory.GetCurrentDirectory());
                if (current is not null && !starts.Contains(current))
                {
                    starts.Add(current);
                }
            }
            catch (IOException)
            {
                // No current directory: nothing to walk up from.
            }
            catch (UnauthorizedAccessException)
            {
                // Ditto.
            }

            string[] configurations = { "", "Release", "Debug", "RelWithDebInfo", "MinSizeRel" };
            HashSet<string> seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (string start in starts)
            {
                string? directory = start;
                for (int depth = 0; depth < 8 && directory is not null; depth++)
                {
                    if (seen.Add(directory))
                    {
                        string build = IOPath.Combine(directory, "build");
                        foreach (string configuration in configurations)
                        {
                            string candidate = configuration.Length == 0 ? build : IOPath.Combine(build, configuration);
                            foreach (string name in LibraryNames())
                            {
                                string file = IOPath.Combine(candidate, name);
                                if (File.Exists(file))
                                {
                                    searched.Add("build tree " + file);
                                    return file;
                                }
                            }
                        }

                        searched.Add("build tree " + build + "{,/Release,/Debug,/RelWithDebInfo,/MinSizeRel}/"
                                     + LibraryNames()[0] + ": missing");
                    }

                    directory = IOPath.GetDirectoryName(directory);
                }
            }

            return null;
        }

        // --- helpers ---------------------------------------------------------

        /// <summary>The file names the library goes by on this platform, preferred first.</summary>
        internal static string[] LibraryNames()
        {
            if (IsWindows())
            {
                return new[] { BaseName + ".dll", "adbc_driver_odbc.dll" };
            }

            return new[] { IsMacOS() ? BaseName + ".dylib" : BaseName + ".so" };
        }

        /// <summary>The .NET runtime identifier the package's native asset is filed under.</summary>
        public static string RuntimeIdentifier()
        {
            string os = IsWindows() ? "win" : IsMacOS() ? "osx" : IsFreeBSD() ? "freebsd" : "linux";
            string arch;
            switch (RuntimeInformation.ProcessArchitecture)
            {
                case Architecture.X64: arch = "x64"; break;
                case Architecture.X86: arch = "x86"; break;
                case Architecture.Arm64: arch = "arm64"; break;
                case Architecture.Arm: arch = "arm"; break;
                default: arch = RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant(); break;
            }

            return os + "-" + arch;
        }

        private static List<string> RuntimeIdentifiers()
        {
            List<string> rids = new List<string>();
#if NET5_0_OR_GREATER
            // The runtime's own idea first: catches linux-musl-x64 and friends.
            string own = RuntimeInformation.RuntimeIdentifier;
            if (!string.IsNullOrEmpty(own))
            {
                rids.Add(own);
            }
#endif
            string computed = RuntimeIdentifier();
            if (!rids.Contains(computed))
            {
                rids.Add(computed);
            }

            return rids;
        }

        private static string? Probe(List<string> directories, string label, List<string> searched)
        {
            HashSet<string> seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (string directory in directories)
            {
                string? full = Normalize(directory);
                if (full is null || !seen.Add(full))
                {
                    continue;
                }

                foreach (string name in LibraryNames())
                {
                    string candidate = IOPath.Combine(full, name);
                    if (File.Exists(candidate))
                    {
                        searched.Add(label + " " + candidate);
                        return candidate;
                    }
                }

                searched.Add(label + " " + IOPath.Combine(full, LibraryNames()[0]) + ": missing");
            }

            return null;
        }

        private static string? AssemblyDirectory()
        {
            try
            {
                string location = typeof(Driver).GetTypeInfo().Assembly.Location;
                if (string.IsNullOrEmpty(location))
                {
                    return null; // single-file bundle: AppContext.BaseDirectory covers it
                }

                return Normalize(IOPath.GetDirectoryName(location));
            }
            catch (NotSupportedException)
            {
                return null;
            }
        }

        /// <summary>Absolute form of a directory without a trailing separator, or null if it is not a usable path.</summary>
        private static string? Normalize(string? directory)
        {
            if (string.IsNullOrEmpty(directory))
            {
                return null;
            }

            try
            {
                string full = IOPath.GetFullPath(directory!);
                string trimmed = full.TrimEnd(IOPath.DirectorySeparatorChar, IOPath.AltDirectorySeparatorChar);
                return trimmed.Length == 0 || IOPath.GetPathRoot(full) == full ? full : trimmed;
            }
            catch (ArgumentException)
            {
                return null;
            }
            catch (NotSupportedException)
            {
                return null;
            }
            catch (PathTooLongException)
            {
                return null;
            }
        }

        /// <summary>Architecture names ADBC manifests use for this process, canonical first.</summary>
        private static List<string> ArchitectureNames()
        {
            switch (RuntimeInformation.ProcessArchitecture)
            {
                case Architecture.X64: return new List<string> { "amd64", "x86_64" };
                case Architecture.X86: return new List<string> { "x86", "i386", "i686" };
                case Architecture.Arm64: return new List<string> { "arm64", "aarch64" };
                case Architecture.Arm: return new List<string> { "arm", "armv7l" };
                default:
                    string name = RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant();
                    return new List<string> { name };
            }
        }

        private static string UnameMachine()
        {
            switch (RuntimeInformation.ProcessArchitecture)
            {
                case Architecture.X64: return "x86_64";
                case Architecture.X86: return "i386";
                case Architecture.Arm64: return "aarch64";
                case Architecture.Arm: return "arm";
                default: return RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant();
            }
        }

        private static bool IsWindows() => RuntimeInformation.IsOSPlatform(OSPlatform.Windows);

        private static bool IsMacOS() => RuntimeInformation.IsOSPlatform(OSPlatform.OSX);

        private static bool IsLinux() => RuntimeInformation.IsOSPlatform(OSPlatform.Linux);

        private static bool IsFreeBSD() => RuntimeInformation.IsOSPlatform(OSPlatform.Create("FREEBSD"));

        private static string Home()
        {
            string? home = Environment.GetEnvironmentVariable(IsWindows() ? "USERPROFILE" : "HOME");
            if (string.IsNullOrEmpty(home))
            {
                home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            }

            return home ?? "";
        }

        private static string ExpandHome(string path)
        {
            if (path.Length >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\'))
            {
                string home = Home();
                if (home.Length > 0)
                {
                    return IOPath.Combine(home, path.Substring(2));
                }
            }

            return path;
        }

        private static void Add(List<string> directories, string? root, params string[] parts)
        {
            if (string.IsNullOrEmpty(root))
            {
                return;
            }

            string path = root!;
            foreach (string part in parts)
            {
                path = IOPath.Combine(path, part);
            }

            directories.Add(path);
        }

        /// <summary>Keeps a connection's database and driver reachable for as long as the connection is.</summary>
        private sealed class Owner
        {
            public Owner(AdbcDriver driver, AdbcDatabase database)
            {
                Driver = driver;
                Database = database;
            }

            public AdbcDriver Driver { get; }

            public AdbcDatabase Database { get; }
        }
    }
}
