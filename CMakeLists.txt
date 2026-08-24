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

cmake_minimum_required(VERSION 3.16)
project(adbcbridge
        VERSION 0.1.0
        DESCRIPTION "adbcbridge: an ADBC driver for any ODBC data source"
        HOMEPAGE_URL "https://adbcbridge.org"
        LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ADBC_ODBC_BUILD_SHARED "Build shared library" ON)
option(ADBC_ODBC_BUILD_TESTS "Build the C smoke test" ON)
option(ADBC_ODBC_ASAN "Build with AddressSanitizer/UBSan" OFF)

if(ADBC_ODBC_ASAN)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ADBC_ODBC_ASAN requires GCC or Clang")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
option(ADBCBRIDGE_BUILD_SHARED "Build shared library" ON)
option(ADBCBRIDGE_INSTALL_MANIFEST "Install an ADBC driver manifest (odbc.toml)" ON)
set(ADBCBRIDGE_MANIFEST_DIR "etc/adbc/drivers"
    CACHE STRING "Install directory for the ADBC driver manifest, relative to the install prefix")

# Native delegation caches the driver manager's loader and unixODBC's odbc.ini
# reader behind a pthread_once/pthread_mutex, so that concurrent
# AdbcDatabaseInit calls cannot race on them.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- ODBC driver manager (unixODBC / iODBC / Windows) ---
find_package(ODBC QUIET)
if(NOT ODBC_FOUND)
  find_path(ODBC_INCLUDE_DIR sql.h)
  find_library(ODBC_LIBRARY NAMES odbc odbc32 iodbc)
  if(NOT ODBC_INCLUDE_DIR OR NOT ODBC_LIBRARY)
    message(FATAL_ERROR "ODBC headers/library not found (install unixodbc-dev)")
  endif()
  add_library(ODBC::ODBC UNKNOWN IMPORTED)
  set_target_properties(ODBC::ODBC PROPERTIES
    IMPORTED_LOCATION "${ODBC_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ODBC_INCLUDE_DIR}")
endif()

add_library(adbc_driver_odbc SHARED
  src/odbc_driver.c
  src/odbc_delegate.c
  src/odbc_reader.c
  src/odbc_partition.c
  src/odbc_objects.c
  src/odbc_bind.c
  src/odbc_text.c
  src/utils.c
  vendor/nanoarrow/nanoarrow.c)

target_include_directories(adbc_driver_odbc
  PRIVATE include vendor src)
target_compile_definitions(adbc_driver_odbc PRIVATE
  ADBC_EXPORTING
  NANOARROW_DLL=)
# ${CMAKE_DL_LIBS}: native delegation resolves the ADBC driver manager
# (AdbcLoadDriver) and unixODBC's odbc.ini reader with dlopen/dlsym rather
# than linking them, which would make the dependency circular.
target_link_libraries(adbc_driver_odbc PRIVATE ODBC::ODBC ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(adbc_driver_odbc PROPERTIES
  C_VISIBILITY_PRESET hidden
  PREFIX "lib"
  OUTPUT_NAME "adbc_driver_odbc")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(adbc_driver_odbc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS adbc_driver_odbc
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# --- ADBC driver manifest -------------------------------------------------
# The ADBC driver manager can discover a driver by name if it finds a TOML
# manifest named <name>.toml in one of its search directories.  We install the
# manifest as "odbc.toml" so that applications can simply pass driver="odbc".
# See https://arrow.apache.org/adbc/current/format/driver_manifests.html
if(ADBCBRIDGE_INSTALL_MANIFEST)
  # Platform tuple used as the key under [Driver.shared], per the ADBC spec.
  if(WIN32)
    set(ADBCBRIDGE_MANIFEST_OS "windows")
  elseif(APPLE)
    set(ADBCBRIDGE_MANIFEST_OS "macos")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(ADBCBRIDGE_MANIFEST_OS "freebsd")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    set(ADBCBRIDGE_MANIFEST_OS "openbsd")
  else()
    set(ADBCBRIDGE_MANIFEST_OS "linux")
  endif()

  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "amd64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i386|i486|i586|i686|x86|X86)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "x86")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|AARCH64|ARM64|armv8b|armv8l)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "arm64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armhf|armv7l)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "arm")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "ppc64le")
    set(ADBCBRIDGE_MANIFEST_ARCH "powerpc64le")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "ppc64")
    set(ADBCBRIDGE_MANIFEST_ARCH "powerpc64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ppc|powerpc)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "powerpc")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
    set(ADBCBRIDGE_MANIFEST_ARCH "riscv64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(s390|s390x)$")
    set(ADBCBRIDGE_MANIFEST_ARCH "s390x")
  else()
    message(WARNING "adbcbridge: unrecognized architecture "
                    "'${CMAKE_SYSTEM_PROCESSOR}'; using it verbatim in the "
                    "driver manifest platform tuple")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ADBCBRIDGE_MANIFEST_ARCH)
  endif()

  # The driver manager appends an environment suffix to the tuple when the
  # toolchain is MinGW or a non-glibc libc: it looks up "linux_amd64_musl", not
  # "linux_amd64".  A manifest keyed without the suffix is simply never matched
  # there, so `driver="odbc"` falls through to dlopen()ing unixODBC's own
  # libodbc.so.  Detect musl exactly the way the driver manager does (glibc's
  # features.h defines __USE_GNU under _GNU_SOURCE, musl's does not) so the two
  # always agree -- including on the shared corner case that any other libc
  # without __USE_GNU is also labelled "musl".
  if(MINGW)
    set(ADBCBRIDGE_MANIFEST_ENV "_mingw")
  elseif(WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(ADBCBRIDGE_MANIFEST_ENV "")
  else()
    include(CheckCSourceCompiles)
    check_c_source_compiles("
#define _GNU_SOURCE
#include <features.h>
#ifndef __USE_GNU
#error no __USE_GNU: not glibc
#endif
int main(void) { return 0; }
" ADBCBRIDGE_LIBC_IS_GLIBC)
    if(ADBCBRIDGE_LIBC_IS_GLIBC)
      set(ADBCBRIDGE_MANIFEST_ENV "")
    else()
      set(ADBCBRIDGE_MANIFEST_ENV "_musl")
    endif()
  endif()

  set(ADBCBRIDGE_MANIFEST_PLATFORM
      "${ADBCBRIDGE_MANIFEST_OS}_${ADBCBRIDGE_MANIFEST_ARCH}${ADBCBRIDGE_MANIFEST_ENV}")

  # Where the shared library will live once installed, relative to the install
  # prefix.  On Windows the DLL goes to the runtime (bin) directory, elsewhere
  # to the library directory.
  if(WIN32)
    set(ADBCBRIDGE_MANIFEST_LIBDIR "${CMAKE_INSTALL_BINDIR}")
  else()
    set(ADBCBRIDGE_MANIFEST_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  endif()

  # The manifest has to spell out an absolute path to the library, so it can
  # only be written once the install prefix is known -- and the prefix is not
  # known until `cmake --install` runs, because `cmake --install --prefix DIR`
  # overrides the configure-time CMAKE_INSTALL_PREFIX.  Generating the manifest
  # at configure time would bake in the wrong path for every relocated install
  # (the driver manager would then fail to dlopen it, or worse, fall back to
  # some other library that happens to be called "odbc").  So the template is
  # expanded during installation instead, into a staging file that the
  # following install(FILES) copies into place (which also gets DESTDIR and
  # install_manifest.txt handling for free).
  set(ADBCBRIDGE_MANIFEST_STAGE
      "${CMAKE_CURRENT_BINARY_DIR}/manifest/odbc.toml")

  # The target sets PREFIX "lib" on every platform (so the DLL is
  # libadbc_driver_odbc.dll, not adbc_driver_odbc.dll), hence the literal "lib"
  # instead of CMAKE_SHARED_LIBRARY_PREFIX.
  install(CODE "
set(PROJECT_VERSION \"${PROJECT_VERSION}\")
set(ADBCBRIDGE_MANIFEST_PLATFORM \"${ADBCBRIDGE_MANIFEST_PLATFORM}\")
set(_adbcbridge_libdir \"${ADBCBRIDGE_MANIFEST_LIBDIR}\")
if(NOT IS_ABSOLUTE \"\${_adbcbridge_libdir}\")
  set(_adbcbridge_libdir \"\${CMAKE_INSTALL_PREFIX}/\${_adbcbridge_libdir}\")
endif()
set(ADBCBRIDGE_MANIFEST_DRIVER_PATH
    \"\${_adbcbridge_libdir}/libadbc_driver_odbc${CMAKE_SHARED_LIBRARY_SUFFIX}\")
configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/adbc_driver_odbc.toml.in\"
               \"${ADBCBRIDGE_MANIFEST_STAGE}\" @ONLY)
message(STATUS
        \"adbcbridge: driver manifest points at \${ADBCBRIDGE_MANIFEST_DRIVER_PATH}\")
")
  install(FILES "${ADBCBRIDGE_MANIFEST_STAGE}"
          DESTINATION "${ADBCBRIDGE_MANIFEST_DIR}")
endif()

# --- Tests ---
# The C smoke test is POSIX-only: it uses dlopen()/dlsym() and mkdtemp() to
# load the driver and create a scratch directory, so it is built only when
# ADBC_ODBC_BUILD_TESTS is ON *and* we are not targeting Windows. The library
# build itself never depends on this test code.
if(ADBC_ODBC_BUILD_TESTS AND NOT WIN32)
  enable_testing()

  # Dependency-free C smoke test: dlopen()s the driver and drives the ADBC
  # 1.1.0 vtable directly. Needs SQLITE_ODBC_DRIVER in the environment
  # (the path to, or the registered name of, the SQLite ODBC driver):
  #   SQLITE_ODBC_DRIVER=/path/to/libsqlite3odbc.so ctest --test-dir build
  add_executable(adbc_odbc_test_driver
    tests/c/test_driver.c
    vendor/nanoarrow/nanoarrow.c)
  target_include_directories(adbc_odbc_test_driver PRIVATE include vendor)
  target_compile_definitions(adbc_odbc_test_driver PRIVATE NANOARROW_DLL=)
  target_link_libraries(adbc_odbc_test_driver PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  add_dependencies(adbc_odbc_test_driver adbc_driver_odbc)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(adbc_odbc_test_driver PRIVATE -Wall -Wextra)
  endif()
  if(ADBC_ODBC_ASAN)
    target_compile_definitions(adbc_odbc_test_driver PRIVATE ADBC_ODBC_TEST_ASAN)
  endif()

  # A stand-in "native" ADBC driver for the delegation tests: it hands back the
  # URI and options adbcbridge translated for it, so tests can assert on exactly
  # what a native driver would have been given.  Never installed.
  add_library(adbc_fake_native_driver SHARED
    tests/c/fake_native_driver.c
    vendor/nanoarrow/nanoarrow.c)
  target_include_directories(adbc_fake_native_driver PRIVATE include vendor)
  target_compile_definitions(adbc_fake_native_driver PRIVATE
    ADBC_EXPORTING
    NANOARROW_DLL=)
  set_target_properties(adbc_fake_native_driver PROPERTIES
    C_VISIBILITY_PRESET hidden
    PREFIX "lib"
    OUTPUT_NAME "adbc_fake_native_driver")
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(adbc_fake_native_driver PRIVATE -Wall -Wextra)
  endif()

  # Fixture for tests/test_driver_load_errors.py: a pair of libraries that
  # reproduce "cannot allocate memory in static TLS block" on demand, without
  # depending on which ODBC drivers or Python packages the host happens to have.
  # See tests/c/tls_dep.c.  ELF/glibc only -- the failure does not exist on
  # Windows, and macOS reserves static TLS differently.
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    add_library(adbc_odbc_tls_dep SHARED tests/c/tls_dep.c)
    add_library(adbc_odbc_tls_user SHARED tests/c/tls_user.c)
    target_link_libraries(adbc_odbc_tls_user PRIVATE adbc_odbc_tls_dep)
    foreach(tls_target IN ITEMS adbc_odbc_tls_dep adbc_odbc_tls_user)
      set_target_properties(${tls_target} PROPERTIES
        PREFIX "lib"
        BUILD_WITH_INSTALL_RPATH OFF)
      target_compile_options(${tls_target} PRIVATE -Wall -Wextra)
    endforeach()
  endif()

  add_test(NAME adbc_odbc_c_smoke
    COMMAND adbc_odbc_test_driver $<TARGET_FILE:adbc_driver_odbc>)
  # SQLITE_ODBC_DRIVER is inherited from the environment ctest runs in; the
  # test reports "skipped" (77) when it is unset and no SQLite ODBC driver is
  # registered under the default name.
  set_tests_properties(adbc_odbc_c_smoke PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
elseif(ADBC_ODBC_BUILD_TESTS AND WIN32)
  message(STATUS
    "adbc_driver_odbc: skipping the C smoke test (POSIX-only: dlopen/mkdtemp)")
endif()

# --- C unit tests (ctest) ---
if(ADBC_ODBC_BUILD_TESTS)
  enable_testing()
  foreach(test_name IN ITEMS test_utf16 test_types test_sqllen32 test_objects test_errors test_multirow test_partition)
    add_executable(${test_name}
      tests/c/${test_name}.c
      src/utils.c
      src/odbc_text.c
      vendor/nanoarrow/nanoarrow.c)
    target_include_directories(${test_name} PRIVATE include vendor src tests/c)
    target_compile_definitions(${test_name} PRIVATE
      ADBC_EXPORTING
      NANOARROW_DLL=)
    # The tests compile the driver's own sources, whose reader and ingest paths
    # use pthreads and dlopen; on glibc < 2.34 (manylinux_2_28) libpthread and
    # libdl are separate DSOs that must be named on the link line.
    target_link_libraries(${test_name} PRIVATE ODBC::ODBC Threads::Threads ${CMAKE_DL_LIBS})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
