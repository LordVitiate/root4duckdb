#!/usr/bin/env bash
set -euo pipefail
DUCKDB_BIN=""; ATTACH_SQL=""; COMPACTION_SQL=""; STATUS=""; ALLOW_NOT_CONFIGURED=0
while (($#)); do
  case "$1" in
    --duckdb) DUCKDB_BIN="$2"; shift 2 ;;
    --attach-sql) ATTACH_SQL="$2"; shift 2 ;;
    --compaction-sql) COMPACTION_SQL="$2"; shift 2 ;;
    --status) STATUS="$2"; shift 2 ;;
    --allow-not-configured) ALLOW_NOT_CONFIGURED=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[[ -x "$DUCKDB_BIN" && -f "$ATTACH_SQL" ]] || { echo "required: --duckdb --attach-sql" >&2; exit 2; }
STATUS="${STATUS:-compact-status.json}"
if [[ -z "$COMPACTION_SQL" || ! -f "$COMPACTION_SQL" ]]; then
  python3 - "$STATUS" <<'PY'
import json,os,pathlib,sys,time
p=pathlib.Path(sys.argv[1]); p.parent.mkdir(parents=True,exist_ok=True)
t=p.with_suffix(p.suffix+'.tmp')
t.write_text(json.dumps({'format':'root4duckdb-compaction-v1','state':'not_configured',
 'message':'catalog-specific rewrite/expire SQL was not supplied','updated_at_ns':time.time_ns()},indent=2)+"\n")
os.replace(t,p)
PY
  if ((ALLOW_NOT_CONFIGURED)); then
    echo "[INFO] compaction hook not configured; explicitly allowed"
    exit 0
  fi
  echo "[FAIL] production compaction SQL is required; no unsafe generic rewrite was attempted" >&2
  exit 2
fi
sql="$(mktemp)"; trap 'rm -f "$sql"' EXIT
cat "$ATTACH_SQL" "$COMPACTION_SQL" > "$sql"
MALLOC_CHECK_=3 "$DUCKDB_BIN" :memory: < "$sql"
python3 - "$STATUS" <<'PY'
import json,os,pathlib,sys,time
p=pathlib.Path(sys.argv[1]); p.parent.mkdir(parents=True,exist_ok=True)
t=p.with_suffix(p.suffix+'.tmp')
t.write_text(json.dumps({'format':'root4duckdb-compaction-v1','state':'succeeded',
 'updated_at_ns':time.time_ns()},indent=2)+"\n"); os.replace(t,p)
PY
echo "[OK] catalog-specific compaction completed"
