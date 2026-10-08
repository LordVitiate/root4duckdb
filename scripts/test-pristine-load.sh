#!/usr/bin/env bash
# Diagnostic only. Run with no sourced LCG/ROOT environment.
# Verify whether a standalone DuckDB can load root4duckdb in a *pristine*
# process, and report which runtime prerequisite fails first.
set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB="${ROOT4DUCKDB_INDEPENDENT_DUCKDB:-$PROJECT_DIR/.deps/vanilla-duckdb-1.5.6/duckdb}"
EXT="$PROJECT_DIR/build/release/extension/root/root.duckdb_extension"
LOG="${ROOT4DUCKDB_PRISTINE_LOG:-/tmp/root4duckdb-pristine-load.log}"

if [[ ! -x "$DUCKDB" || ! -f "$EXT" ]]; then
    echo "[FAIL] Missing DuckDB or extension: $DUCKDB / $EXT" >&2
    exit 2
fi

echo '=== HOST DUCKDB ==='
"$DUCKDB" --version

echo
echo '=== HOST DUCKDB ELF DEPENDENCIES ==='
readelf -d "$DUCKDB" | grep '(NEEDED)' || true

echo
echo '=== ROOT IN PRISTINE PATH ==='
env -i PATH=/usr/bin:/bin sh -c '
  if command -v root-config >/dev/null 2>&1; then
    echo "root-config: $(command -v root-config)"
    echo "ROOT version: $(root-config --version)"
    echo "ROOT prefix: $(root-config --prefix)"
    echo "ROOT libdir: $(root-config --libdir)"
  else
    echo "[INFO] root-config is unavailable on PATH=/usr/bin:/bin"
  fi
'

echo
echo '=== NO SOURCED RUNTIME ==='
echo 'PATH=/usr/bin:/bin'
echo 'ROOTSYS=<unset>'
echo 'LD_LIBRARY_PATH=<unset>'
echo 'CPLUS_INCLUDE_PATH=<unset>'
echo 'ROOT4DUCKDB_ROOT_CONFIG=<unset>'

echo
echo '=== PRISTINE LOAD: EXPECTED TO EXPOSE THE NEXT BLOCKER ==='
echo "[INFO] Full error output is live and saved at $LOG"

# This is the actual no-setup acceptance test: intentionally do NOT pass
# LD_LIBRARY_PATH, ROOTSYS, CPLUS_INCLUDE_PATH, GCC paths, or root-config path.
# Keep the test shell alive on failure and preserve the DuckDB exit status.
set +e
env -i \
  HOME="$HOME" \
  PATH=/usr/bin:/bin \
  timeout 45 \
  "$DUCKDB" -unsigned -csv -noheader -c "
      LOAD '$EXT';
      SELECT count(DISTINCT function_name) = 6 AS registration_ok
      FROM duckdb_functions()
      WHERE function_name IN
        ('read_root','read_root_dataset','root_build_index',
         'root_build_dataset_index','root_dataset_stats','root_iceberg_catalog');
  " 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e

# ROOT/Cling may emit a fatal compiler diagnostic while DuckDB exits 0.
# The SQL result alone is therefore not sufficient for acceptance.
if ! grep -Fxq 'true' "$LOG"; then
  echo '[FAIL] Expected registration_ok=true from all six SQL functions' >&2
  rc=1
fi

if grep -Eq \
  'fatal error:|Fatal in <|Error in <UnknownClass::FindDynamicLibrary>|CheckABICompatibility|Possible C\\+\\+ standard library mismatch' \
  "$LOG"; then
  echo '[FAIL] ROOT/Cling reported an initialization or compiler error' >&2
  rc=1
fi

echo
echo "PRISTINE LOAD STATUS: $rc"
if ((rc == 0)); then
  echo '[OK] All six SQL functions load without runtime setup or ROOT/Cling diagnostics'
else
  echo '[BLOCKED] Pristine LOAD failed or ROOT/Cling emitted an error'
fi
exit "$rc"
