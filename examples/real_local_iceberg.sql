-- One folder, one real local Apache Iceberg SqlCatalog + SQLite warehouse.
SET threads = 8;
SET root_max_in_flight_files = 0; -- derive from threads + root_memory_limit
SET root_memory_limit = '8GB';

SELECT *
FROM root_build_dataset_index(
    '/data/events/*.root',
    'PaEvent',
    '["/PaEvent/vecTrack/chi2tot", "/PaEvent/vecCaloClus/e"]',
    '/data/root4duckdb/catalog',
    dictionary := '/data/libExperiment.so',
    catalog_mode := 'sqlite'
);

-- Must show six real Iceberg tables, metadata JSON and Iceberg snapshot IDs.
SELECT *
FROM root_iceberg_catalog('/data/root4duckdb/catalog')
ORDER BY table_name;

-- Normal SQL predicates replace JSON entry_selection.
SELECT source_id, entry_id, vecTrack_idx, value
FROM read_root_dataset(
    '/data/root4duckdb/catalog',
    '/PaEvent/vecTrack/chi2tot',
    dictionary := '/data/libExperiment.so'
)
WHERE entry_id < 10;
