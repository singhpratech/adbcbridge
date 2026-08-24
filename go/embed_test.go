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

//go:build adbcbridge_embed

package adbcbridge

import (
	"io/fs"
	"os"
	"path"
	"testing"
)

// TestEmbeddedLibrary checks the tagged build in both states: with nothing
// under internal/native/<goos>_<goarch>/ it must report "no copy" without an
// error, and with a library there it must extract it to an executable file.
func TestEmbeddedLibrary(t *testing.T) {
	var present bool
	for _, name := range libraryNames() {
		if _, err := fs.Stat(nativeFS, path.Join(embedDir(), name)); err == nil {
			present = true
		}
	}
	got, ok, err := embeddedLibrary()
	if err != nil {
		t.Fatalf("embeddedLibrary: %v", err)
	}
	if ok != present {
		t.Fatalf("embeddedLibrary ok = %v, but library present in %s = %v", ok, embedDir(), present)
	}
	if !ok {
		t.Logf("nothing embedded under %s; extraction not exercised", embedDir())
		return
	}
	info, err := os.Stat(got)
	if err != nil {
		t.Fatalf("extracted library %s: %v", got, err)
	}
	if !info.Mode().IsRegular() || info.Size() == 0 {
		t.Fatalf("extracted library %s is not a regular non-empty file", got)
	}
	// The extracted copy must win over manifests and install directories when
	// no environment variable overrides it.
	t.Setenv(EnvDriver, "")
	t.Setenv(EnvLibrary, "")
	resolved, err := DriverPath()
	if err != nil {
		t.Fatalf("DriverPath with embedded copy: %v", err)
	}
	if resolved != got {
		t.Errorf("DriverPath() = %q, want the extracted copy %q", resolved, got)
	}
}
