cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME ossie)

# DuckDB's extension distribution supports vcpkg: add dependencies to ./vcpkg.json and pull them in here with
# find_package. duckdb_ossie deliberately uses none -- JSON parsing uses the yyjson DuckDB already vendors, and
# YAML support is a single vendored header (third_party/fkyaml), so keeping vcpkg off the critical path makes the
# build reproducible from a bare checkout.

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
include_directories(third_party)

set(EXTENSION_SOURCES
    src/ossie_extension.cpp
    src/model/parser.cpp
    src/model/validate.cpp
    src/model/graph.cpp
    src/model/catalog.cpp
    src/model/yaml.cpp
    src/compile/compile.cpp
    src/functions/load.cpp
    src/functions/compile.cpp
    src/functions/describe.cpp
    src/functions/query.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
