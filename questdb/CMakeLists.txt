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

# adbc_driver_questdb: an ADBC driver for QuestDB over QWP, the QuestDB wire
# protocol, built on QuestDB's official C client (questdb/c-questdb-client).
#
# This is a separate CMake project from the ODBC driver at the repository
# root because it needs a Rust toolchain (the client is written in Rust and
# exposes a C11 ABI) and pulls arrow-rs for the Arrow C Data Interface
# exports.  Build it on its own:
#
#   cmake -S questdb -B build-questdb -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-questdb
#
# Requirements: CMake 3.16+, a C11 compiler, Rust 1.91+ (rustup), git.

cmake_minimum_required(VERSION 3.16)
project(adbcbridge_questdb
        VERSION 0.1.0
        DESCRIPTION "adbcBridge for QuestDB: an ADBC driver over the QuestDB wire protocol (QWP)"
        HOMEPAGE_URL "https://adbcbridge.org"
        LANGUAGES C)

include(GNUInstallDirs)
include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ADBC_QUESTDB_BUILD_TESTS "Build the C smoke test" ON)

# The pinned client release.  Bump both lines together: the tag is for humans,
# the commit is what gets built.
set(ADBC_QUESTDB_CLIENT_TAG "7.2.0")
set(ADBC_QUESTDB_CLIENT_COMMIT "a236dbffba41239d08740ea0d24eb7e0e7fafa47")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- QuestDB C client -------------------------------------------------------
# Static link: the Rust core ends up inside libadbc_driver_questdb.so, so the
# driver is a single file for the ADBC driver manager to load.  The client's
# C++ API, tests and examples are not built; only the Arrow C Data Interface
# exports are switched on.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QUESTDB_ENABLE_ARROW ON CACHE BOOL "" FORCE)
set(QUESTDB_TESTS_AND_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUESTDB_QWP_BENCH OFF CACHE BOOL "" FORCE)
set(QUESTDB_ENABLE_INSECURE_SKIP_VERIFY ON CACHE BOOL "" FORCE)
# The source tarball of that commit, pinned by hash.  A git clone would also
# pull the client's `questdb` submodule, the whole server repository, which
# only its system tests use.
set(ADBC_QUESTDB_CLIENT_SHA256 "e2884ba1a5bca1d6c86502b8dedf9d1dd3eac8428e7dbcdee8bccf4690c4545d")
FetchContent_Declare(c_questdb_client
  URL      https://github.com/questdb/c-questdb-client/archive/${ADBC_QUESTDB_CLIENT_COMMIT}.tar.gz
  URL_HASH SHA256=${ADBC_QUESTDB_CLIENT_SHA256})
FetchContent_MakeAvailable(c_questdb_client)

# --- The driver ---------------------------------------------------------------
add_library(adbc_driver_questdb SHARED
  src/qwp_driver.c
  ../src/utils.c
  ../vendor/nanoarrow/nanoarrow.c)

target_include_directories(adbc_driver_questdb PRIVATE
  ../include ../vendor ../src src
  ${c_questdb_client_SOURCE_DIR}/include)
target_compile_definitions(adbc_driver_questdb PRIVATE
  ADBC_EXPORTING
  NANOARROW_DLL=
  QUESTDB_CLIENT_ENABLE_ARROW
  ADBC_QUESTDB_VERSION="${PROJECT_VERSION}"
  ADBC_QUESTDB_CLIENT_TAG="${ADBC_QUESTDB_CLIENT_TAG}")
target_link_libraries(adbc_driver_questdb PRIVATE questdb_client Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(adbc_driver_questdb PROPERTIES
  C_VISIBILITY_PRESET hidden
  PREFIX "lib"
  OUTPUT_NAME "adbc_driver_questdb")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(adbc_driver_questdb PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# Keep the client's own symbols out of our export table: an application that
# also loads libquestdb_client must not resolve its calls into this library.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(adbc_driver_questdb PRIVATE "LINKER:--exclude-libs,ALL")
elseif(APPLE)
  target_link_options(adbc_driver_questdb PRIVATE
    "LINKER:-exported_symbol,_AdbcDriverInit"
    "LINKER:-exported_symbol,_AdbcDriverQuestDBInit")
endif()

install(TARGETS adbc_driver_questdb
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# --- C smoke test -------------------------------------------------------------
if(ADBC_QUESTDB_BUILD_TESTS AND NOT WIN32)
  enable_testing()
  add_executable(adbc_questdb_test_driver tests/c/test_qwp_driver.c ../vendor/nanoarrow/nanoarrow.c)
  target_include_directories(adbc_questdb_test_driver PRIVATE ../include ../vendor)
  target_compile_definitions(adbc_questdb_test_driver PRIVATE NANOARROW_DLL=)
  target_link_libraries(adbc_questdb_test_driver PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  add_dependencies(adbc_questdb_test_driver adbc_driver_questdb)
  add_test(NAME adbc_questdb_test_driver
           COMMAND adbc_questdb_test_driver $<TARGET_FILE:adbc_driver_questdb>)
endif()
