cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME root)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(root4duckdb LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootDependencies.cmake")

set(ROOT4DUCKDB_ICEBERG_SOURCES
    src/iceberg/root_iceberg_common.cpp
    src/iceberg/root_iceberg_publish.cpp
    src/iceberg/root_iceberg_scan.cpp
)

set(EXTENSION_SOURCES
    src/root_extension.cpp
    src/root_meta_generator.cpp
    src/root_scan.cpp
    src/root_debug.cpp
    src/root_bloom.cpp
    src/root_branch_projection.cpp
    src/root_filter.cpp
    src/root_index_metadata.cpp
    src/root_lake_index.cpp
    src/root_lake_scan.cpp
    src/root_runtime_settings.cpp
    src/root_serialized_plan.cpp
    src/root_serialized_deep.cpp
    src/root_serialized_reader.cpp
    src/root_serialized_codec.cpp
    src/root_serialized_nested_codec.cpp
    ${ROOT4DUCKDB_ICEBERG_SOURCES}
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})

foreach(ROOT4DUCKDB_TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    target_include_directories(${ROOT4DUCKDB_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${ROOT4DUCKDB_ROOT_INCDIR}
        ${ROOT4DUCKDB_NLOHMANN_INCLUDE_DIRS}
    )

    # DuckDB v1.4.5 injects -std=c++11. Append C++17 for the ROOT reader.
    # Only the small Iceberg adapter sources are compiled as C++23.
    target_compile_features(${ROOT4DUCKDB_TARGET} PRIVATE cxx_std_17)
    target_compile_options(${ROOT4DUCKDB_TARGET} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
    )
    target_compile_definitions(${ROOT4DUCKDB_TARGET} PRIVATE
        ROOT4DUCKDB_LAKEHOUSE=1
    )

    # DuckDB creates extension targets with the plain target_link_libraries
    # signature, so every dependency below intentionally uses that signature.
    target_link_libraries(${ROOT4DUCKDB_TARGET}
        ${ROOT4DUCKDB_ROOT_LINK_ITEMS}
    )
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
