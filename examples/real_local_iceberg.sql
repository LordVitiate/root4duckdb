-- One folder, one real local Apache Iceberg SqlCatalog + SQLite warehouse.
SET threads = 8;
SET root_max_in_flight_files = 0; -- derive from threads + root_memory_limit
SET root_memory_limit = '8GB';

SELECT *
FROM root_build_dataset_index(
    '/eos/experiment/compass/data/2022/W08/slot1/mDST/*.root',
    'PaEvent',
    '["/PaEvent/vecTrack/chi2tot", "/PaEvent/vecCaloClus/e"]',
    '/eos/user/s/sseryubi/root4duckdb/catalog',
    dictionary := '/afs/cern.ch/work/s/sseryubi/private/COMPASS/phast.8.026/lib/libPhast.so',
    catalog_mode := 'sqlite'
);

-- Must show six real Iceberg tables, metadata JSON and Iceberg snapshot IDs.
SELECT *
FROM root_iceberg_catalog('/eos/user/s/sseryubi/root4duckdb/catalog')
ORDER BY table_name;

-- Normal SQL predicates replace JSON entry_selection.
SELECT source_id, entry_id, vecTrack_idx, value
FROM read_root_dataset(
    '/eos/user/s/sseryubi/root4duckdb/catalog',
    '/PaEvent/vecTrack/chi2tot',
    dictionary := '/afs/cern.ch/work/s/sseryubi/private/COMPASS/phast.8.026/lib/libPhast.so'
)
WHERE source_id = '7bceccc1d6127e64'
  AND entry_id >= 929
  AND entry_id < 934;
