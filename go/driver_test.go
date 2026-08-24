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
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

// driverPathOrSkip resolves the library, skipping the test when there is none
// on this machine and nothing points at one.
func driverPathOrSkip(t *testing.T) string {
	t.Helper()
	path, err := DriverPath()
	var notFound *DriverNotFoundError
	if errors.As(err, &notFound) && os.Getenv(EnvDriver) == "" && os.Getenv(EnvLibrary) == "" {
		t.Skipf("no adbcbridge library found; set %s=/path/to/libadbc_driver_odbc.so to run this test:\n%v", EnvDriver, err)
	}
	if err != nil {
		t.Fatalf("DriverPath: %v", err)
	}
	return path
}

// sqliteURI builds the SQLite connection string tests/compat/test_matrix.py
// uses, "Driver=<library>;Database=<file>;", skipping when SQLITE_ODBC_DRIVER
// is unset.
func sqliteURI(t *testing.T) string {
	t.Helper()
	drv := os.Getenv("SQLITE_ODBC_DRIVER")
	if drv == "" {
		t.Skip("SQLITE_ODBC_DRIVER is not set; point it at the SQLite ODBC driver (libsqlite3odbc.so) to run this test")
	}
	return "Driver=" + drv + ";Database=" + filepath.Join(t.TempDir(), "m.db") + ";"
}

// selectOne runs query on db and returns the single value it yields as text.
func selectOne(t *testing.T, ctx context.Context, db adbc.Database, query string) string {
	t.Helper()
	cnxn, err := db.Open(ctx)
	if err != nil {
		t.Fatalf("Open connection: %v", err)
	}
	defer cnxn.Close()
	stmt, err := cnxn.NewStatement()
	if err != nil {
		t.Fatalf("NewStatement: %v", err)
	}
	defer stmt.Close()
	if err := stmt.SetSqlQuery(query); err != nil {
		t.Fatalf("SetSqlQuery: %v", err)
	}
	rdr, _, err := stmt.ExecuteQuery(ctx)
	if err != nil {
		t.Fatalf("ExecuteQuery(%q): %v", query, err)
	}
	defer rdr.Release()
	if n := rdr.Schema().NumFields(); n != 1 {
		t.Fatalf("schema has %d fields, want 1: %v", n, rdr.Schema())
	}
	var value string
	rows := int64(0)
	for rdr.Next() {
		rec := rdr.RecordBatch()
		if rec.NumCols() != 1 {
			t.Fatalf("batch has %d columns, want 1", rec.NumCols())
		}
		for i := 0; i < int(rec.NumRows()); i++ {
			value = rec.Column(0).ValueStr(i)
		}
		rows += rec.NumRows()
	}
	if err := rdr.Err(); err != nil {
		t.Fatalf("reading %q: %v", query, err)
	}
	if rows != 1 {
		t.Fatalf("%q returned %d rows, want 1", query, rows)
	}
	return value
}

func TestDriverPath(t *testing.T) {
	path := driverPathOrSkip(t)
	if !filepath.IsAbs(path) {
		t.Errorf("DriverPath() = %q, want an absolute path", path)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("DriverPath() = %q: %v", path, err)
	}
	if !info.Mode().IsRegular() {
		t.Errorf("DriverPath() = %q is not a regular file", path)
	}
	base := filepath.Base(path)
	if !strings.Contains(base, "adbc_driver_odbc") {
		t.Errorf("DriverPath() = %q, want a libadbc_driver_odbc library", path)
	}
	t.Logf("driver: %s (embedded build: %v)", path, Embedded)
}

func TestDriverPathRejectsMissingEnvFile(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "libadbc_driver_odbc.so")
	t.Setenv(EnvDriver, missing)
	_, err := DriverPath()
	var notFound *DriverNotFoundError
	if !errors.As(err, &notFound) {
		t.Fatalf("DriverPath() error = %v, want *DriverNotFoundError", err)
	}
	if len(notFound.Searched) != 1 || !strings.Contains(notFound.Searched[0], EnvDriver) {
		t.Errorf("Searched = %q, want just the %s entry", notFound.Searched, EnvDriver)
	}
	if !strings.Contains(err.Error(), missing) {
		t.Errorf("error %q does not name the path that was set", err)
	}
}

func TestDriverPathFromLibraryEnv(t *testing.T) {
	path := driverPathOrSkip(t)
	t.Setenv(EnvDriver, "")
	t.Setenv(EnvLibrary, path)
	got, err := DriverPath()
	if err != nil {
		t.Fatalf("DriverPath() with %s: %v", EnvLibrary, err)
	}
	if got != path {
		t.Errorf("DriverPath() = %q, want %q", got, path)
	}
}

func TestDriverPathFromManifest(t *testing.T) {
	path := driverPathOrSkip(t)
	dir := t.TempDir()
	// TOML literal strings, as adbc_driver_odbc.toml.in writes them; forward
	// slashes are fine on Windows too.
	manifest := "manifest_version = 1\nname = 'adbcbridge (ODBC)'\n\n[Driver]\nentrypoint = 'AdbcDriverInit'\n\n" +
		"[Driver.shared]\n" + platformKeys()[0] + " = '" + filepath.ToSlash(path) + "'\n"
	if err := os.WriteFile(filepath.Join(dir, ManifestName), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Setenv(EnvDriver, "")
	t.Setenv(EnvLibrary, "")
	t.Setenv(EnvManifestPath, dir)
	got, err := DriverPath()
	if err != nil {
		t.Fatalf("DriverPath() via manifest: %v", err)
	}
	if filepath.Clean(got) != filepath.Clean(path) {
		t.Errorf("DriverPath() = %q, want %q", got, path)
	}
}

func TestNewDriverSelectOne(t *testing.T) {
	path := driverPathOrSkip(t)
	uri := sqliteURI(t)
	ctx := context.Background()

	drv, err := NewDriver(memory.DefaultAllocator)
	if err != nil {
		t.Fatalf("NewDriver: %v", err)
	}
	if got := drv.(*Driver).Path(); got != path {
		t.Errorf("Driver.Path() = %q, want %q", got, path)
	}
	// adbc.odbc.delegate=never keeps the bridge on its own ODBC path even when a
	// native ADBC SQLite driver is installed, so this exercises adbcbridge.
	db, err := drv.NewDatabase(map[string]string{
		OptionKeyURI:         uri,
		"adbc.odbc.delegate": "never",
	})
	if err != nil {
		t.Fatalf("NewDatabase: %v", err)
	}
	defer db.Close()
	if got := selectOne(t, ctx, db, "SELECT 1"); got != "1" {
		t.Errorf("SELECT 1 = %q, want \"1\"", got)
	}
}

func TestOpenSelectOne(t *testing.T) {
	driverPathOrSkip(t)
	uri := sqliteURI(t)
	ctx := context.Background()

	db, err := Open(ctx, nil, uri, map[string]string{"adbc.odbc.delegate": "never"})
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer db.Close()
	if got := selectOne(t, ctx, db, "SELECT 1"); got != "1" {
		t.Errorf("SELECT 1 = %q, want \"1\"", got)
	}
	if got := selectOne(t, ctx, db, "SELECT 40 + 2"); got != "42" {
		t.Errorf("SELECT 40 + 2 = %q, want \"42\"", got)
	}
}

func TestManifestLibrary(t *testing.T) {
	keys := []string{"linux_amd64", "linux_x86_64"}
	cases := []struct {
		name, text, want string
	}{
		{"table", "[Driver]\nentrypoint = 'AdbcDriverInit'\n[Driver.shared]\nlinux_amd64 = '/opt/x/lib.so'\n", "/opt/x/lib.so"},
		{"alias", "[Driver.shared]\nlinux_x86_64 = \"/opt/y/lib.so\" # comment\n", "/opt/y/lib.so"},
		{"canonical wins", "[Driver.shared]\nlinux_x86_64 = '/b'\nlinux_amd64 = '/a'\n", "/a"},
		{"bare", "[Driver]\nshared = '/opt/z/lib.so'\n", "/opt/z/lib.so"},
		{"dotted", "[Driver]\nshared.linux_amd64 = '/opt/d/lib.so'\n", "/opt/d/lib.so"},
		{"quoted key", "[Driver.shared]\n\"linux_amd64\" = 'C:/x/adbc_driver_odbc.dll'\n", "C:/x/adbc_driver_odbc.dll"},
		{"other platform only", "[Driver.shared]\nwindows_amd64 = 'C:/x.dll'\n", ""},
		{"escape", "[Driver.shared]\nlinux_amd64 = \"C:\\\\x\\\\lib.so\"\n", "C:\\x\\lib.so"},
	}
	for _, c := range cases {
		if got := manifestLibrary(c.text, keys); got != c.want {
			t.Errorf("%s: manifestLibrary = %q, want %q", c.name, got, c.want)
		}
	}
}
