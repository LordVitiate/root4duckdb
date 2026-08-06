#!/usr/bin/env bash
set -euo pipefail

DUCKDB_BIN=""
ATTACH_SQL=""
CATALOG=""
NAMESPACE=""
PREFIX="root4duckdb"
PLAN=""
CHUNKS_DIR=""
STATUS=""

while (($#)); do
    case "$1" in
        --duckdb) DUCKDB_BIN="$2"; shift 2 ;;
        --attach-sql) ATTACH_SQL="$2"; shift 2 ;;
        --catalog) CATALOG="$2"; shift 2 ;;
        --namespace) NAMESPACE="$2"; shift 2 ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --plan) PLAN="$2"; shift 2 ;;
        --chunks-dir) CHUNKS_DIR="$2"; shift 2 ;;
        --status) STATUS="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -x "$DUCKDB_BIN" && -f "$ATTACH_SQL" && -f "$PLAN" && -d "$CHUNKS_DIR" ]] || {
    echo "required: --duckdb --attach-sql --catalog --namespace --plan --chunks-dir" >&2
    exit 2
}
for identifier in "$CATALOG" "$NAMESPACE" "$PREFIX"; do
    [[ "$identifier" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || {
        echo "unsafe SQL identifier: $identifier" >&2
        exit 2
    }
done

STATUS="${STATUS:-commit-status.json}"
VALIDATION="${STATUS%.json}-validation.json"
SQL_FILE="$(mktemp)"
write_status() {
    local state="$1" message="${2:-}"
    python3 - "$STATUS" "$state" "$message" "$PLAN" <<'PY'
import json,os,pathlib,sys,time
path,state,message,plan_path=sys.argv[1:]
plan=json.load(open(plan_path)); target=pathlib.Path(path); target.parent.mkdir(parents=True,exist_ok=True)
payload={"format":"root4duckdb-commit-status-v1","state":state,"message":message,
         "plan_fingerprint":plan["fingerprint"],"updated_at_ns":time.time_ns()}
tmp=target.with_suffix(target.suffix+".tmp"); tmp.write_text(json.dumps(payload,indent=2,sort_keys=True)+"\n")
os.replace(tmp,target)
PY
}
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    rm -f "$SQL_FILE"
    if ((rc != 0)); then write_status failed "committer exit code $rc" || true; fi
    exit "$rc"
}
trap cleanup EXIT INT TERM
write_status running "validating immutable chunks"
python3 "$(dirname "$0")/validate_chunks.py" \
    --plan "$PLAN" --chunks-dir "$CHUNKS_DIR" --output "$VALIDATION"

readarray -t META < <(python3 - "$PLAN" "$VALIDATION" <<'PY'
import hashlib, json, sys, time
plan = json.load(open(sys.argv[1]))
validation = json.load(open(sys.argv[2]))
schemas = validation.get("schema_fingerprints", [])
if not schemas:
    raise SystemExit("validated schema fingerprints are missing")
print(plan["fingerprint"])
print(hashlib.sha256(plan["fingerprint"].encode()).hexdigest()[:16])
print(str(time.time_ns()))
print(len(plan["chunks"]))
print(hashlib.sha256("\n".join(sorted(schemas)).encode()).hexdigest())
PY
)
PLAN_FP="${META[0]}"
DATASET_ID="${META[1]}"
SNAPSHOT_ID="${META[2]}"
CHUNK_COUNT="${META[3]}"
SCHEMA_FP="${META[4]}"

FILES_GLOB="$CHUNKS_DIR/*/snapshots/*/root_files.parquet"
SCHEMAS_GLOB="$CHUNKS_DIR/*/snapshots/*/root_schemas.parquet"
ACCESS_GLOB="$CHUNKS_DIR/*/snapshots/*/root_access_levels.parquet"
BASKETS_GLOB="$CHUNKS_DIR/*/snapshots/*/root_baskets.parquet"
BASE="${CATALOG}.${NAMESPACE}.${PREFIX}"

cat "$ATTACH_SQL" > "$SQL_FILE"
cat >> "$SQL_FILE" <<SQL
LOAD parquet;
LOAD iceberg;
CREATE SCHEMA IF NOT EXISTS ${CATALOG}.${NAMESPACE};

-- These are native Iceberg tables because ${CATALOG} is an attached Iceberg
-- catalog.  The five-table naming matches read_root_dataset(catalog_prefix=...).
CREATE TABLE IF NOT EXISTS ${BASE}_files (
    index_version UINTEGER,
    dataset_id VARCHAR,
    snapshot_id VARCHAR,
    file_id VARCHAR,
    root_uri VARCHAR,
    tree_name VARCHAR,
    schema_id VARCHAR,
    column_id VARCHAR,
    event_base UBIGINT,
    total_entries UBIGINT,
    file_size UBIGINT,
    mtime_ns BIGINT,
    min_value DOUBLE,
    max_value DOUBLE,
    value_count UBIGINT,
    null_count UBIGINT,
    nan_count UBIGINT,
    pos_inf_count UBIGINT,
    neg_inf_count UBIGINT,
    basket_count UBIGINT,
    logical_path VARCHAR,
    year INTEGER,
    period VARCHAR,
    plan_fingerprint VARCHAR
) PARTITIONED BY (year, period, logical_path, bucket(64, file_id));

CREATE TABLE IF NOT EXISTS ${BASE}_schemas (
    index_version UINTEGER,
    schema_id VARCHAR,
    column_id VARCHAR,
    logical_path VARCHAR,
    root_class VARCHAR,
    root_type VARCHAR,
    duckdb_type VARCHAR,
    access_plan_id VARCHAR,
    index_signature VARCHAR,
    container_depth UBIGINT,
    plan_fingerprint VARCHAR
) PARTITIONED BY (logical_path);

CREATE TABLE IF NOT EXISTS ${BASE}_access (
    index_version UINTEGER,
    access_plan_id VARCHAR,
    level_no UBIGINT,
    field_name VARCHAR,
    root_type VARCHAR,
    offset_in_parent BIGINT,
    cumulative_offset BIGINT,
    is_pointer BOOLEAN,
    is_container BOOLEAN,
    is_primitive BOOLEAN,
    is_string BOOLEAN,
    is_fixed_array BOOLEAN,
    array_rank UBIGINT,
    array_length UBIGINT,
    array_dimensions VARCHAR,
    element_size UBIGINT,
    plan_fingerprint VARCHAR
) PARTITIONED BY (bucket(64, access_plan_id));

CREATE TABLE IF NOT EXISTS ${BASE}_baskets (
    index_version UINTEGER,
    snapshot_id VARCHAR,
    file_id VARCHAR,
    column_id VARCHAR,
    basket_id UBIGINT,
    basket_branch_name VARCHAR,
    basket_branch_mode VARCHAR,
    entry_begin UBIGINT,
    entry_end UBIGINT,
    event_base UBIGINT,
    flat_value_begin UBIGINT,
    value_count UBIGINT,
    physical_offset UBIGINT,
    key_length UINTEGER,
    compressed_size UINTEGER,
    uncompressed_size UINTEGER,
    compression UINTEGER,
    min_value DOUBLE,
    max_value DOUBLE,
    null_count UBIGINT,
    nan_count UBIGINT,
    pos_inf_count UBIGINT,
    neg_inf_count UBIGINT,
    bloom_filter BLOB,
    logical_path VARCHAR,
    year INTEGER,
    period VARCHAR,
    plan_fingerprint VARCHAR
) PARTITIONED BY (year, period, logical_path, bucket(64, file_id));

CREATE TABLE IF NOT EXISTS ${BASE}_snapshots (
    index_version UINTEGER,
    dataset_id VARCHAR,
    snapshot_id VARCHAR,
    parent_snapshot_id VARCHAR,
    created_at_ns UBIGINT,
    state VARCHAR,
    root_glob VARCHAR,
    tree_name VARCHAR,
    logical_paths VARCHAR,
    files_table VARCHAR,
    schemas_table VARCHAR,
    access_table VARCHAR,
    baskets_table VARCHAR,
    plan_fingerprint VARCHAR,
    schema_fingerprint VARCHAR,
    chunk_count UBIGINT,
    source_file_count UBIGINT
);

CREATE TABLE IF NOT EXISTS ${BASE}_commits (
    snapshot_id VARCHAR,
    plan_fingerprint VARCHAR,
    state VARCHAR,
    committed_at_ns UBIGINT,
    validation_report VARCHAR,
    schema_fingerprint VARCHAR,
    chunk_count UBIGINT,
    source_file_count UBIGINT
);

-- Idempotent schema evolution for catalogs created by older ROOT4DuckDB releases.
-- Iceberg assigns new field IDs; existing data remains readable by name.
ALTER TABLE ${BASE}_files ADD COLUMN IF NOT EXISTS logical_path VARCHAR;
ALTER TABLE ${BASE}_files ADD COLUMN IF NOT EXISTS year INTEGER;
ALTER TABLE ${BASE}_files ADD COLUMN IF NOT EXISTS period VARCHAR;
ALTER TABLE ${BASE}_files ADD COLUMN IF NOT EXISTS plan_fingerprint VARCHAR;
ALTER TABLE ${BASE}_schemas ADD COLUMN IF NOT EXISTS plan_fingerprint VARCHAR;
ALTER TABLE ${BASE}_access ADD COLUMN IF NOT EXISTS plan_fingerprint VARCHAR;
ALTER TABLE ${BASE}_baskets ADD COLUMN IF NOT EXISTS logical_path VARCHAR;
ALTER TABLE ${BASE}_baskets ADD COLUMN IF NOT EXISTS year INTEGER;
ALTER TABLE ${BASE}_baskets ADD COLUMN IF NOT EXISTS period VARCHAR;
ALTER TABLE ${BASE}_baskets ADD COLUMN IF NOT EXISTS plan_fingerprint VARCHAR;
ALTER TABLE ${BASE}_snapshots ADD COLUMN IF NOT EXISTS schema_fingerprint VARCHAR;
ALTER TABLE ${BASE}_snapshots ADD COLUMN IF NOT EXISTS chunk_count UBIGINT;
ALTER TABLE ${BASE}_snapshots ADD COLUMN IF NOT EXISTS source_file_count UBIGINT;

BEGIN TRANSACTION;

SELECT CASE WHEN EXISTS (
    SELECT 1 FROM ${BASE}_commits
    WHERE plan_fingerprint='${PLAN_FP}' AND state='COMMITTED'
) THEN error('plan already committed: ${PLAN_FP}') ELSE true END;

CREATE OR REPLACE TEMP VIEW stage_files_input AS
SELECT * FROM read_parquet('${FILES_GLOB}', union_by_name := true);
CREATE OR REPLACE TEMP VIEW stage_schemas_input AS
SELECT DISTINCT * FROM read_parquet('${SCHEMAS_GLOB}', union_by_name := true);
CREATE OR REPLACE TEMP VIEW stage_access_input AS
SELECT DISTINCT * FROM read_parquet('${ACCESS_GLOB}', union_by_name := true);
CREATE OR REPLACE TEMP VIEW stage_baskets_input AS
SELECT * FROM read_parquet('${BASKETS_GLOB}', union_by_name := true);

-- Recompute a deterministic global event base from the complete validated
-- source set.  Workers never coordinate global offsets.
CREATE OR REPLACE TEMP VIEW file_lineage AS
WITH unique_files AS (
    SELECT file_id, root_uri, max(total_entries)::UBIGINT AS total_entries
    FROM stage_files_input
    GROUP BY file_id, root_uri
)
SELECT file_id,
       root_uri,
       total_entries,
       COALESCE(sum(total_entries) OVER (
           ORDER BY root_uri, file_id
           ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
       ), 0)::UBIGINT AS global_event_base,
       TRY_CAST(regexp_extract(root_uri, '/(20[0-9]{2})/', 1) AS INTEGER) AS year,
       nullif(regexp_extract(root_uri, '/(W[0-9]{2})/', 1), '') AS period
FROM unique_files;

INSERT INTO ${BASE}_schemas BY NAME
SELECT s.*, '${PLAN_FP}' AS plan_fingerprint
FROM stage_schemas_input s
WHERE NOT EXISTS (
    SELECT 1 FROM ${BASE}_schemas old
    WHERE old.schema_id=s.schema_id AND old.column_id=s.column_id
)
ORDER BY s.logical_path, s.schema_id, s.column_id;

INSERT INTO ${BASE}_access BY NAME
SELECT a.index_version, a.access_plan_id, a.level_no, a.field_name, a.root_type,
       a.offset_in_parent, a.cumulative_offset,
       CAST(a.is_pointer AS BOOLEAN), CAST(a.is_container AS BOOLEAN),
       CAST(a.is_primitive AS BOOLEAN), CAST(a.is_string AS BOOLEAN),
       CAST(a.is_fixed_array AS BOOLEAN), a.array_rank, a.array_length,
       a.array_dimensions, a.element_size, '${PLAN_FP}' AS plan_fingerprint
FROM stage_access_input a
WHERE NOT EXISTS (
    SELECT 1 FROM ${BASE}_access old
    WHERE old.access_plan_id=a.access_plan_id AND old.level_no=a.level_no
)
ORDER BY a.access_plan_id, a.level_no;

INSERT INTO ${BASE}_files BY NAME
SELECT f.index_version,
       '${DATASET_ID}' AS dataset_id,
       '${SNAPSHOT_ID}' AS snapshot_id,
       f.file_id, f.root_uri, f.tree_name, f.schema_id, f.column_id,
       l.global_event_base AS event_base,
       f.total_entries, f.file_size, f.mtime_ns,
       f.min_value, f.max_value, f.value_count, f.null_count, f.nan_count,
       f.pos_inf_count, f.neg_inf_count, f.basket_count,
       s.logical_path, l.year, l.period,
       '${PLAN_FP}' AS plan_fingerprint
FROM stage_files_input f
JOIN file_lineage l USING (file_id, root_uri)
JOIN stage_schemas_input s USING (column_id)
ORDER BY s.logical_path, f.file_id;

INSERT INTO ${BASE}_baskets BY NAME
SELECT b.index_version,
       '${SNAPSHOT_ID}' AS snapshot_id,
       b.file_id, b.column_id, b.basket_id,
       b.basket_branch_name, b.basket_branch_mode,
       b.entry_begin, b.entry_end,
       l.global_event_base AS event_base,
       b.flat_value_begin, b.value_count, b.physical_offset,
       b.key_length, b.compressed_size, b.uncompressed_size, b."compression" AS compression,
       b.min_value, b.max_value, b.null_count, b.nan_count,
       b.pos_inf_count, b.neg_inf_count, b.bloom_filter,
       s.logical_path, l.year, l.period,
       '${PLAN_FP}' AS plan_fingerprint
FROM stage_baskets_input b
JOIN (SELECT DISTINCT file_id, root_uri FROM stage_files_input) f USING (file_id)
JOIN file_lineage l USING (file_id, root_uri)
JOIN stage_schemas_input s USING (column_id)
ORDER BY s.logical_path, b.file_id, b.entry_begin, b.basket_id;

-- Validate all data-table writes before publishing the commit marker.
SELECT CASE WHEN
    (SELECT count(DISTINCT file_id) FROM ${BASE}_files WHERE snapshot_id='${SNAPSHOT_ID}')
    = (SELECT count(*) FROM file_lineage)
THEN true ELSE error('Iceberg validation failed: source-file count mismatch') END;

-- Logical atomicity: readers resolve only snapshots with state COMMITTED.
-- This row is written last; any earlier failure leaves invisible orphan rows
-- that can be removed by the compaction/orphan cleanup stage.
INSERT INTO ${BASE}_snapshots BY NAME
SELECT 12 AS index_version,
       '${DATASET_ID}' AS dataset_id,
       '${SNAPSHOT_ID}' AS snapshot_id,
       (SELECT snapshot_id FROM ${BASE}_snapshots
        WHERE state='COMMITTED' ORDER BY created_at_ns DESC LIMIT 1) AS parent_snapshot_id,
       ${SNAPSHOT_ID}::UBIGINT AS created_at_ns,
       'COMMITTED' AS state,
       '${PLAN_FP}' AS root_glob,
       (SELECT any_value(tree_name) FROM stage_files_input) AS tree_name,
       (SELECT to_json(list(logical_path ORDER BY logical_path))::VARCHAR
        FROM (SELECT DISTINCT logical_path FROM stage_schemas_input)) AS logical_paths,
       '${BASE}_files' AS files_table,
       '${BASE}_schemas' AS schemas_table,
       '${BASE}_access' AS access_table,
       '${BASE}_baskets' AS baskets_table,
       '${PLAN_FP}' AS plan_fingerprint,
       '${SCHEMA_FP}' AS schema_fingerprint,
       ${CHUNK_COUNT}::UBIGINT AS chunk_count,
       (SELECT count(*) FROM file_lineage)::UBIGINT AS source_file_count;

-- The generation becomes visible only through this final commit row.
INSERT INTO ${BASE}_commits BY NAME
SELECT '${SNAPSHOT_ID}' AS snapshot_id,
       '${PLAN_FP}' AS plan_fingerprint,
       'COMMITTED' AS state,
       ${SNAPSHOT_ID}::UBIGINT AS committed_at_ns,
       '${VALIDATION}' AS validation_report,
       '${SCHEMA_FP}' AS schema_fingerprint,
       ${CHUNK_COUNT}::UBIGINT AS chunk_count,
       (SELECT count(*) FROM file_lineage)::UBIGINT AS source_file_count;

COMMIT;
SQL

MALLOC_CHECK_=3 "$DUCKDB_BIN" :memory: < "$SQL_FILE"
write_status committed "native Iceberg generation ${SNAPSHOT_ID} is visible"
trap - EXIT INT TERM
rm -f "$SQL_FILE"
echo "[OK] native Iceberg snapshot committed: ${BASE} snapshot=${SNAPSHOT_ID}"
echo "[OK] query with catalog_prefix := '${BASE}'"
