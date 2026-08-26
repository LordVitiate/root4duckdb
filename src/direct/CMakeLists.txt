list(APPEND ROOT4DUCKDB_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/root_direct_scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_describe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_bind.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_filter_pruning.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_special_modes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_modes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/root_scan_execute.cpp
)
set(ROOT4DUCKDB_SOURCES ${ROOT4DUCKDB_SOURCES} PARENT_SCOPE)
