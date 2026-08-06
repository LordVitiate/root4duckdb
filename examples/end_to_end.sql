LOAD parquet;
LOAD root;

SELECT *
FROM root_build_dataset_index(
    '@/data/chunk.uris',
    'PaEvent',
    '["/PaEvent/vecParticle/flags","/PaEvent/vecParticle/indTrack"]',
    '/data/index/chunk-0001',
    dictionary := '/data/libPhast.so',
    dictionary_cleanup := 'retain',
    index_threads := 8,
    -- Concurrency and memory are derived from DuckDB/node resources. Optional
    -- named limits can still be supplied as explicit upper bounds.
    bloom_false_positive_rate := 0.01,
    catalog_mode := 'local',
    allow_partial := false
);

SELECT event_fk, vecParticle_idx, value
FROM read_root_dataset(
    '/data/index/chunk-0001',
    '/PaEvent/vecParticle/flags',
    dictionary := '/data/libPhast.so'
)
WHERE event_fk < 5000
LIMIT 100;
