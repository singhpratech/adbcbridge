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

//go:build !adbcbridge_embed

package adbcbridge

// Embedded reports whether this build carries (or at least looks for) an
// embedded copy of the driver: false without -tags adbcbridge_embed.
const Embedded = false

// embeddedLibrary is the no-embed variant: the library always comes from the
// environment, the manifest or an install directory.
func embeddedLibrary() (string, bool, error) { return "", false, nil }

func embedStatus() string { return "not built with -tags adbcbridge_embed" }
