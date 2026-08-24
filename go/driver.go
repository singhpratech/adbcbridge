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

// Package adbcbridge loads adbcbridge -- the ADBC-over-ODBC driver built as
// libadbc_driver_odbc.so / .dylib / .dll -- through the ADBC Go driver manager.
//
// The package is a thin layer over github.com/apache/arrow-adbc/go/adbc/drivermgr:
// it finds the shared library (DriverPath), hands its path to the driver
// manager (NewDriver) and fills in the ODBC connection string (Open). Everything
// past that -- adbc.Database, adbc.Connection, adbc.Statement -- is the ordinary
// ADBC Go API.
//
// The driver manager is cgo: building this package needs a C compiler (on
// Windows a GCC such as mingw-w64, with CGO_ENABLED=1).
//
// The shared library itself is not part of the module. It comes from
// install.sh or `cmake --install` in a checkout of the driver, from a release
// binary, or from the ADBC driver manifest named "odbc"; see DriverPath for the
// lookup order. Building with `-tags adbcbridge_embed` embeds a copy placed in
// internal/native/<goos>_<goarch>/ into the binary instead.
package adbcbridge

import (
	"context"

	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-adbc/go/adbc/drivermgr"
	"github.com/apache/arrow-go/v18/arrow/memory"
)

// OptionKeyDriver is the drivermgr option naming the shared library to load.
const OptionKeyDriver = "driver"

// OptionKeyURI is the option carrying the ODBC connection string
// ("Driver=...;Database=...;" or "DSN=...;").
const OptionKeyURI = "uri"

// Driver is an adbc.Driver that loads adbcbridge through the ADBC driver
// manager. Its NewDatabase adds the resolved library path under the "driver"
// key and otherwise passes the options to drivermgr.Driver unchanged.
//
// Get one from NewDriver; the zero value is not usable.
type Driver struct {
	path  string
	alloc memory.Allocator
	inner drivermgr.Driver
}

var _ adbc.Driver = (*Driver)(nil)

// NewDriver resolves the adbcbridge shared library with DriverPath and returns
// an adbc.Driver (concretely a *Driver) whose NewDatabase loads it.
//
// alloc is the allocator ADBC Go drivers conventionally take; drivermgr v1.8.0
// imports Arrow data through the C Data Interface and takes no allocator, so it
// is kept only for API parity (see Driver.Allocator). nil selects
// memory.DefaultAllocator.
func NewDriver(alloc memory.Allocator) (adbc.Driver, error) {
	path, err := DriverPath()
	if err != nil {
		return nil, err
	}
	if alloc == nil {
		alloc = memory.DefaultAllocator
	}
	return &Driver{path: path, alloc: alloc}, nil
}

// Path is the shared library NewDatabase loads.
func (d *Driver) Path() string { return d.path }

// Allocator is the allocator NewDriver was given.
func (d *Driver) Allocator() memory.Allocator { return d.alloc }

// NewDatabase creates an adbc.Database backed by the adbcbridge library.
//
// opts are drivermgr / driver options: "uri" carries the ODBC connection
// string, and the driver's own keys ("adbc.odbc.*") go straight through. The
// resolved library path is added under "driver"; a caller-supplied "driver"
// option is left alone, so a specific library can still be forced per database.
func (d *Driver) NewDatabase(opts map[string]string) (adbc.Database, error) {
	return d.NewDatabaseWithContext(context.Background(), opts)
}

// NewDatabaseWithContext is NewDatabase with a context, like
// drivermgr.Driver.NewDatabaseWithContext.
func (d *Driver) NewDatabaseWithContext(ctx context.Context, opts map[string]string) (adbc.Database, error) {
	merged := make(map[string]string, len(opts)+1)
	for k, v := range opts {
		merged[k] = v
	}
	if merged[OptionKeyDriver] == "" {
		merged[OptionKeyDriver] = d.path
	}
	return d.inner.NewDatabaseWithContext(ctx, merged)
}

// Open creates an adbc.Database for connectionString, an ODBC connection
// string such as "Driver=SQLite3;Database=my.db;" or "DSN=mydsn;". options are
// extra database options ("adbc.odbc.delegate", "adbc.odbc.prefetch", ...);
// the connection string is stored under "uri" and wins over any "uri" in
// options.
//
// The database is initialised but not connected: call Open on the result to
// get an adbc.Connection, and Close it when done.
func Open(ctx context.Context, alloc memory.Allocator, connectionString string, options map[string]string) (adbc.Database, error) {
	drv, err := NewDriver(alloc)
	if err != nil {
		return nil, err
	}
	opts := make(map[string]string, len(options)+1)
	for k, v := range options {
		opts[k] = v
	}
	opts[OptionKeyURI] = connectionString
	return drv.(*Driver).NewDatabaseWithContext(ctx, opts)
}
