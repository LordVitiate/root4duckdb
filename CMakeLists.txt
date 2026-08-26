cmake_minimum_required(VERSION 3.25)

project(root4duckdb LANGUAGES CXX)

set(TARGET_NAME root)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

option(ROOT4DUCKDB_LOADABLE_ONLY
       "Build root as an isolated loadable extension" OFF)
set(_root4duckdb_cern_bootstrap_default OFF)
if(DEFINED ENV{ROOT4DUCKDB_CERN_BOOTSTRAP} AND
   NOT "$ENV{ROOT4DUCKDB_CERN_BOOTSTRAP}" STREQUAL "" AND
   NOT "$ENV{ROOT4DUCKDB_CERN_BOOTSTRAP}" STREQUAL "0")
    set(_root4duckdb_cern_bootstrap_default ON)
endif()
option(ROOT4DUCKDB_CERN_BOOTSTRAP
       "Build the loadable extension as a one-file CERN ROOT bootstrap"
       ${_root4duckdb_cern_bootstrap_default})
unset(_root4duckdb_cern_bootstrap_default)

set(_root4duckdb_cern_lcg_view_default
    "/cvmfs/sft.cern.ch/lcg/views/LCG_110/x86_64-el9-gcc14-opt")
if(DEFINED ENV{ROOT4DUCKDB_CERN_LCG_VIEW} AND
   NOT "$ENV{ROOT4DUCKDB_CERN_LCG_VIEW}" STREQUAL "")
    set(_root4duckdb_cern_lcg_view_default
        "$ENV{ROOT4DUCKDB_CERN_LCG_VIEW}")
endif()
set(ROOT4DUCKDB_CERN_LCG_VIEW
    "${_root4duckdb_cern_lcg_view_default}"
    CACHE PATH "CERN LCG view used by the one-file ROOT bootstrap")
unset(_root4duckdb_cern_lcg_view_default)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootCompiler.cmake")

add_subdirectory(src)

# Bootstrap is registered in src/core for source ownership, but it is not part
# of the ROOT-linked implementation payload. It is compiled only by the public
# bootstrap loadable-extension target below.
list(REMOVE_ITEM EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/root_bootstrap.cpp"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootIceberg.cmake")

# Keep the bundled/static DuckDB build unchanged.
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
root4duckdb_configure_target(${EXTENSION_NAME})
root4duckdb_use_shared_iceberg(${EXTENSION_NAME})

if(ROOT4DUCKDB_CERN_BOOTSTRAP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ROOT4DUCKDB_CERN_BOOTSTRAP is supported only on Linux")
    endif()

    # Build the normal ROOT-dependent implementation as an internal DuckDB
    # extension ELF.  It is never distributed as a separate file: the binary is
    # embedded below into the public root.duckdb_extension.
    # The embedded payload is a real loadable ELF. It must not link
    # duckdb_static, because that would pull the statically bundled root
    # extension and its shared Iceberg dependencies back into the payload.
    # Build the embedded payload without DuckDB's automatic static-extension
    # dependency graph. We need DuckDB core itself, but not the root/parquet/etc.
    # extensions attached to the duckdb_static CMake target through
    # link_extension_libraries(duckdb_static PUBLIC).
    set(_root4duckdb_saved_payload_static_build "${EXTENSION_STATIC_BUILD}")
    set(EXTENSION_STATIC_BUILD OFF)

    build_loadable_extension(root_payload "" ${EXTENSION_SOURCES})
    set(ROOT4DUCKDB_PAYLOAD_TARGET root_payload_loadable_extension)

    root4duckdb_configure_target(${ROOT4DUCKDB_PAYLOAD_TARGET})

    # Link the raw DuckDB archive by filename. Do NOT link the CMake target
    # "duckdb_static": its PUBLIC interface contains all statically configured
    # DuckDB extensions, including root_extension and its shared Iceberg graph.
    add_dependencies(${ROOT4DUCKDB_PAYLOAD_TARGET}
        duckdb_static
        parquet_extension
        core_functions_extension
        jemalloc_extension
    )

    target_link_libraries(${ROOT4DUCKDB_PAYLOAD_TARGET}
        "$<TARGET_FILE:duckdb_static>"
        "$<TARGET_FILE:parquet_extension>"
        "$<TARGET_FILE:core_functions_extension>"
        "$<TARGET_FILE:jemalloc_extension>"

        duckdb_fsst
        duckdb_fmt
        duckdb_pg_query
        duckdb_re2
        duckdb_miniz
        duckdb_utf8proc
        duckdb_hyperloglog
        duckdb_fastpforlib
        duckdb_skiplistlib
        duckdb_mbedtls
        duckdb_yyjson
        duckdb_zstd

        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    # Iceberg remains entirely inside the payload.
    root4duckdb_embed_static_iceberg(${ROOT4DUCKDB_PAYLOAD_TARGET})

    set(EXTENSION_STATIC_BUILD "${_root4duckdb_saved_payload_static_build}")
    unset(_root4duckdb_saved_payload_static_build)

    set(ROOT4DUCKDB_PAYLOAD_BIN
        "${CMAKE_CURRENT_BINARY_DIR}/root4duckdb_payload.bin")
    set(ROOT4DUCKDB_PAYLOAD_OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/root4duckdb_payload_blob.o")

    add_custom_command(
        OUTPUT "${ROOT4DUCKDB_PAYLOAD_OBJECT}"
        COMMAND ${CMAKE_COMMAND} -E copy
                $<TARGET_FILE:${ROOT4DUCKDB_PAYLOAD_TARGET}>
                "${ROOT4DUCKDB_PAYLOAD_BIN}"
        COMMAND ${CMAKE_LINKER} -r -b binary
                root4duckdb_payload.bin
                -o root4duckdb_payload_blob.o
        DEPENDS ${ROOT4DUCKDB_PAYLOAD_TARGET}
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Embedding ROOT4DuckDB ROOT-linked payload"
        VERBATIM
    )
    set_source_files_properties(
        "${ROOT4DUCKDB_PAYLOAD_OBJECT}"
        PROPERTIES
            GENERATED TRUE
            EXTERNAL_OBJECT TRUE
    )

    # The public extension is deliberately ROOT-free.  It first selects one
    # CERN ROOT installation, loads that runtime, and only then dlopen()s the
    # embedded payload from memfd.
    # The public bootstrap must not link duckdb_static: that target carries the
    # statically linked root_extension back into this ELF and defeats controlled
    # ROOT loading. DuckDB CLI resolves the bootstrap DuckDB symbols at dlopen.
    set(_root4duckdb_saved_extension_static_build "${EXTENSION_STATIC_BUILD}")
    set(EXTENSION_STATIC_BUILD OFF)

    build_loadable_extension(${TARGET_NAME} ""
        src/core/root_bootstrap.cpp
        "${ROOT4DUCKDB_PAYLOAD_OBJECT}"
    )
    set(EXTENSION_STATIC_BUILD "${_root4duckdb_saved_extension_static_build}")
    unset(_root4duckdb_saved_extension_static_build)

    target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
    )
    target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
    )
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE
        ROOT4DUCKDB_CERN_LCG_VIEW="${ROOT4DUCKDB_CERN_LCG_VIEW}"
    )
    # DuckDB's extension macro uses the plain target_link_libraries signature.
    target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CMAKE_DL_LIBS})
    set(ROOT4DUCKDB_BOOTSTRAP_VERSION_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/root_bootstrap.map"
    )

    set_property(
        TARGET ${LOADABLE_EXTENSION_NAME}
        APPEND PROPERTY LINK_DEPENDS
        "${ROOT4DUCKDB_BOOTSTRAP_VERSION_SCRIPT}"
    )

    target_link_options(${LOADABLE_EXTENSION_NAME} PRIVATE
        "LINKER:-z,noexecstack"
        "LINKER:--version-script=${ROOT4DUCKDB_BOOTSTRAP_VERSION_SCRIPT}"
    )
else()
    build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES})
    root4duckdb_configure_target(${LOADABLE_EXTENSION_NAME})

    if(ROOT4DUCKDB_LOADABLE_ONLY)
        root4duckdb_embed_static_iceberg(${LOADABLE_EXTENSION_NAME})
    else()
        root4duckdb_use_shared_iceberg(${LOADABLE_EXTENSION_NAME})
    endif()
endif()

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

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
