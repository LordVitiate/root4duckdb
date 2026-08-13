list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_histogram_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_histogram_schema.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_histogram_materializer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_histogram_row.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
