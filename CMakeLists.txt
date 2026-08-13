cmake_minimum_required(VERSION 3.25)

project(root4duckdb LANGUAGES CXX)

set(TARGET_NAME root)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootCompiler.cmake")
add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

foreach(ROOT4DUCKDB_TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    root4duckdb_configure_target(${ROOT4DUCKDB_TARGET})
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootIceberg.cmake")
root4duckdb_use_shared_iceberg(${EXTENSION_NAME})
root4duckdb_use_shared_iceberg(${LOADABLE_EXTENSION_NAME})

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
