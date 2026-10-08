#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JDBC_JAR="${ROOT4DUCKDB_JDBC_JAR:-/tmp/duckdb_jdbc-1.5.6.0.jar}"
EXTENSION="${ROOT4DUCKDB_EXTENSION:-$PROJECT_DIR/build/release/extension/root/root.duckdb_extension}"
MDST="${ROOT4DUCKDB_MDST:-/eos/experiment/compass/data/2022/W08/slot1/mDST/mDST-297667-1-8.root}"
PHAST_LIB="${ROOT4DUCKDB_PHAST_LIB:-/eos/experiment/compass/scratch/SSS/ontop-test/phast.8.051/lib/libPhast.so}"
JAVA_BIN="${ROOT4DUCKDB_JAVA:-$(command -v java)}"
JAVAC_BIN="${ROOT4DUCKDB_JAVAC:-$(command -v javac)}"
JAVA_HOME_RESOLVED="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$JAVA_BIN")")")}"
JSIG="${ROOT4DUCKDB_JSIG:-$JAVA_HOME_RESOLVED/lib/libjsig.so}"
TIMEOUT_SECONDS="${ROOT4DUCKDB_JDBC_TIMEOUT:-240}"

for file in "$JDBC_JAR" "$EXTENSION" "$MDST" "$PHAST_LIB" "$JSIG"; do
    if [[ ! -f "$file" ]]; then
        printf 'Missing prerequisite: %s\n' "$file" >&2
        exit 2
    fi
done
if [[ "$EXTENSION" == *"'"* ]]; then
    echo "Extension path must not contain a single quote" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/root4duckdb-jdbc.XXXXXXXX")"
cleanup() {
    status=$?
    if (( status == 0 )); then
        rm -rf "$WORK_DIR"
    else
        printf 'JDBC diagnostics retained: %s\n' "$WORK_DIR" >&2
    fi
}
trap cleanup EXIT
printf "LOAD '%s';\n" "$EXTENSION" > "$WORK_DIR/init.sql"
"$JAVAC_BIN" --release 17 -d "$WORK_DIR" "$PROJECT_DIR/test/fixtures/JdbcRootIntegration.java"

run_scenario() {
    local mode="$1" workers="$2"
    printf '[TEST] JDBC %s workers=%s\n' "$mode" "$workers"
    timeout -k 5s "${TIMEOUT_SECONDS}s" env LD_PRELOAD="$JSIG${LD_PRELOAD:+:$LD_PRELOAD}" \
        "$JAVA_BIN" -Xmx768m -XX:ActiveProcessorCount=2 \
        -XX:ErrorFile="$WORK_DIR/hs_err_pid%p.log" \
        -cp "$JDBC_JAR:$WORK_DIR" JdbcRootIntegration \
        "$mode" "$workers" "$WORK_DIR/init.sql" "$MDST" "$PHAST_LIB"
}

run_scenario load 1
run_scenario load 1
run_scenario load 1
run_scenario load 8
run_scenario serialized 1
run_scenario object 1
run_scenario serialized 8
run_scenario object 8
