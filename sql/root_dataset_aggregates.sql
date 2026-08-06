-- Exact metadata aggregate helpers. These never open ROOT files.
CREATE OR REPLACE MACRO root_dataset_count(catalog, logical_path) AS TABLE
SELECT row_count FROM root_dataset_stats(catalog, logical_path);

CREATE OR REPLACE MACRO root_dataset_non_null_count(catalog, logical_path) AS TABLE
SELECT non_null_count FROM root_dataset_stats(catalog, logical_path);

CREATE OR REPLACE MACRO root_dataset_min(catalog, logical_path) AS TABLE
SELECT min_value FROM root_dataset_stats(catalog, logical_path);

CREATE OR REPLACE MACRO root_dataset_max(catalog, logical_path) AS TABLE
SELECT max_value FROM root_dataset_stats(catalog, logical_path);
