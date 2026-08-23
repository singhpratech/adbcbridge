cmake_minimum_required(VERSION 3.16)
project(adbc_driver_odbc C)

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
  src/odbc_reader.c
  src/odbc_objects.c
  src/odbc_bind.c
  src/utils.c
  vendor/nanoarrow/nanoarrow.c)

target_include_directories(adbc_driver_odbc
  PRIVATE include vendor src)
target_compile_definitions(adbc_driver_odbc PRIVATE
  ADBC_EXPORTING
  NANOARROW_DLL=)
target_link_libraries(adbc_driver_odbc PRIVATE ODBC::ODBC)
set_target_properties(adbc_driver_odbc PROPERTIES
  C_VISIBILITY_PRESET hidden
  PREFIX "lib"
  OUTPUT_NAME "adbc_driver_odbc")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(adbc_driver_odbc PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS adbc_driver_odbc LIBRARY DESTINATION lib)

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
  target_link_libraries(adbc_odbc_test_driver PRIVATE ${CMAKE_DL_LIBS})
  add_dependencies(adbc_odbc_test_driver adbc_driver_odbc)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(adbc_odbc_test_driver PRIVATE -Wall -Wextra)
  endif()
  if(ADBC_ODBC_ASAN)
    target_compile_definitions(adbc_odbc_test_driver PRIVATE ADBC_ODBC_TEST_ASAN)
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
