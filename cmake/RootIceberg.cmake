# Use shared Iceberg for the bundled DuckDB CLI and static Iceberg for the
# independently loadable extension. This avoids duplicate dependency symbols
# in DuckDB while keeping the release extension self-contained.

set(ROOT4DUCKDB_ICEBERG_PREFIX "" CACHE PATH
    "Prefix produced by build-iceberg.sh")
if(ROOT4DUCKDB_ICEBERG_PREFIX STREQUAL "")
    if(DEFINED ENV{ROOT4DUCKDB_ICEBERG_PREFIX} AND
       NOT "$ENV{ROOT4DUCKDB_ICEBERG_PREFIX}" STREQUAL "")
        set(ROOT4DUCKDB_ICEBERG_PREFIX "$ENV{ROOT4DUCKDB_ICEBERG_PREFIX}")
    else()
        set(ROOT4DUCKDB_ICEBERG_PREFIX
            "${CMAKE_CURRENT_SOURCE_DIR}/.deps/iceberg-cpp")
    endif()
endif()

get_filename_component(
    ROOT4DUCKDB_ICEBERG_PREFIX
    "${ROOT4DUCKDB_ICEBERG_PREFIX}"
    ABSOLUTE
)

set(ROOT4DUCKDB_ICEBERG_INCLUDE_DIR
    "${ROOT4DUCKDB_ICEBERG_PREFIX}/include")

if(NOT EXISTS "${ROOT4DUCKDB_ICEBERG_INCLUDE_DIR}/iceberg/table.h")
    message(FATAL_ERROR
        "Apache Iceberg C++ is not installed below "
        "${ROOT4DUCKDB_ICEBERG_PREFIX}. Run ./build-iceberg.sh first.")
endif()

set(_r4d_iceberg_library_dirs
    "${ROOT4DUCKDB_ICEBERG_PREFIX}/lib"
    "${ROOT4DUCKDB_ICEBERG_PREFIX}/lib64"
)

function(_root4duckdb_find_iceberg_shared out_var library_name)
    unset(_r4d_shared_library CACHE)

    find_library(
        _r4d_shared_library
        NAMES ${library_name}
        PATHS ${_r4d_iceberg_library_dirs}
        NO_DEFAULT_PATH
    )

    if(NOT _r4d_shared_library)
        message(FATAL_ERROR
            "Missing shared library "
            "lib${library_name}${CMAKE_SHARED_LIBRARY_SUFFIX} below "
            "${ROOT4DUCKDB_ICEBERG_PREFIX}. "
            "Re-run ./build-iceberg.sh --clean.")
    endif()

    get_filename_component(
        _r4d_shared_library_real
        "${_r4d_shared_library}"
        REALPATH
    )

    if(_r4d_shared_library_real MATCHES
       "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$")
        message(FATAL_ERROR
            "Static Iceberg archive selected unexpectedly: "
            "${_r4d_shared_library_real}")
    endif()

    set(${out_var} "${_r4d_shared_library}" PARENT_SCOPE)
    unset(_r4d_shared_library CACHE)
endfunction()

_root4duckdb_find_iceberg_shared(
    ROOT4DUCKDB_ICEBERG_CORE
    iceberg
)
_root4duckdb_find_iceberg_shared(
    ROOT4DUCKDB_ICEBERG_DATA
    iceberg_data
)
_root4duckdb_find_iceberg_shared(
    ROOT4DUCKDB_ICEBERG_BUNDLE
    iceberg_bundle
)
_root4duckdb_find_iceberg_shared(
    ROOT4DUCKDB_ICEBERG_SQL
    iceberg_sql_catalog
)

set(ROOT4DUCKDB_ICEBERG_SHARED_LIBRARIES
    ${ROOT4DUCKDB_ICEBERG_CORE}
    ${ROOT4DUCKDB_ICEBERG_DATA}
    ${ROOT4DUCKDB_ICEBERG_BUNDLE}
    ${ROOT4DUCKDB_ICEBERG_SQL}
)

find_package(
    iceberg
    CONFIG
    REQUIRED
    COMPONENTS bundle sql_catalog
    PATHS "${ROOT4DUCKDB_ICEBERG_PREFIX}"
    NO_DEFAULT_PATH
)

set(ROOT4DUCKDB_ICEBERG_STATIC_LIBRARIES
    iceberg::iceberg_bundle_static
    iceberg::iceberg_sql_catalog_static
)

set(_r4d_iceberg_runtime_dirs "")
foreach(_r4d_iceberg_library
        IN LISTS ROOT4DUCKDB_ICEBERG_SHARED_LIBRARIES)
    get_filename_component(
        _r4d_iceberg_library_dir
        "${_r4d_iceberg_library}"
        DIRECTORY
    )
    list(APPEND
        _r4d_iceberg_runtime_dirs
        "${_r4d_iceberg_library_dir}"
    )
endforeach()
list(REMOVE_DUPLICATES _r4d_iceberg_runtime_dirs)

foreach(_r4d_iceberg_source IN LISTS ROOT4DUCKDB_ICEBERG_SOURCES)
    set_property(
        SOURCE "${_r4d_iceberg_source}"
        PROPERTY SKIP_PRECOMPILE_HEADERS ON
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_property(
            SOURCE "${_r4d_iceberg_source}"
            APPEND
            PROPERTY COMPILE_OPTIONS "-std=c++23"
        )
    elseif(MSVC)
        set_property(
            SOURCE "${_r4d_iceberg_source}"
            APPEND
            PROPERTY COMPILE_OPTIONS "/std:c++latest"
        )
    endif()
endforeach()

function(root4duckdb_use_shared_iceberg target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "Unknown ROOT4DuckDB target: ${target_name}")
    endif()

    target_include_directories(
        ${target_name}
        PRIVATE
        ${ROOT4DUCKDB_ICEBERG_INCLUDE_DIR}
    )

    # DuckDB creates extension targets with the plain signature.
    target_link_libraries(
        ${target_name}
        ${ROOT4DUCKDB_ICEBERG_SHARED_LIBRARIES}
    )

    # Keep bundled build artifacts directly runnable.
    set_property(
        TARGET ${target_name}
        APPEND
        PROPERTY BUILD_RPATH "${_r4d_iceberg_runtime_dirs}"
    )
    set_property(
        TARGET ${target_name}
        APPEND
        PROPERTY INSTALL_RPATH "${_r4d_iceberg_runtime_dirs}"
    )
endfunction()

function(root4duckdb_embed_static_iceberg target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR
            "Unknown ROOT4DuckDB target: ${target_name}")
    endif()

    target_include_directories(
        ${target_name}
        PRIVATE
        ${ROOT4DUCKDB_ICEBERG_INCLUDE_DIR}
    )

    # DuckDB creates extension targets with the plain signature.
    target_link_libraries(
        ${target_name}
        ${ROOT4DUCKDB_ICEBERG_STATIC_LIBRARIES}
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(
            ${target_name}
            PRIVATE
            "LINKER:--as-needed"
            "LINKER:--exclude-libs,ALL"
            "LINKER:-Bsymbolic-functions"
            "LINKER:-z,defs"
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/root_extension.exports.map"
        )
    endif()

    # Do not retain paths from the release build host.
    set_target_properties(
        ${target_name}
        PROPERTIES
        BUILD_RPATH ""
        INSTALL_RPATH ""
        SKIP_BUILD_RPATH TRUE
    )
endfunction()

message(STATUS
    "Iceberg prefix: ${ROOT4DUCKDB_ICEBERG_PREFIX}")
message(STATUS
    "Shared Iceberg libraries: "
    "${ROOT4DUCKDB_ICEBERG_SHARED_LIBRARIES}")
message(STATUS
    "Static Iceberg libraries: "
    "${ROOT4DUCKDB_ICEBERG_STATIC_LIBRARIES}")
