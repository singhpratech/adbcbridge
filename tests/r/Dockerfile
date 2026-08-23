# Copyright 2026 the adbcbridge authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0

# Image for the R smoke test: R, the ADBC driver manager R package, and a
# unixODBC driver manager for libadbc_driver_odbc.so to link against.
#
#   docker build -t adbcbridge-r tests/r
#
# rocker/r-ver:4.4 is Ubuntu 24.04 (glibc 2.39) and is preconfigured for the
# Posit binary package repository, so the R packages install as binaries in
# seconds rather than compiling from source.
FROM rocker/r-ver:4.4

# libodbc.so.2 is a hard NEEDED of libadbc_driver_odbc.so; libsqlite3.so.0 is
# what the SQLite ODBC driver it bridges to loads.
RUN apt-get update \
    && apt-get install -y --no-install-recommends unixodbc libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

RUN install2.r --error --skipinstalled adbcdrivermanager nanoarrow

WORKDIR /repo
