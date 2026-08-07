-- Native Iceberg publication is performed by the single-committer script:
-- scripts/production/commit_iceberg.sh
--
-- After commit, attach the same catalog and query using its catalog prefix:

SELECT count(*)
FROM read_root_dataset(
    'compass_iceberg.root_index.compass2022_w08',
    '/PaEvent/vecParticle/flags',
    dictionary := '/data/libPhast.so'
);
