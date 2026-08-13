# Applies one compiler and dependency policy to both DuckDB extension targets.
function(root4duckdb_configure_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        ${ROOT4DUCKDB_ROOT_INCDIR}
        ${ROOT4DUCKDB_NLOHMANN_INCLUDE_DIRS}
    )

    target_compile_features(${target_name} PRIVATE cxx_std_17)
    target_compile_options(${target_name} PRIVATE
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;-Wnon-virtual-dtor>"
    )
    target_compile_definitions(${target_name} PRIVATE ROOT4DUCKDB_LAKEHOUSE=1)
    target_precompile_headers(${target_name} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/include/root4duckdb/root_pch.hpp>"
    )

    # DuckDB creates extension targets with the plain signature.
    target_link_libraries(${target_name} ${ROOT4DUCKDB_ROOT_LINK_ITEMS})
endfunction()
