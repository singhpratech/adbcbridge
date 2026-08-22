cmake_minimum_required(VERSION 3.16)
project(adbc_driver_odbc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ADBC_ODBC_BUILD_SHARED "Build shared library" ON)

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
