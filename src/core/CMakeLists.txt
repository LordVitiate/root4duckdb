list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_debug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_dictionary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_file_opener.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_input_resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_lake_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_runtime_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_bootstrap.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
