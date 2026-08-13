# Registration entrypoints stay at the extension root.
set(ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_extension.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_lake_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_lake_scan.cpp
)

foreach(ROOT4DUCKDB_MODULE IN ITEMS core reader serialized direct index dataset histogram iceberg)
    add_subdirectory(${ROOT4DUCKDB_MODULE})
endforeach()

set(EXTENSION_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
set(ROOT4DUCKDB_ICEBERG_SOURCES ${ROOT4DUCKDB_ICEBERG_SOURCES} PARENT_SCOPE)
