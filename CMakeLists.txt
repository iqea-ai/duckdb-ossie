cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME ossie)

# JSON parsing uses the yyjson DuckDB already vendors. YAML support needs a parser DuckDB does not ship, so
# fkYAML is pulled in through vcpkg (see ./vcpkg.json). It is header-only, so this costs a resolve step at
# configure time rather than a link-time dependency.

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

find_package(fkYAML CONFIG REQUIRED)

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

target_link_libraries(${EXTENSION_NAME} fkYAML::fkYAML)
target_link_libraries(${LOADABLE_EXTENSION_NAME} fkYAML::fkYAML)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
