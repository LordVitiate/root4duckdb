"""Dagster production graph for ROOT4DUCKDB.

Dagster owns dependencies, retries, materialization metadata and scheduling.
HTCondor is only an optional execution backend for immutable index chunks;
there is no DAGMan graph and chunk jobs never publish shared Iceberg state.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import dagster as dg

PROJECT_ROOT = Path(__file__).resolve().parents[2]
PRODUCTION = PROJECT_ROOT / "scripts" / "production"
if str(PRODUCTION) not in sys.path:
    sys.path.insert(0, str(PRODUCTION))

from pipeline_config import artifact_paths, load_config  # noqa: E402
from run_pipeline import cleanup, commit, compact, discover, execute_one, plan, validate  # noqa: E402


def _mapping_key(chunk_id: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_]", "_", chunk_id)
    return ("c_" + value)[:63]


@dg.resource(config_schema={"config_path": str})
def production_config(context: dg.InitResourceContext) -> dict:
    config = load_config(context.resource_config["config_path"])
    Path(config["workspace"]).mkdir(parents=True, exist_ok=True)
    Path(config["chunks_dir"]).mkdir(parents=True, exist_ok=True)
    return config


@dg.op(required_resource_keys={"production_config"}, out=dg.DynamicOut(str))
def discover_and_plan(context: dg.OpExecutionContext):
    config = context.resources.production_config
    paths = artifact_paths(config)
    discover(config, paths)
    plan(config, paths)
    plan_doc = json.loads(Path(paths["plan"]).read_text(encoding="utf-8"))
    context.add_output_metadata({
        "manifest": dg.MetadataValue.path(paths["manifest"]),
        "plan": dg.MetadataValue.path(paths["plan"]),
        "file_count": sum(int(chunk.get("file_count", 0)) for chunk in plan_doc["chunks"]),
        "chunk_count": int(plan_doc["chunk_count"]),
        "plan_fingerprint": str(plan_doc["fingerprint"]),
    })
    for chunk in plan_doc["chunks"]:
        chunk_id = str(chunk["chunk_id"])
        yield dg.DynamicOutput(chunk_id, mapping_key=_mapping_key(chunk_id))


@dg.op(
    required_resource_keys={"production_config"},
    retry_policy=dg.RetryPolicy(max_retries=3, delay=30),
    tags={"root4duckdb/pool": "index"},
)
def index_chunk(context: dg.OpExecutionContext, chunk_id: str) -> dict:
    config = context.resources.production_config
    paths = artifact_paths(config)
    backend = config["execution_backend"]
    context.log.info("chunk=%s backend=%s", chunk_id, backend)
    execute_one(config, paths, chunk_id)
    success = Path(config["chunks_dir"]) / chunk_id / "_SUCCESS.json"
    if not success.is_file():
        raise dg.Failure(f"chunk {chunk_id} completed without a valid _SUCCESS.json")
    status = json.loads(success.read_text(encoding="utf-8"))
    context.add_output_metadata({
        "chunk_id": chunk_id,
        "execution_backend": backend,
        "state": str(status.get("state", "unknown")),
        "attempt": int(status.get("attempt", 0)),
        "source_count": int(status.get("source_count", status.get("file_count", 0))),
        "output": dg.MetadataValue.path(str(success.parent)),
    })
    return status


@dg.op(required_resource_keys={"production_config"})
def validate_all_chunks(context: dg.OpExecutionContext, statuses: list[dict]) -> dict:
    if not statuses:
        raise dg.Failure("index plan contains no completed chunks")
    config = context.resources.production_config
    paths = artifact_paths(config)
    validate(config, paths)
    report = json.loads(Path(paths["validation"]).read_text(encoding="utf-8"))
    if report.get("state") not in {"valid", "validated", "succeeded"} and not report.get("ok", False):
        raise dg.Failure(f"validation failed: {report}")
    context.add_output_metadata({
        "validation": dg.MetadataValue.path(paths["validation"]),
        "chunk_count": len(statuses),
        "plan_fingerprint": str(report.get("plan_fingerprint", "")),
    })
    return report


@dg.op(
    required_resource_keys={"production_config"},
    retry_policy=dg.RetryPolicy(max_retries=2, delay=60),
    tags={"root4duckdb/pool": "single-committer"},
)
def single_iceberg_commit(context: dg.OpExecutionContext, validation_report: dict) -> dict:
    config = context.resources.production_config
    paths = artifact_paths(config)
    if not validation_report:
        raise dg.Failure("single committer received no validation report")
    commit(config, paths)
    if config["iceberg"]["enabled"]:
        status_path = Path(paths["commit_status"])
        if not status_path.is_file():
            raise dg.Failure("Iceberg commit returned without commit-status.json")
        status = json.loads(status_path.read_text(encoding="utf-8"))
    else:
        status = {"state": "skipped", "reason": "iceberg.disabled"}
    context.add_output_metadata({
        "state": str(status.get("state", "unknown")),
        "status": dg.MetadataValue.path(paths["commit_status"]),
    })
    return status


@dg.op(
    required_resource_keys={"production_config"},
    retry_policy=dg.RetryPolicy(max_retries=1, delay=60),
    tags={"root4duckdb/pool": "catalog-maintenance"},
)
def compact_committed_snapshot(context: dg.OpExecutionContext, commit_status: dict) -> dict:
    config = context.resources.production_config
    paths = artifact_paths(config)
    if config["iceberg"]["enabled"]:
        compact(config, paths)
        status_path = Path(paths["compact_status"])
        status = json.loads(status_path.read_text(encoding="utf-8")) if status_path.is_file() else {
            "state": "succeeded"
        }
    else:
        status = {"state": "skipped", "reason": "iceberg.disabled"}
    context.add_output_metadata({"state": str(status.get("state", "unknown"))})
    return status


@dg.op(required_resource_keys={"production_config"})
def cleanup_stale_staging(context: dg.OpExecutionContext, compact_status: dict) -> dict:
    config = context.resources.production_config
    paths = artifact_paths(config)
    delete = bool(config.get("dagster", {}).get("cleanup_orphans", False))
    cleanup(config, paths, delete=delete)
    report = json.loads(Path(paths["cleanup_status"]).read_text(encoding="utf-8"))
    context.add_output_metadata({
        "delete_enabled": delete,
        "removed_count": len(report.get("removed", [])),
        "kept_count": len(report.get("kept", [])),
    })
    return report


@dg.job(resource_defs={"production_config": production_config}, executor_def=dg.multiprocess_executor)
def root4duckdb_production_job():
    chunks = discover_and_plan().map(index_chunk)
    validated = validate_all_chunks(chunks.collect())
    committed = single_iceberg_commit(validated)
    compacted = compact_committed_snapshot(committed)
    cleanup_stale_staging(compacted)


defs = dg.Definitions(jobs=[root4duckdb_production_job])
