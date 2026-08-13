cmake_minimum_required(VERSION 3.25)

project(root4duckdb LANGUAGES CXX)

set(TARGET_NAME root)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

option(ROOT4DUCKDB_LOADABLE_ONLY
       "Build root as an isolated loadable extension" OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootCompiler.cmake")

add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(root_loadable_extension_smoke EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/test/loadable_extension_smoke.cpp
    )
    target_link_libraries(root_loadable_extension_smoke PRIVATE duckdb_static)
    target_compile_features(root_loadable_extension_smoke PRIVATE cxx_std_17)
    set_target_properties(root_loadable_extension_smoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension/root"
    )
endif()

foreach(ROOT4DUCKDB_TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    root4duckdb_configure_target(${ROOT4DUCKDB_TARGET})
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootIceberg.cmake")

root4duckdb_use_shared_iceberg(${EXTENSION_NAME})

if(ROOT4DUCKDB_LOADABLE_ONLY)
    root4duckdb_embed_static_iceberg(${LOADABLE_EXTENSION_NAME})
else()
    root4duckdb_use_shared_iceberg(${LOADABLE_EXTENSION_NAME})
endif()

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
