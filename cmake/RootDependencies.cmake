# ROOT and header-only dependencies used by the ROOT4DuckDB core.

# ROOT is intentionally discovered through root-config. Some standalone ROOT
# installations do not export a complete set of ROOT::* CMake targets.
find_program(ROOT_CONFIG_EXECUTABLE NAMES root-config)
if(NOT ROOT_CONFIG_EXECUTABLE)
    message(FATAL_ERROR
        "root-config was not found. Source ROOT's thisroot.sh first.")
endif()

foreach(_r4d_root_query prefix incdir libdir libs)
    execute_process(
        COMMAND ${ROOT_CONFIG_EXECUTABLE} --${_r4d_root_query}
        OUTPUT_VARIABLE ROOT4DUCKDB_ROOT_${_r4d_root_query}
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _r4d_root_result
    )
    if(NOT _r4d_root_result EQUAL 0)
        message(FATAL_ERROR
            "root-config --${_r4d_root_query} failed")
    endif()
endforeach()

set(ROOT4DUCKDB_ROOT_PREFIX "${ROOT4DUCKDB_ROOT_prefix}")
set(ROOT4DUCKDB_ROOT_INCDIR "${ROOT4DUCKDB_ROOT_incdir}")
set(ROOT4DUCKDB_ROOT_LIBDIR "${ROOT4DUCKDB_ROOT_libdir}")
set(ROOT4DUCKDB_ROOT_LIBS_RAW "${ROOT4DUCKDB_ROOT_libs}")

execute_process(
    COMMAND ${ROOT_CONFIG_EXECUTABLE} --ldflags
    OUTPUT_VARIABLE ROOT4DUCKDB_ROOT_LDFLAGS_RAW
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT IS_DIRECTORY "${ROOT4DUCKDB_ROOT_INCDIR}")
    message(FATAL_ERROR
        "Invalid ROOT include directory: ${ROOT4DUCKDB_ROOT_INCDIR}")
endif()
if(NOT IS_DIRECTORY "${ROOT4DUCKDB_ROOT_LIBDIR}")
    message(FATAL_ERROR
        "Invalid ROOT library directory: ${ROOT4DUCKDB_ROOT_LIBDIR}")
endif()
if(ROOT4DUCKDB_ROOT_LIBS_RAW STREQUAL "")
    message(FATAL_ERROR "root-config --libs returned an empty result")
endif()

# Preserve ROOT's link order without embedding its machine-specific RPATH.
separate_arguments(
    _root4duckdb_root_link_items
    UNIX_COMMAND
    "${ROOT4DUCKDB_ROOT_LIBS_RAW} ${ROOT4DUCKDB_ROOT_LDFLAGS_RAW}"
)

set(ROOT4DUCKDB_ROOT_LINK_ITEMS "")
set(_root4duckdb_skip_rpath_value FALSE)

foreach(_root4duckdb_root_link_item IN LISTS _root4duckdb_root_link_items)
    if(_root4duckdb_skip_rpath_value)
        set(_root4duckdb_skip_rpath_value FALSE)
    elseif(_root4duckdb_root_link_item STREQUAL "-Wl,-rpath" OR
           _root4duckdb_root_link_item STREQUAL "-Wl,--rpath" OR
           _root4duckdb_root_link_item STREQUAL "-Wl,-R" OR
           _root4duckdb_root_link_item STREQUAL "-R")
        set(_root4duckdb_skip_rpath_value TRUE)
    elseif(_root4duckdb_root_link_item MATCHES
           "^-Wl,(-rpath|--rpath|-R)(,|=|/)")
        continue()
    elseif(_root4duckdb_root_link_item MATCHES "^-R.+")
        continue()
    else()
        list(APPEND ROOT4DUCKDB_ROOT_LINK_ITEMS
             "${_root4duckdb_root_link_item}")
    endif()
endforeach()

unset(_root4duckdb_root_link_items)
unset(_root4duckdb_root_link_item)
unset(_root4duckdb_skip_rpath_value)

find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
    get_target_property(
        ROOT4DUCKDB_NLOHMANN_INCLUDE_DIRS
        nlohmann_json::nlohmann_json
        INTERFACE_INCLUDE_DIRECTORIES
    )
else()
    find_path(
        ROOT4DUCKDB_NLOHMANN_INCLUDE_DIRS
        nlohmann/json.hpp
    )
endif()

if(NOT ROOT4DUCKDB_NLOHMANN_INCLUDE_DIRS)
    message(FATAL_ERROR
        "nlohmann/json.hpp was not found. Install nlohmann-json3-dev or set "
        "CMAKE_PREFIX_PATH to an nlohmann_json package.")
endif()

message(STATUS "ROOT prefix: ${ROOT4DUCKDB_ROOT_PREFIX}")
message(STATUS "ROOT include: ${ROOT4DUCKDB_ROOT_INCDIR}")
message(STATUS "ROOT libdir: ${ROOT4DUCKDB_ROOT_LIBDIR}")
