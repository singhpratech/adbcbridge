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

package adbcbridge

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// EnvDriver is the environment variable DriverPath checks first: the path of
// the adbcbridge shared library, as the rest of the repository (tests,
// benchmarks, the Python package) spells it.
const EnvDriver = "ADBC_ODBC_DRIVER"

// EnvLibrary is the second environment variable DriverPath checks, the
// package-specific spelling of the same thing.
const EnvLibrary = "ADBCBRIDGE_LIBRARY"

// EnvManifestPath is the ADBC driver manager's own search-path variable: a
// list of directories, separated by os.PathListSeparator, holding driver
// manifests such as odbc.toml.
const EnvManifestPath = "ADBC_DRIVER_PATH"

// ManifestName is the file name of the ADBC driver manifest that names the
// driver "odbc", as written by install.sh / cmake --install.
const ManifestName = "odbc.toml"

// DriverNotFoundError is returned by DriverPath (and so by NewDriver and Open)
// when no adbcbridge shared library can be found.
type DriverNotFoundError struct {
	// Library is the file name looked for on this OS
	// (libadbc_driver_odbc.so, libadbc_driver_odbc.dylib, adbc_driver_odbc.dll).
	Library string
	// Searched lists, in order, every place that was checked, each with a
	// short note on why it did not yield the library.
	Searched []string
}

func (e *DriverNotFoundError) Error() string {
	var b strings.Builder
	fmt.Fprintf(&b, "adbcbridge: could not find %s; searched:", e.Library)
	for _, s := range e.Searched {
		b.WriteString("\n  ")
		b.WriteString(s)
	}
	fmt.Fprintf(&b, "\nSet %s=/path/to/%s, run install.sh in a checkout of the driver "+
		"(which also writes the %s manifest), or build with -tags adbcbridge_embed "+
		"after copying the library into internal/native/%s_%s/.",
		EnvDriver, e.Library, ManifestName, runtime.GOOS, runtime.GOARCH)
	return b.String()
}

// DriverPath returns the absolute path of the adbcbridge shared library.
//
// It is looked for, in order, in
//
//  1. the ADBC_ODBC_DRIVER environment variable, then ADBCBRIDGE_LIBRARY: an
//     explicit value that does not exist is an error, not a silent fallback;
//  2. the copy embedded in the binary, when built with -tags adbcbridge_embed
//     (extracted to the user cache directory on first use);
//  3. the ADBC driver manifest named "odbc" (odbc.toml) in the directories the
//     ADBC driver manager searches: $ADBC_DRIVER_PATH, the user directory
//     (~/.config/adbc/drivers, ~/Library/Application Support/ADBC/Drivers,
//     %LOCALAPPDATA%\ADBC\Drivers), /etc/adbc/drivers and friends, plus
//     $VIRTUAL_ENV and $CONDA_PREFIX prefixes;
//  4. common install locations (/usr/local/lib, /usr/lib, /opt/adbcbridge/lib,
//     /opt/homebrew/lib, /usr/lib/<arch>-linux-gnu, %ProgramFiles%\adbcbridge,
//     the lib directories of $VIRTUAL_ENV / $CONDA_PREFIX) and a CMake build/
//     tree next to a source checkout of this package.
//
// The error is a *DriverNotFoundError listing everything that was tried.
func DriverPath() (string, error) {
	names := libraryNames()
	notFound := &DriverNotFoundError{Library: names[0]}
	note := func(format string, args ...any) {
		notFound.Searched = append(notFound.Searched, fmt.Sprintf(format, args...))
	}

	// 1. environment variables
	for _, env := range []string{EnvDriver, EnvLibrary} {
		value := os.Getenv(env)
		if value == "" {
			note("$%s (unset)", env)
			continue
		}
		if isFile(value) {
			return absPath(value), nil
		}
		note("$%s=%s (not a file)", env, value)
		return "", notFound
	}

	// 2. copy embedded with -tags adbcbridge_embed
	if path, ok, err := embeddedLibrary(); err != nil {
		note("embedded copy: %v", err)
	} else if ok {
		return path, nil
	} else {
		note("embedded copy (%s)", embedStatus())
	}

	// 3. the "odbc" driver manifest
	seen := map[string]bool{}
	for _, dir := range manifestDirs() {
		manifest := filepath.Join(dir, ManifestName)
		if seen[manifest] {
			continue
		}
		seen[manifest] = true
		text, err := os.ReadFile(manifest)
		if err != nil {
			note("%s (%s)", manifest, readNote(err))
			continue
		}
		library := manifestLibrary(string(text), platformKeys())
		if library == "" {
			note("%s (no [Driver.shared] entry for %s)", manifest, platformKeys()[0])
			continue
		}
		if isFile(library) {
			return absPath(library), nil
		}
		note("%s -> %s (not a file)", manifest, library)
	}

	// 4. install directories and a build tree next to a checkout
	for _, dir := range installDirs() {
		if seen[dir] {
			continue
		}
		seen[dir] = true
		for _, name := range names {
			candidate := filepath.Join(dir, name)
			if isFile(candidate) {
				return absPath(candidate), nil
			}
		}
		note("%s (no %s)", dir, strings.Join(names, " / "))
	}
	return "", notFound
}

// libraryNames returns the file names the driver library goes by on this OS,
// preferred spelling first.
func libraryNames() []string {
	switch runtime.GOOS {
	case "windows":
		return []string{"adbc_driver_odbc.dll", "libadbc_driver_odbc.dll"}
	case "darwin":
		return []string{"libadbc_driver_odbc.dylib"}
	default:
		return []string{"libadbc_driver_odbc.so"}
	}
}

// manifestOS is the OS half of the <os>_<arch> key under [Driver.shared].
func manifestOS() string {
	switch runtime.GOOS {
	case "darwin":
		return "macos"
	default:
		return runtime.GOOS // linux, windows, freebsd, openbsd, ...
	}
}

// archAliases maps GOARCH to the spellings a manifest may use for it: the
// ADBC one first (what cmake --install writes), then uname-style ones a
// hand-written manifest might carry.
var archAliases = map[string][]string{
	"amd64":   {"amd64", "x86_64"},
	"arm64":   {"arm64", "aarch64"},
	"386":     {"x86", "i386", "i686"},
	"arm":     {"arm", "armv7l", "armv6l"},
	"ppc64le": {"powerpc64le", "ppc64le"},
	"ppc64":   {"powerpc64", "ppc64"},
	"s390x":   {"s390x"},
	"riscv64": {"riscv64"},
}

// platformKeys returns the [Driver.shared] keys accepted for this machine,
// most canonical first. The OS half always has to match; on macOS both the
// ADBC spelling (macos) and Go's (darwin) are accepted.
func platformKeys() []string {
	arches, ok := archAliases[runtime.GOARCH]
	if !ok {
		arches = []string{runtime.GOARCH}
	}
	oses := []string{manifestOS()}
	if runtime.GOOS == "darwin" {
		oses = append(oses, "darwin")
	}
	var keys []string
	for _, o := range oses {
		for _, a := range arches {
			keys = append(keys, o+"_"+a)
		}
	}
	return keys
}

// prefixes returns the Python-style installation prefixes in play: an active
// virtualenv or conda environment, where `cmake --install build --prefix
// "$VIRTUAL_ENV"` puts both the manifest and the library.
func prefixes() []string {
	var out []string
	for _, env := range []string{"VIRTUAL_ENV", "CONDA_PREFIX"} {
		if p := os.Getenv(env); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// manifestDirs lists the directories searched for odbc.toml, mirroring the
// ADBC driver manager's own order: $ADBC_DRIVER_PATH, the user configuration
// directory, then the system ones.
func manifestDirs() []string {
	var dirs []string
	if env := os.Getenv(EnvManifestPath); env != "" {
		for _, p := range strings.Split(env, string(os.PathListSeparator)) {
			if p != "" {
				dirs = append(dirs, p)
			}
		}
	}
	for _, prefix := range prefixes() {
		dirs = append(dirs,
			filepath.Join(prefix, "etc", "adbc", "drivers"),
			filepath.Join(prefix, "share", "adbc", "drivers"))
	}
	switch runtime.GOOS {
	case "windows":
		if local := os.Getenv("LOCALAPPDATA"); local != "" {
			dirs = append(dirs, filepath.Join(local, "ADBC", "Drivers"))
		}
	case "darwin":
		if home, err := os.UserHomeDir(); err == nil {
			dirs = append(dirs, filepath.Join(home, "Library", "Application Support", "ADBC", "Drivers"))
		}
		dirs = append(dirs, "/Library/Application Support/ADBC/Drivers")
	default:
		// os.UserConfigDir honours $XDG_CONFIG_HOME, like the driver manager.
		if config, err := os.UserConfigDir(); err == nil {
			dirs = append(dirs, filepath.Join(config, "adbc", "drivers"))
		}
	}
	if runtime.GOOS != "windows" {
		dirs = append(dirs,
			"/etc/adbc/drivers",
			"/usr/local/etc/adbc/drivers",
			"/usr/share/adbc/drivers",
			"/usr/local/share/adbc/drivers")
	}
	return dirs
}

// gnuMachine is the uname -m / GNU triplet architecture for GOARCH, for
// Debian-style /usr/lib/<machine>-linux-gnu directories.
var gnuMachine = map[string]string{
	"amd64":   "x86_64",
	"arm64":   "aarch64",
	"386":     "i386",
	"ppc64le": "powerpc64le",
	"ppc64":   "powerpc64",
	"s390x":   "s390x",
	"riscv64": "riscv64",
}

// installDirs lists the directories a system-wide or prefix install puts the
// library in, followed by a CMake build tree next to a source checkout.
func installDirs() []string {
	var dirs []string
	for _, prefix := range prefixes() {
		dirs = append(dirs, filepath.Join(prefix, "lib"), filepath.Join(prefix, "lib64"))
		if runtime.GOOS == "windows" {
			dirs = append(dirs, filepath.Join(prefix, "bin"), filepath.Join(prefix, "Library", "bin"))
		}
	}
	if runtime.GOOS == "windows" {
		for _, env := range []string{"ProgramFiles", "ProgramW6432", "LOCALAPPDATA"} {
			base := os.Getenv(env)
			if base == "" {
				continue
			}
			if env == "LOCALAPPDATA" {
				base = filepath.Join(base, "Programs")
			}
			dirs = append(dirs,
				filepath.Join(base, "adbcbridge", "bin"),
				filepath.Join(base, "adbcbridge", "lib"))
		}
	} else {
		for _, base := range []string{"/usr/local", "/usr", "/opt/adbcbridge", "/opt/homebrew"} {
			dirs = append(dirs, filepath.Join(base, "lib"), filepath.Join(base, "lib64"))
		}
		if runtime.GOOS == "linux" {
			if machine, ok := gnuMachine[runtime.GOARCH]; ok {
				dirs = append(dirs,
					"/usr/lib/"+machine+"-linux-gnu",
					"/usr/local/lib/"+machine+"-linux-gnu")
			}
		}
	}
	// A CMake build tree next to a source checkout, so a `replace` directive or
	// a go.work pointing at ./go works straight after `cmake --build build`.
	// runtime.Caller gives the path this file was compiled from; with -trimpath
	// (or from the module cache) there is no such tree and the check is moot.
	if _, file, _, ok := runtime.Caller(0); ok && filepath.IsAbs(file) {
		repo := filepath.Dir(filepath.Dir(file))
		for _, sub := range []string{"", "Release", "Debug", "RelWithDebInfo", "MinSizeRel"} {
			dirs = append(dirs, filepath.Join(repo, "build", sub))
		}
	}
	return dirs
}

func isFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func absPath(path string) string {
	if abs, err := filepath.Abs(path); err == nil {
		return abs
	}
	return path
}

func readNote(err error) string {
	if os.IsNotExist(err) {
		return "absent"
	}
	return err.Error()
}
