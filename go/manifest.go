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
	"os"
	"strings"
)

// manifestLibrary extracts the shared-library path for this platform from the
// text of an ADBC driver manifest.
//
// A manifest's [Driver] table carries either a bare `shared = '<path>'` or a
// [Driver.shared] table keyed by `<os>_<arch>`. Rather than pull in a TOML
// parser for two keys, this scans the line-oriented subset those manifests
// use (tables, dotted keys, single- or double-quoted strings, # comments);
// keys are tried in the order given so the canonical spelling wins.
func manifestLibrary(text string, keys []string) string {
	found := map[string]string{}
	table := ""
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") {
			// [Driver.shared] or [[array]] -- only plain tables matter here.
			name := strings.TrimSpace(strings.TrimSuffix(strings.TrimPrefix(line, "["), "]"))
			table = unquoteKey(name)
			continue
		}
		eq := strings.Index(line, "=")
		if eq < 0 {
			continue
		}
		key := unquoteKey(strings.TrimSpace(line[:eq]))
		value, ok := tomlString(strings.TrimSpace(line[eq+1:]))
		if !ok {
			continue
		}
		if table != "" {
			key = table + "." + key
		}
		found[key] = value
	}
	if bare := found["Driver.shared"]; bare != "" {
		return os.ExpandEnv(bare)
	}
	for _, key := range keys {
		if v := found["Driver.shared."+key]; v != "" {
			return os.ExpandEnv(v)
		}
	}
	return ""
}

// unquoteKey strips the quotes from each segment of a (possibly dotted,
// possibly quoted) TOML key: `Driver."shared"` -> Driver.shared.
func unquoteKey(key string) string {
	parts := strings.Split(key, ".")
	for i, p := range parts {
		p = strings.TrimSpace(p)
		if len(p) >= 2 && (p[0] == '\'' || p[0] == '"') && p[len(p)-1] == p[0] {
			p = p[1 : len(p)-1]
		}
		parts[i] = p
	}
	return strings.Join(parts, ".")
}

// tomlString decodes a TOML string value at the start of s: a literal
// ('...', no escapes) or a basic ("...", backslash escapes) string, with any
// trailing comment ignored. ok is false for anything else (tables, numbers,
// multi-line strings).
func tomlString(s string) (value string, ok bool) {
	if len(s) < 2 {
		return "", false
	}
	switch s[0] {
	case '\'':
		end := strings.IndexByte(s[1:], '\'')
		if end < 0 {
			return "", false
		}
		return s[1 : 1+end], true
	case '"':
		var b strings.Builder
		for i := 1; i < len(s); i++ {
			c := s[i]
			switch c {
			case '"':
				return b.String(), true
			case '\\':
				if i+1 >= len(s) {
					return "", false
				}
				i++
				switch s[i] {
				case 'n':
					b.WriteByte('\n')
				case 't':
					b.WriteByte('\t')
				case 'r':
					b.WriteByte('\r')
				default: // \\ \" and anything else: keep the character
					b.WriteByte(s[i])
				}
			default:
				b.WriteByte(c)
			}
		}
		return "", false
	}
	return "", false
}
