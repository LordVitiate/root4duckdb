#!/usr/bin/env bash
set -uo pipefail

ROOT_FILE="${ROOT_FILE:-}"
DICTIONARY="${DICTIONARY:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DUCKDB="${DUCKDB:-$PROJECT_DIR/build/release/duckdb}"
OUT="${OUT:-$PWD/root4duckdb_schema_audit_v335}"
JOBS="${JOBS:-2}"
PER_PATH_TIMEOUT="${PER_PATH_TIMEOUT:-180}"
BLOOM_BYTES="${BLOOM_BYTES:-4096}"
RESUME="${RESUME:-1}"
PATHS_FILE="${PATHS_FILE:-$PROJECT_DIR/test/phast/schema_paths.psv}"
source "$PROJECT_DIR/scripts/lib/sql.sh"

one_line() { tr '\t\r\n' '   ' | sed 's/[[:space:]][[:space:]]*/ /g'; }
now_ns() { date +%s%N; }
elapsed_ms() { local a="$1" b="$2"; printf '%s' $(( (b-a)/1000000 )); }

run_dictionary_cli() {
    local timeout_s="$1" sql_file="$2" log_file="$3"
    ulimit -c 0 2>/dev/null || true
    timeout "$timeout_s" "$DUCKDB" -csv < "$sql_file" > "$log_file" 2>&1
}

classify_message() {
    local m="$1"
    case "$m" in
        *"No persistent physical branch"*) printf 'NO_PHYSICAL_BRANCH' ;;
        *"no persistent baskets"*|*"has no persistent baskets"*) printf 'NO_PERSISTENT_BASKETS' ;;
        *"supports numeric leaves"*|*"unsupported leaf type"*|*"Unsupported ROOT leaf type"*|*"does not end in a primitive/string"*) printf 'UNSUPPORTED_TYPE' ;;
        *"absent in ROOT streamer path"*|*"Field '"*" is absent"*) printf 'PATH_RESOLUTION' ;;
        *"dictionary is unavailable"*|*"Failed to load ROOT dictionary"*) printf 'NO_DICTIONARY' ;;
        *"No TTree found"*|*"No object branch"*) printf 'NO_TREE_OR_BRANCH' ;;
        *"No ROOT file was indexed successfully"*) printf 'BUILD_ERROR' ;;
        "") printf 'TIMEOUT_OR_CRASH' ;;
        *) printf 'BUILD_ERROR' ;;
    esac
}

write_part() {
    local part="$1"; shift
    local fields=("$@") i
    {
        for ((i=0; i<30; ++i)); do
            (( i > 0 )) && printf '\t'
            printf '%s' "${fields[i]-}"
        done
        printf '\n'
    } > "$part"
}

worker() {
    local ordinal="$1" root_tree="$2" semantic_kind="$3" logical_path="$4" test_path="$5"
    local cpp_type="$6" category="$7" expected_support="$8" parent_entity="$9"
    local part="$OUT/parts/$(printf '%04d' "$ordinal").tsv"
    mkdir -p "$OUT/parts" "$OUT/indexes" "$OUT/logs"
    if [[ "$RESUME" == "1" && -s "$part" ]]; then return 0; fi

    if [[ "$expected_support" == "STRUCTURAL" || -z "$test_path" ]]; then
        write_part "$part" "$ordinal" "$root_tree" "$semantic_kind" "$logical_path" "$test_path" "$cpp_type" "$category" "$expected_support" "$parent_entity" \
            "SKIPPED" "STRUCTURAL_NODE" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "Not a terminal SQL value"
        return 0
    fi

    local hash idx_dir log_dir build_log read_log start end build_ms read_ms
    hash="$(printf '%s' "$test_path" | sha256sum | awk '{print substr($1,1,12)}')"
    idx_dir="$OUT/indexes/$(printf '%04d' "$ordinal")_${hash}"
    log_dir="$OUT/logs/$(printf '%04d' "$ordinal")_${hash}"
    build_log="$log_dir/build.log"; read_log="$log_dir/read.log"
    mkdir -p "$log_dir"
    rm -rf "$idx_dir"

    local q_root q_dict q_test q_idx sql
    q_root="$(root4duckdb_sql_escape "$ROOT_FILE")"; q_dict="$(root4duckdb_sql_escape "$DICTIONARY")"
    q_test="$(root4duckdb_sql_escape "$test_path")"; q_idx="$(root4duckdb_sql_escape "$idx_dir")"
    sql="SELECT * FROM root_build_index('$q_root','$root_tree','$q_test','$q_idx', dictionary := '$q_dict', index_threads := 1, bloom_bytes := $BLOOM_BYTES, overwrite := true, allow_partial := false);"

    local build_sql_file="$log_dir/build.sql"
    printf '%s\n' "$sql" > "$build_sql_file"
    start="$(now_ns)"
    run_dictionary_cli "${PER_PATH_TIMEOUT}s" "$build_sql_file" "$build_log" || true
    end="$(now_ns)"; build_ms="$(elapsed_ms "$start" "$end")"

    if [[ ! -f "$idx_dir/current.json" ]]; then
        local failed message reason status
        failed="$(find "$idx_dir" -maxdepth 1 -type f -name 'failed-*.csv' 2>/dev/null | sort | tail -1 || true)"
        message=""
        if [[ -n "$failed" ]]; then
            local q_failed
            q_failed="$(root4duckdb_sql_escape "$failed")"
            message="$("$DUCKDB" -noheader -list -c "SELECT message FROM read_csv_auto('$q_failed', header=true) LIMIT 1;" 2>/dev/null | one_line || true)"
        fi
        if [[ -z "$message" ]]; then message="$(tail -20 "$build_log" 2>/dev/null | one_line || true)"; fi
        reason="$(classify_message "$message")"
        if [[ "$expected_support" == "EXPECTED_UNSUPPORTED" && ( "$reason" == "UNSUPPORTED_TYPE" || "$reason" == "PATH_RESOLUTION" ) ]]; then
            status="EXPECTED_FAIL"
        else
            status="FAIL"
        fi
        write_part "$part" "$ordinal" "$root_tree" "$semantic_kind" "$logical_path" "$test_path" "$cpp_type" "$category" "$expected_support" "$parent_entity" \
            "$status" "$reason" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "" "$build_ms" "" "$idx_dir" "$message"
        printf '[%04d] %-13s %-24s %s\n' "$ordinal" "$status" "$reason" "$test_path"
        return 0
    fi

    local meta branch_name root_type duckdb_type index_signature container_depth
    meta="$("$DUCKDB" -noheader -list -c "SELECT concat_ws(chr(31),(SELECT coalesce(min(basket_branch_name),'') FROM read_parquet('$q_idx/snapshots/*/root_baskets.parquet')),coalesce(root_type,''),coalesce(duckdb_type,''),coalesce(index_signature,''),coalesce(container_depth::VARCHAR,'')) FROM read_parquet('$q_idx/snapshots/*/root_schemas.parquet') LIMIT 1;" 2>>"$read_log" || true)"
    IFS=$'\x1f' read -r branch_name root_type duckdb_type index_signature container_depth < <(printf '%s' "$meta")

    local key_args='event_fk' key_group='event_fk' col
    if [[ -n "${index_signature:-}" ]]; then
        IFS=',' read -ra cols <<< "$index_signature"
        for col in "${cols[@]}"; do
            col="${col#${col%%[![:space:]]*}}"; col="${col%${col##*[![:space:]]}}"
            [[ -z "$col" ]] && continue
            key_args+=",\"$col\""; key_group+=",\"$col\""
        done
    fi

    local stats_sql stats row_count events_with_values min_value max_value null_count nan_count duplicate_groups key_fp data_fp entries
    stats_sql="WITH d AS (SELECT * FROM read_root_dataset('$q_idx','$q_test')), s AS (SELECT count(*) AS row_count, count(DISTINCT event_fk) events_with_values, min(TRY_CAST(value AS DOUBLE)) min_value, max(TRY_CAST(value AS DOUBLE)) max_value, count(*) FILTER (WHERE value IS NULL) null_count, count(*) FILTER (WHERE isnan(TRY_CAST(value AS DOUBLE))) nan_count, sum(hash($key_args)::HUGEINT) key_fp, sum(hash($key_args,value)::HUGEINT) data_fp FROM d), g AS (SELECT count(*) duplicate_groups FROM (SELECT $key_group FROM d GROUP BY $key_group HAVING count(*)>1)) SELECT concat_ws(chr(31),s.row_count::VARCHAR,s.events_with_values::VARCHAR,coalesce(s.min_value::VARCHAR,''),coalesce(s.max_value::VARCHAR,''),s.null_count::VARCHAR,s.nan_count::VARCHAR,g.duplicate_groups::VARCHAR,coalesce(s.key_fp::VARCHAR,''),coalesce(s.data_fp::VARCHAR,'')) FROM s,g;"
    start="$(now_ns)"
    stats="$("$DUCKDB" -noheader -list -c "$stats_sql" 2>>"$read_log" || true)"
    end="$(now_ns)"; read_ms="$(elapsed_ms "$start" "$end")"
    stats="$(printf '%s' "$stats" | tail -1)"
    IFS=$'\x1f' read -r row_count events_with_values min_value max_value null_count nan_count duplicate_groups key_fp data_fp < <(printf '%s' "$stats")

    entries="$("$DUCKDB" -noheader -list -c "SELECT total_entries FROM read_parquet('$q_idx/snapshots/*/root_files.parquet') LIMIT 1;" 2>>"$read_log" | tail -1 || true)"

    if [[ ! "${row_count:-}" =~ ^[0-9]+$ ]]; then
        local message
        message="$(tail -30 "$read_log" | one_line || true)"
        write_part "$part" "$ordinal" "$root_tree" "$semantic_kind" "$logical_path" "$test_path" "$cpp_type" "$category" "$expected_support" "$parent_entity" \
            "FAIL" "READ_ERROR" "$branch_name" "$root_type" "$duckdb_type" "$index_signature" "$container_depth" "$entries" "" "" "" "" "" "" "" "" "" "$build_ms" "$read_ms" "$idx_dir" "$message"
        printf '[%04d] %-13s %-24s %s\n' "$ordinal" "FAIL" "READ_ERROR" "$test_path"
        return 0
    fi

    local status reason message
    if [[ "$expected_support" == "EXPECTED_UNSUPPORTED" ]]; then status="UNEXPECTED_OK"; reason="UNEXPECTED_SUPPORT";
    elif [[ "$row_count" == "0" ]]; then status="OK_EMPTY"; reason="";
    elif [[ "${duplicate_groups:-0}" != "0" ]]; then status="FAIL"; reason="DUPLICATE_KEYS";
    else status="OK"; reason=""; fi
    message="indexed and read back without dictionary"
    write_part "$part" "$ordinal" "$root_tree" "$semantic_kind" "$logical_path" "$test_path" "$cpp_type" "$category" "$expected_support" "$parent_entity" \
        "$status" "$reason" "$branch_name" "$root_type" "$duckdb_type" "$index_signature" "$container_depth" "$entries" "$row_count" "$events_with_values" "$min_value" "$max_value" "$null_count" "$nan_count" "$duplicate_groups" "$key_fp" "$data_fp" "$build_ms" "$read_ms" "$idx_dir" "$message"
    printf '[%04d] %-13s rows=%-8s %s\n' "$ordinal" "$status" "${row_count:-?}" "$test_path"
}

if [[ "${1:-}" == "--worker" ]]; then shift; worker "$@"; exit 0; fi

if [[ -z "$ROOT_FILE" || -z "$DICTIONARY" ]]; then
    echo "Usage: ROOT_FILE=/path/file.root DICTIONARY=/path/libPhast.so ./scripts/audit-phast-schema.sh" >&2
    exit 2
fi

for required in "$ROOT_FILE" "$DICTIONARY" "$DUCKDB" "$PATHS_FILE"; do
    if [[ ! -e "$required" ]]; then printf 'Missing required path: %s\n' "$required" >&2; exit 2; fi
done
"$SCRIPT_DIR/check-root-dictionary.sh" "$DICTIONARY"
mkdir -p "$OUT/parts" "$OUT/indexes" "$OUT/logs"
printf 'ROOT4DUCKDB full schema audit v3.3.5\nROOT: %s\nDictionary: %s\nDuckDB: %s\nOutput: %s\nJobs: %s\nResume: %s\n\n' "$ROOT_FILE" "$DICTIONARY" "$DUCKDB" "$OUT" "$JOBS" "$RESUME"

running=0
{
    IFS= read -r _header
    while IFS='|' read -r ordinal root_tree semantic_kind logical_path test_path cpp_type category expected_support parent_entity; do
        "$0" --worker "$ordinal" "$root_tree" "$semantic_kind" "$logical_path" "$test_path" "$cpp_type" "$category" "$expected_support" "$parent_entity" &
        running=$((running+1))
        if (( running >= JOBS )); then wait -n || true; running=$((running-1)); fi
    done
    wait || true
} < "$PATHS_FILE"

RESULTS="$OUT/audit_results.tsv"
printf 'ordinal\troot_tree\tsemantic_kind\tlogical_path\ttest_path\tcpp_type\tcategory\texpected_support\tparent_entity\tstatus\treason_code\tphysical_branch\troot_type\tduckdb_type\tindex_signature\tcontainer_depth\tentries\trow_count\tevents_with_values\tmin_value\tmax_value\tnull_count\tnan_count\tduplicate_key_groups\tkey_fingerprint\tdata_fingerprint\tbuild_ms\tread_ms\tindex_dir\tmessage\n' > "$RESULTS"
find "$OUT/parts" -maxdepth 1 -type f -name '*.tsv' -print0 | sort -z | xargs -0 cat >> "$RESULTS"

q_results="$(root4duckdb_sql_escape "$RESULTS")"
"$DUCKDB" -c "COPY (SELECT * FROM read_csv('$q_results', delim='\\t', header=true, all_varchar=true)) TO '$(root4duckdb_sql_escape "$OUT/audit_results.parquet")' (FORMAT PARQUET, COMPRESSION ZSTD);" >/dev/null 2>&1 || true
"$DUCKDB" -csv -c "SELECT status,count(*) paths FROM read_csv('$q_results',delim='\\t',header=true,all_varchar=true) GROUP BY status ORDER BY status;" > "$OUT/status_summary.csv" 2>/dev/null || true
"$DUCKDB" -csv -c "SELECT reason_code,count(*) paths FROM read_csv('$q_results',delim='\\t',header=true,all_varchar=true) WHERE reason_code<>'' GROUP BY reason_code ORDER BY paths DESC;" > "$OUT/failure_summary.csv" 2>/dev/null || true
"$DUCKDB" -csv -c "SELECT parent_entity,index_signature,count(*) field_count,count(DISTINCT row_count) row_count_variants,count(DISTINCT key_fingerprint) key_fingerprint_variants,string_agg(logical_path,' | ' ORDER BY logical_path) fields FROM read_csv('$q_results',delim='\\t',header=true,all_varchar=true) WHERE status IN ('OK','OK_EMPTY','UNEXPECTED_OK') AND category='numeric_scalar' GROUP BY parent_entity,index_signature HAVING count(*)>1 ORDER BY key_fingerprint_variants DESC,row_count_variants DESC,parent_entity;" > "$OUT/entity_consistency.csv" 2>/dev/null || true
"$DUCKDB" -csv -c "SELECT * FROM read_csv('$q_results',delim='\\t',header=true,all_varchar=true) WHERE status IN ('FAIL','UNEXPECTED_OK') OR TRY_CAST(duplicate_key_groups AS BIGINT)>0 ORDER BY ordinal::INTEGER;" > "$OUT/bugs.csv" 2>/dev/null || true

{
    printf '=== STATUS ===\n'; cat "$OUT/status_summary.csv" 2>/dev/null || true
    printf '\n=== FAILURE / LIMITATION CODES ===\n'; cat "$OUT/failure_summary.csv" 2>/dev/null || true
    printf '\n=== ENTITY CONSISTENCY PROBLEMS ===\n'
    "$DUCKDB" -csv -c "SELECT * FROM read_csv_auto('$(root4duckdb_sql_escape "$OUT/entity_consistency.csv")',header=true) WHERE row_count_variants>1 OR key_fingerprint_variants>1;" 2>/dev/null || true
    printf '\nResults: %s\n' "$RESULTS"
    printf 'Parquet: %s\n' "$OUT/audit_results.parquet"
    printf 'Bugs: %s\n' "$OUT/bugs.csv"
} | tee "$OUT/SUMMARY.txt"
