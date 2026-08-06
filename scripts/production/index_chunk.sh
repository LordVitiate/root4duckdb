#!/usr/bin/env bash
set -euo pipefail

PLAN=""
CHUNK_ID=""
OUTPUT_ROOT=""
PROJECT_DIR=""
DUCKDB_BIN=""
THREADS="${ROOT4DUCKDB_INDEX_THREADS:-}"
MAX_IN_FLIGHT="${ROOT4DUCKDB_MAX_IN_FLIGHT_FILES:-}"
MEMORY_LIMIT="${ROOT4DUCKDB_MEMORY_LIMIT:-}"
TEMP_LIMIT="${ROOT4DUCKDB_TEMP_LIMIT:-}"
MEMORY_BUDGET_BYTES="${ROOT4DUCKDB_MEMORY_BUDGET_BYTES:-}"
ESTIMATED_WORKER_BYTES="${ROOT4DUCKDB_ESTIMATED_WORKER_BYTES:-}"
RSS_HARD_LIMIT_BYTES="${ROOT4DUCKDB_RSS_HARD_LIMIT_BYTES:-}"
ROOT_MEMORY_BUDGET_BYTES="${ROOT4DUCKDB_ROOT_MEMORY_BUDGET_BYTES:-}"
DUCKDB_MEMORY_BUDGET_BYTES="${ROOT4DUCKDB_DUCKDB_MEMORY_BUDGET_BYTES:-}"
METADATA_MEMORY_BUDGET_BYTES="${ROOT4DUCKDB_METADATA_MEMORY_BUDGET_BYTES:-}"

while (($#)); do
    case "$1" in
        --plan) PLAN="$2"; shift 2 ;;
        --chunk-id) CHUNK_ID="$2"; shift 2 ;;
        --output-root) OUTPUT_ROOT="$2"; shift 2 ;;
        --project-dir) PROJECT_DIR="$2"; shift 2 ;;
        --duckdb) DUCKDB_BIN="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --max-in-flight-files) MAX_IN_FLIGHT="$2"; shift 2 ;;
        --memory-limit) MEMORY_LIMIT="$2"; shift 2 ;;
        --temp-limit) TEMP_LIMIT="$2"; shift 2 ;;
        --memory-budget-bytes) MEMORY_BUDGET_BYTES="$2"; shift 2 ;;
        --estimated-worker-bytes) ESTIMATED_WORKER_BYTES="$2"; shift 2 ;;
        --rss-hard-limit-bytes) RSS_HARD_LIMIT_BYTES="$2"; shift 2 ;;
        --root-memory-budget-bytes) ROOT_MEMORY_BUDGET_BYTES="$2"; shift 2 ;;
        --duckdb-memory-budget-bytes) DUCKDB_MEMORY_BUDGET_BYTES="$2"; shift 2 ;;
        --metadata-memory-budget-bytes) METADATA_MEMORY_BUDGET_BYTES="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$PLAN" && -n "$CHUNK_ID" && -n "$OUTPUT_ROOT" && -n "$PROJECT_DIR" ]] || {
    echo "required: --plan --chunk-id --output-root --project-dir" >&2
    exit 2
}
DUCKDB_BIN="${DUCKDB_BIN:-$PROJECT_DIR/build/release/duckdb}"
[[ -x "$DUCKDB_BIN" ]] || { echo "DuckDB binary not found: $DUCKDB_BIN" >&2; exit 2; }
[[ -f "$PLAN" ]] || { echo "plan not found: $PLAN" >&2; exit 2; }

readarray -t PLAN_DEFAULTS < <(python3 - "$PLAN" <<'PY'
import json, sys
p=json.load(open(sys.argv[1]))
for key in ("threads","max_in_flight_files","memory_budget_bytes","estimated_worker_bytes",
            "rss_hard_limit_bytes","root_memory_budget_bytes","duckdb_memory_budget_bytes",
            "metadata_memory_budget_bytes"):
 print(p[key])
PY
)
THREADS="${THREADS:-${PLAN_DEFAULTS[0]}}"
MAX_IN_FLIGHT="${MAX_IN_FLIGHT:-${PLAN_DEFAULTS[1]}}"
MEMORY_BUDGET_BYTES="${MEMORY_BUDGET_BYTES:-${PLAN_DEFAULTS[2]}}"
ESTIMATED_WORKER_BYTES="${ESTIMATED_WORKER_BYTES:-${PLAN_DEFAULTS[3]}}"
RSS_HARD_LIMIT_BYTES="${RSS_HARD_LIMIT_BYTES:-${PLAN_DEFAULTS[4]}}"
ROOT_MEMORY_BUDGET_BYTES="${ROOT_MEMORY_BUDGET_BYTES:-${PLAN_DEFAULTS[5]}}"
DUCKDB_MEMORY_BUDGET_BYTES="${DUCKDB_MEMORY_BUDGET_BYTES:-${PLAN_DEFAULTS[6]}}"
METADATA_MEMORY_BUDGET_BYTES="${METADATA_MEMORY_BUDGET_BYTES:-${PLAN_DEFAULTS[7]}}"
RSS_HARD_LIMIT_BYTES="${RSS_HARD_LIMIT_BYTES//[[:space:]]/}"
for value in "$THREADS" "$MAX_IN_FLIGHT" "$MEMORY_BUDGET_BYTES" "$ESTIMATED_WORKER_BYTES" "$RSS_HARD_LIMIT_BYTES" "$ROOT_MEMORY_BUDGET_BYTES" "$DUCKDB_MEMORY_BUDGET_BYTES" "$METADATA_MEMORY_BUDGET_BYTES"; do
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || { echo "resource limits must be positive integers" >&2; exit 2; }
done
SEPARATED_BUDGET=$((ROOT_MEMORY_BUDGET_BYTES + DUCKDB_MEMORY_BUDGET_BYTES + METADATA_MEMORY_BUDGET_BYTES))
((SEPARATED_BUDGET <= MEMORY_BUDGET_BYTES)) || { echo "separated memory budgets exceed total memory budget" >&2; exit 2; }
((MEMORY_BUDGET_BYTES <= RSS_HARD_LIMIT_BYTES)) || { echo "memory budget exceeds hard RSS limit" >&2; exit 2; }
if [[ -z "$MEMORY_LIMIT" ]]; then
    MEMORY_LIMIT="${DUCKDB_MEMORY_BUDGET_BYTES}B"
fi

mkdir -p "$OUTPUT_ROOT/status" "$OUTPUT_ROOT/failed"
FINAL_DIR="$OUTPUT_ROOT/$CHUNK_ID"
STATUS_PATH="$OUTPUT_ROOT/status/$CHUNK_ID.json"
STAGE="$OUTPUT_ROOT/.staging-${CHUNK_ID}-$$"

write_status() {
    local state="$1" message="${2:-}"
    python3 - "$STATUS_PATH" "$CHUNK_ID" "$PLAN" "$state" "$message" <<'PY'
import json, os, pathlib, sys, time
path, chunk_id, plan_path, state, message = sys.argv[1:]
plan=json.load(open(plan_path))
payload={"format":"root4duckdb-chunk-status-v2","chunk_id":chunk_id,
         "plan_fingerprint":plan["fingerprint"],"state":state,
         "message":message,"updated_at_ns":time.time_ns()}
target=pathlib.Path(path); target.parent.mkdir(parents=True,exist_ok=True)
tmp=target.with_suffix(target.suffix+".tmp")
tmp.write_text(json.dumps(payload,indent=2,sort_keys=True)+"\n")
os.replace(tmp,target)
PY
}

if [[ -f "$FINAL_DIR/_SUCCESS.json" ]]; then
    if python3 - "$FINAL_DIR/_SUCCESS.json" "$PLAN" "$CHUNK_ID" <<'PY'
import json,sys
s=json.load(open(sys.argv[1])); p=json.load(open(sys.argv[2]))
ok=(s.get("state")=="succeeded" and s.get("chunk_id")==sys.argv[3]
    and s.get("plan_fingerprint")==p.get("fingerprint"))
raise SystemExit(0 if ok else 1)
PY
    then
        write_status succeeded "already complete"
        echo "[OK] chunk already succeeded: $CHUNK_ID"
        exit 0
    fi
    mv "$FINAL_DIR" "$OUTPUT_ROOT/failed/${CHUNK_ID}-stale-$(date +%s)-$$"
fi

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if ((rc != 0)); then
        local failed="$OUTPUT_ROOT/failed/${CHUNK_ID}-$(date +%s)-$$"
        [[ -d "$STAGE" ]] && mv "$STAGE" "$failed" 2>/dev/null || true
        write_status failed "worker exit code $rc; diagnostics: $failed" || true
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

rm -rf "$STAGE"
mkdir -p "$STAGE/index" "$STAGE/temp"
write_status running "worker started"

python3 - "$PLAN" "$CHUNK_ID" "$STAGE" <<'PY'
import hashlib,json,pathlib,struct,sys
plan_path,chunk_id,stage=sys.argv[1:]
plan=json.load(open(plan_path))
if plan.get("index_version") != 12: raise SystemExit("plan index version mismatch")
chunk=next((c for c in plan["chunks"] if c["chunk_id"]==chunk_id),None)
if chunk is None: raise SystemExit(f"chunk not found: {chunk_id}")

def hash_file(path):
 h=hashlib.sha256()
 with open(path,"rb") as f:
  for block in iter(lambda:f.read(1024*1024),b""): h.update(block)
 return h.hexdigest()
if hash_file(plan["dictionary"]) != plan["dictionary_fingerprint"]:
 raise SystemExit("dictionary changed after planning")

def fnv(data,seed=14695981039346656037):
 for byte in data:
  seed ^= byte; seed=(seed*1099511628211)&0xffffffffffffffff
 return seed

def local_source_id(uri,size,mtime_ns):
 value=fnv(uri.encode()); value=fnv(struct.pack("<Q",size),value); value=fnv(struct.pack("<q",mtime_ns),value)
 return f"{value:016x}"
for row in chunk["files"]:
 uri=row["uri"]
 if "://" in uri and not uri.startswith("file://"): continue
 path=pathlib.Path(uri.removeprefix("file://")).resolve(); st=path.stat()
 actual=local_source_id(str(path),st.st_size,st.st_mtime_ns)
 if actual != row["source_id"]:
  raise SystemExit(f"source changed after planning: {uri}")
p=pathlib.Path(stage)
(p/"inputs.uris").write_text("".join(f"{r['uri']}\n" for r in chunk["files"]))
(p/"chunk.json").write_text(json.dumps({
 "plan_fingerprint":plan["fingerprint"],
 "manifest_fingerprint":plan["manifest_fingerprint"],
 "dictionary_fingerprint":plan["dictionary_fingerprint"],
 "tree":plan["tree"],"logical_paths":plan["logical_paths"],
 "dictionary":plan["dictionary"],"chunk":chunk,
 "resource_budgets":{"total":plan.get("memory_budget_bytes"),
  "root":plan.get("root_memory_budget_bytes"),"duckdb":plan.get("duckdb_memory_budget_bytes"),
  "metadata":plan.get("metadata_memory_budget_bytes"),"rss_hard":plan.get("rss_hard_limit_bytes")}},indent=2,sort_keys=True)+"\n")
(p/"sql-values.json").write_text(json.dumps({"tree":plan["tree"],
 "paths":json.dumps(plan["logical_paths"],separators=(",",":")),
 "dictionary":plan["dictionary"],"plan":plan["fingerprint"],
 "manifest":plan["manifest_fingerprint"],"dictionary_fp":plan["dictionary_fingerprint"]}))
PY

readarray -t SQL_VALUES < <(python3 - "$STAGE/sql-values.json" <<'PY'
import json,sys
v=json.load(open(sys.argv[1]))
for k in ("tree","paths","dictionary","plan","manifest","dictionary_fp"):
 print(v[k].replace("'","''"))
PY
)
TREE_SQL="${SQL_VALUES[0]}"; PATHS_SQL="${SQL_VALUES[1]}"; DICT_SQL="${SQL_VALUES[2]}"
PLAN_SQL="${SQL_VALUES[3]}"; MANIFEST_SQL="${SQL_VALUES[4]}"; DICT_FP_SQL="${SQL_VALUES[5]}"
INPUT_SQL="$(printf '%s' "@$STAGE/inputs.uris" | sed "s/'/''/g")"
OUTPUT_SQL="$(printf '%s' "$STAGE/index" | sed "s/'/''/g")"
TEMP_SQL="$(printf '%s' "$STAGE/temp" | sed "s/'/''/g")"
CHUNK_SQL="$(printf '%s' "$CHUNK_ID" | sed "s/'/''/g")"
TEMP_LIMIT_SQL=""
if [[ -n "$TEMP_LIMIT" ]]; then
  TEMP_LIMIT_ESCAPED="$(printf '%s' "$TEMP_LIMIT" | sed "s/'/''/g")"
  TEMP_LIMIT_SQL="SET max_temp_directory_size='$TEMP_LIMIT_ESCAPED';"
fi

cat > "$STAGE/index.sql" <<SQL
.timer on
LOAD parquet;
SET threads=$THREADS;
SET memory_limit='$MEMORY_LIMIT';
SET preserve_insertion_order=false;
SET temp_directory='$TEMP_SQL';
$TEMP_LIMIT_SQL
COPY (
 SELECT * FROM root_build_dataset_index(
  '$INPUT_SQL','$TREE_SQL','$PATHS_SQL','$OUTPUT_SQL',
  dictionary:='$DICT_SQL', dictionary_cleanup:='retain',
  index_threads:=$THREADS, max_in_flight_files:=$MAX_IN_FLIGHT,
  memory_budget_bytes:=$MEMORY_BUDGET_BYTES,
  estimated_worker_bytes:=$ESTIMATED_WORKER_BYTES,
  catalog_mode:='external',
  chunk_id:='$CHUNK_SQL', manifest_fingerprint:='$MANIFEST_SQL',
  dictionary_fingerprint:='$DICT_FP_SQL', overwrite:=true, allow_partial:=false
 )
) TO '$STAGE/status.csv' (HEADER, DELIMITER ',');
SQL

MALLOC_CHECK_=3 python3 "$PROJECT_DIR/scripts/production/bounded_exec.py" \
    --rss-limit-bytes "$RSS_HARD_LIMIT_BYTES" \
    --report "$STAGE/resource-usage.json" -- \
    "$DUCKDB_BIN" :memory: < "$STAGE/index.sql" >"$STAGE/duckdb.out" 2>"$STAGE/duckdb.err"

python3 - "$STAGE" "$PLAN" "$CHUNK_ID" <<'PY'
import csv,hashlib,json,os,pathlib,sys,time
stage=pathlib.Path(sys.argv[1]); plan=json.load(open(sys.argv[2])); chunk_id=sys.argv[3]
chunk=next(c for c in plan["chunks"] if c["chunk_id"]==chunk_id)
rows=list(csv.DictReader((stage/"status.csv").open()))
if not rows or any(r.get("status")!="OK" for r in rows): raise SystemExit("chunk status contains failed rows")
if any(r.get("publish_mode")!="external-staging" or r.get("published","").lower()!="false" for r in rows):
 raise SystemExit("worker did not leave immutable external-catalog staging")
expected={r["source_id"] for r in chunk["files"]}; actual={r["file_id"] for r in rows}
if actual != expected: raise SystemExit(f"indexed source ids differ: missing={sorted(expected-actual)} extra={sorted(actual-expected)}")
current=stage/"index"/"current.json"
if not current.is_file(): raise SystemExit("current.json missing")
snapshot=json.load(current.open())
for key,wanted in (("chunk_id",chunk_id),("manifest_fingerprint",plan["manifest_fingerprint"]),
                   ("dictionary_fingerprint",plan["dictionary_fingerprint"])):
 if snapshot.get(key)!=wanted: raise SystemExit(f"snapshot {key} mismatch")
checksums={}
schema_fingerprint=""
for key,value in snapshot["tables"].items():
 path=pathlib.Path(value)
 if not path.is_absolute(): path=stage/"index"/path
 if not path.is_file(): raise SystemExit(f"metadata part missing: {path}")
 h=hashlib.sha256(path.read_bytes()).hexdigest(); checksums[str(path.relative_to(stage/"index"))]=h
 if key == "schemas": schema_fingerprint=h
if not schema_fingerprint: raise SystemExit("schema metadata fingerprint is missing")
snapshot["schema_fingerprint"]=schema_fingerprint
current_tmp=current.with_suffix(current.suffix+".tmp")
current_tmp.write_text(json.dumps(snapshot,indent=2,sort_keys=True)+"\n"); os.replace(current_tmp,current)
success={"format":"root4duckdb-chunk-success-v2","state":"succeeded","chunk_id":chunk_id,
 "plan_fingerprint":plan["fingerprint"],"manifest_fingerprint":plan["manifest_fingerprint"],
 "dictionary_fingerprint":plan["dictionary_fingerprint"],"schema_fingerprint":schema_fingerprint,
 "snapshot_id":snapshot["snapshot_id"],"source_ids":sorted(actual),"source_file_count":len(actual),
 "checksums":checksums,"completed_at_ns":time.time_ns()}
tmp=stage/"index"/"_SUCCESS.json.tmp"; tmp.write_text(json.dumps(success,indent=2,sort_keys=True)+"\n")
os.replace(tmp,stage/"index"/"_SUCCESS.json")
PY

mkdir -p "$STAGE/index/_worker"
for artifact in chunk.json inputs.uris index.sql status.csv duckdb.out duckdb.err resource-usage.json; do
    [[ -f "$STAGE/$artifact" ]] && cp -p "$STAGE/$artifact" "$STAGE/index/_worker/$artifact"
done
rm -rf "$FINAL_DIR"
mv "$STAGE/index" "$FINAL_DIR"
rm -rf "$STAGE"
write_status succeeded "immutable chunk snapshot complete"
trap - EXIT INT TERM
echo "[OK] chunk committed locally: $FINAL_DIR"
