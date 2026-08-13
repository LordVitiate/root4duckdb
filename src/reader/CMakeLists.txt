list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_semantic_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_primitive_value.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_offset_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_path_resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_object_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_branch_resolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_branch_projection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_path_reader.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
