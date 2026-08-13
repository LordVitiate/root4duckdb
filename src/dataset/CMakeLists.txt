list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_dataset_catalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_path_predicates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_dataset_predicates.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_entry_selection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_dataset_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_dataset_execute.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
