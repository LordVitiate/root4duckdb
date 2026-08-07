# Parquet stores the external ROOT physical-index tables.
duckdb_extension_load(parquet)

# ROOT4DUCKDB itself.
duckdb_extension_load(root
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
