# Parquet stores the external ROOT physical-index tables.
duckdb_extension_load(parquet)

# ROOT4DuckDB itself.
file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" ROOT4DUCKDB_VERSION)
string(STRIP "${ROOT4DUCKDB_VERSION}" ROOT4DUCKDB_VERSION)

set(ROOT4DUCKDB_LINK_OPTIONS "")
# The runtime bootstrap embeds its own ROOT-linked payload. Linking a second
# static copy of ROOT4DuckDB into the DuckDB host creates duplicate C++ symbols
# and can dispatch serialized scans into a stale implementation.
if(ROOT4DUCKDB_LOADABLE_ONLY OR ROOT4DUCKDB_RUNTIME_BOOTSTRAP OR
   (DEFINED ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP} AND
    NOT "$ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP}" STREQUAL "" AND
    NOT "$ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP}" STREQUAL "0"))
    # DuckDB 2.0 removed DONT_LINK: static linking is now opt-in.
    # Keep the flag for DuckDB 1.x, where it prevents duplicate ROOT symbols.
    if(DUCKDB_MAJOR_VERSION LESS 2)
        list(APPEND ROOT4DUCKDB_LINK_OPTIONS DONT_LINK)
    endif()
endif()

duckdb_extension_load(root
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION "v${ROOT4DUCKDB_VERSION}"
    LOAD_TESTS
    ${ROOT4DUCKDB_LINK_OPTIONS}
)
