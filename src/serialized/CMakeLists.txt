list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_codec_utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_nested_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_deep.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_serialized_reader.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
