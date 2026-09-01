# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(ossie
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# tpcds is a TEST dependency only -- nothing in src/ references it. The differential tests call
# dsdgen to generate TPC-DS data, then diff this extension's generated SQL against hand-written
# TPC-DS SQL over that data; that is the only layer that can catch a wrong *number* rather than
# merely wrong-looking SQL. It is not linked into the shipped extension.
duckdb_extension_load(tpcds)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)