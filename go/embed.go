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
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"runtime"
	"sync"
)

// Embedded reports whether this build carries (or at least looks for) an
// embedded copy of the driver: true under -tags adbcbridge_embed.
const Embedded = true

// nativeFS holds internal/native/<goos>_<goarch>/<library> for every platform
// a library was copied in for before `go build -tags adbcbridge_embed`. The
// `all:` prefix keeps the pattern valid when the tree holds nothing but
// .gitkeep, so the tagged build compiles in a fresh checkout too -- it just
// embeds no library, and DriverPath moves on to the manifest and install
// directories.
//
//go:embed all:internal/native
var nativeFS embed.FS

var extractOnce struct {
	sync.Once
	path string
	ok   bool
	err  error
}

// embedDir is the embedded directory for the running platform.
func embedDir() string {
	return path.Join("internal", "native", runtime.GOOS+"_"+runtime.GOARCH)
}

// embeddedLibrary returns the path of the embedded driver extracted to the
// user cache directory (or the temporary directory when there is none). ok is
// false when nothing was embedded for this platform.
func embeddedLibrary() (string, bool, error) {
	extractOnce.Do(func() {
		extractOnce.path, extractOnce.ok, extractOnce.err = extractEmbedded()
	})
	return extractOnce.path, extractOnce.ok, extractOnce.err
}

func embedStatus() string {
	if _, ok, err := embeddedLibrary(); err != nil {
		return err.Error()
	} else if ok {
		return "extracted"
	}
	return "none for " + embedDir()
}

func extractEmbedded() (string, bool, error) {
	var data []byte
	var name string
	for _, n := range libraryNames() {
		b, err := nativeFS.ReadFile(path.Join(embedDir(), n))
		if err == nil {
			data, name = b, n
			break
		}
		if !errors.Is(err, fs.ErrNotExist) {
			return "", false, fmt.Errorf("adbcbridge: reading embedded %s: %w", n, err)
		}
	}
	if name == "" {
		return "", false, nil
	}

	// One directory per distinct library content, so an upgraded binary never
	// loads a stale extraction and concurrent processes converge on the same
	// file.
	sum := sha256.Sum256(data)
	base, err := os.UserCacheDir()
	if err != nil || base == "" {
		base = os.TempDir()
	}
	dir := filepath.Join(base, "adbcbridge", "native", hex.EncodeToString(sum[:12]))
	target := filepath.Join(dir, name)
	if info, err := os.Stat(target); err == nil && info.Mode().IsRegular() && info.Size() == int64(len(data)) {
		return target, true, nil
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", false, fmt.Errorf("adbcbridge: creating %s: %w", dir, err)
	}
	tmp, err := os.CreateTemp(dir, name+".*.tmp")
	if err != nil {
		return "", false, fmt.Errorf("adbcbridge: extracting %s: %w", name, err)
	}
	tmpName := tmp.Name()
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		os.Remove(tmpName)
		return "", false, fmt.Errorf("adbcbridge: extracting %s: %w", name, err)
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmpName)
		return "", false, fmt.Errorf("adbcbridge: extracting %s: %w", name, err)
	}
	if err := os.Chmod(tmpName, 0o755); err != nil {
		os.Remove(tmpName)
		return "", false, fmt.Errorf("adbcbridge: extracting %s: %w", name, err)
	}
	if err := os.Rename(tmpName, target); err != nil {
		os.Remove(tmpName)
		// Another process may have won the race (Windows refuses to replace an
		// open file); its copy has the same content.
		if info, statErr := os.Stat(target); statErr == nil && info.Size() == int64(len(data)) {
			return target, true, nil
		}
		return "", false, fmt.Errorf("adbcbridge: extracting %s: %w", name, err)
	}
	return target, true, nil
}
