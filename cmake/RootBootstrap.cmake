if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "ROOT4DUCKDB_RUNTIME_BOOTSTRAP is supported only on Linux")
endif()

# Embed a ROOT-linked payload without DuckDB's automatic extension dependency graph.
set(_root4duckdb_saved_payload_static_build "${EXTENSION_STATIC_BUILD}")
set(EXTENSION_STATIC_BUILD OFF)

build_loadable_extension(root_payload "" ${EXTENSION_SOURCES})
set(ROOT4DUCKDB_PAYLOAD_TARGET root_payload_loadable_extension)
target_compile_definitions(${ROOT4DUCKDB_PAYLOAD_TARGET} PRIVATE ROOT4DUCKDB_PAYLOAD_ONLY=1)

root4duckdb_configure_target(${ROOT4DUCKDB_PAYLOAD_TARGET})

# DuckDB hosts may already expose symbols from a statically linked root extension.
# Keep payload-local C++ helpers private so dlopen never binds them to stale host copies.
# Explicit DUCKDB_EXTENSION_API entrypoints remain publicly exported for dlsym.
set_target_properties(${ROOT4DUCKDB_PAYLOAD_TARGET} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# Host DuckDB resolves payload API symbols; Iceberg stays inside the payload.
root4duckdb_embed_static_iceberg(
    ${ROOT4DUCKDB_PAYLOAD_TARGET}
    ALLOW_HOST_DUCKDB_SYMBOLS
)

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

# Load ROOT before the embedded payload; keep the public bootstrap ROOT-free.
set(_root4duckdb_saved_extension_static_build "${EXTENSION_STATIC_BUILD}")
set(EXTENSION_STATIC_BUILD OFF)

build_loadable_extension_capi(${TARGET_NAME} 1 5 6 ""
    src/core/root_bootstrap.c
    "${ROOT4DUCKDB_PAYLOAD_OBJECT}"
)
set(EXTENSION_STATIC_BUILD "${_root4duckdb_saved_extension_static_build}")
unset(_root4duckdb_saved_extension_static_build)

target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

set_target_properties(${LOADABLE_EXTENSION_NAME} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)

target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE
    "$<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion>"
)
# Embed compatibility metadata only. Never embed an installation path.
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE
    ROOT4DUCKDB_BUILD_ROOT_VERSION="${ROOT4DUCKDB_ROOT_VERSION}"
)
# DuckDB's extension macro uses the plain target_link_libraries signature.
target_link_libraries(${LOADABLE_EXTENSION_NAME}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)
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
