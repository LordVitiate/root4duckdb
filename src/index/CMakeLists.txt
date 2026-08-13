list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_bloom.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scalar_actual.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_filter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_builder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_metadata.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_pipeline_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_publish.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_bind.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_index_coordinator.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
