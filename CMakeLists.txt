cmake_minimum_required(VERSION 3.25)

project(root4duckdb LANGUAGES C CXX)

set(TARGET_NAME root)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

option(ROOT4DUCKDB_LOADABLE_ONLY
       "Build root as an isolated loadable extension" OFF)
set(_root4duckdb_runtime_bootstrap_default OFF)
if(DEFINED ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP} AND
   NOT "$ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP}" STREQUAL "" AND
   NOT "$ENV{ROOT4DUCKDB_RUNTIME_BOOTSTRAP}" STREQUAL "0")
    set(_root4duckdb_runtime_bootstrap_default ON)
endif()

option(ROOT4DUCKDB_RUNTIME_BOOTSTRAP
       "Build a one-file ROOT runtime-discovering bootstrap extension"
       ${_root4duckdb_runtime_bootstrap_default})

unset(_root4duckdb_runtime_bootstrap_default)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootCompiler.cmake")

add_subdirectory(src)

# Bootstrap is registered in src/core for source ownership, but it is not part
# of the ROOT-linked implementation payload. It is compiled only by the public
# bootstrap loadable-extension target below.
list(REMOVE_ITEM EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/root_bootstrap.c"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootIceberg.cmake")

# Keep the bundled/static DuckDB build unchanged.
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
root4duckdb_configure_target(${EXTENSION_NAME})
root4duckdb_use_shared_iceberg(${EXTENSION_NAME})

if(ROOT4DUCKDB_RUNTIME_BOOTSTRAP)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RootBootstrap.cmake")
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
    # DuckDB 2.0's static archive includes its linked extension registry.
    # The smoke host explicitly LOADs the release artifact.
    target_link_libraries(root_loadable_extension_smoke PRIVATE
        duckdb_static
    )
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



# Optional GNU static-libstdc++ payload experiment; ROOT and host ABI constraints remain.
option(ROOT4DUCKDB_PAYLOAD_STATIC_LIBSTDCXX
       "Experiment: link GNU libstdc++ statically into ROOT payload"
       OFF)

if(ROOT4DUCKDB_PAYLOAD_STATIC_LIBSTDCXX)
    if(NOT ROOT4DUCKDB_RUNTIME_BOOTSTRAP OR
       NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "ROOT4DUCKDB_PAYLOAD_STATIC_LIBSTDCXX requires Linux, GCC, "
            "and ROOT4DUCKDB_RUNTIME_BOOTSTRAP=ON")
    endif()

    # GCC driver option: keep libgcc_s dynamic so the system's exception
    # unwinder remains shared. Static libstdc++ is an ABI experiment only.
    target_link_options(root_payload_loadable_extension
        PRIVATE
        "-static-libstdc++"
    )

    message(STATUS
        "[EXPERIMENT] Static libstdc++ enabled for embedded ROOT payload")
endif()

# Resolve circular static archive dependencies inside the payload.
# DuckDB 2.0 embeds its parser and linked extension registry in duckdb_static;
# the former duckdb_pg_query and dummy_static_extension_loader targets are gone.
if(TARGET root_payload_loadable_extension AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(root_payload_loadable_extension
        "-Wl,--start-group"

        "$<TARGET_FILE:duckdb_static>"
        "$<TARGET_FILE:parquet_extension>"
        "$<TARGET_FILE:core_functions_extension>"

        duckdb_fsst
        duckdb_fmt
        duckdb_re2
        duckdb_miniz
        duckdb_utf8proc
        duckdb_hyperloglog
        duckdb_fastpforlib
        duckdb_skiplistlib
        duckdb_mbedtls
        duckdb_yyjson
        duckdb_zstd

        ${ROOT4DUCKDB_ICEBERG_STATIC_LIBRARIES}

        "-Wl,--end-group"
    )

    # Catch unresolved ELF symbols during linking, not at LOAD.
    target_link_options(root_payload_loadable_extension
        PRIVATE
        "LINKER:-z,defs"
    )
endif()
