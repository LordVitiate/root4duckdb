#!/usr/bin/env python3
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

from pipeline_io import detected_memory_bytes

FORMAT = "root4duckdb-production-config-v1"


def _resolve(base: Path, value: str | None) -> str | None:
    if not value:
        return value
    if "://" in value:
        return value
    path = Path(os.path.expandvars(os.path.expanduser(value)))
    return str((base / path).resolve()) if not path.is_absolute() else str(path.resolve())


def load_config(path: str | Path) -> dict[str, Any]:
    config_path = Path(path).resolve()
    data = json.loads(config_path.read_text())
    if data.get("format") != FORMAT:
        raise ValueError(f"unsupported production config: {data.get('format')!r}")
    base = config_path.parent
    project_dir = Path(_resolve(base, data.get("project_dir") or ".") or ".")
    data["project_dir"] = str(project_dir)
    for key in ("workspace", "chunks_dir", "paths_file", "dictionary", "duckdb", "attach_sql", "compaction_sql"):
        if key in data:
            data[key] = _resolve(base, data.get(key))
    data["workspace"] = data.get("workspace") or str((project_dir / "work" / "production").resolve())
    data["chunks_dir"] = data.get("chunks_dir") or str((Path(data["workspace"]) / "chunks").resolve())
    data["duckdb"] = data.get("duckdb") or str((project_dir / "build/release/duckdb").resolve())
    data["execution_backend"] = data.get("execution_backend", "local")
    if data["execution_backend"] not in {"local", "htcondor"}:
        raise ValueError("execution_backend must be local or htcondor")
    inputs = data.get("inputs")
    if not isinstance(inputs, list) or not inputs or not all(isinstance(v, str) and v for v in inputs):
        raise ValueError("inputs must be a non-empty array of strings")
    if not data.get("tree") or not data.get("paths_file") or not data.get("dictionary"):
        raise ValueError("tree, paths_file and dictionary are required")
    resources = data.setdefault("resources", {})
    if data["execution_backend"] == "htcondor" and not resources.get("request_memory_mb"):
        raise ValueError("resources.request_memory_mb is required for HTCondor; the submit host is not the worker node")
    resources.setdefault("max_files", 0)
    resources.setdefault("max_bytes", 0)
    resources.setdefault("target_chunk_seconds", 0)
    resources.setdefault("throughput_bytes_per_second", 0)
    resources.setdefault("threads", max(1, os.cpu_count() or 1))
    resources.setdefault("request_memory_mb", max(1, detected_memory_bytes() // 1_000_000))
    request_bytes = resources["request_memory_mb"] * 1_000_000
    resources.setdefault("rss_hard_limit_bytes", request_bytes * 95 // 100)
    resources.setdefault("memory_budget_bytes", request_bytes * 85 // 100)
    resources.setdefault("estimated_worker_bytes", 256 * 1024 * 1024)
    resources.setdefault("root_memory_budget_bytes", resources["memory_budget_bytes"] * 50 // 100)
    resources.setdefault("duckdb_memory_budget_bytes", resources["memory_budget_bytes"] * 40 // 100)
    resources.setdefault("metadata_memory_budget_bytes",
                         resources["memory_budget_bytes"] - resources["root_memory_budget_bytes"] -
                         resources["duckdb_memory_budget_bytes"])
    resources.setdefault("max_in_flight_files", max(1, min(
        resources["threads"], resources["root_memory_budget_bytes"] // resources["estimated_worker_bytes"])))
    for key in ("max_files", "max_bytes", "target_chunk_seconds", "throughput_bytes_per_second"):
        if not isinstance(resources[key], int) or resources[key] < 0:
            raise ValueError(f"resources.{key} must be a non-negative integer")
    if (resources["target_chunk_seconds"] == 0) != (resources["throughput_bytes_per_second"] == 0):
        raise ValueError("target_chunk_seconds and throughput_bytes_per_second must be supplied together")
    if (data["execution_backend"] == "htcondor" and resources["max_files"] == 0 and
            resources["max_bytes"] == 0 and resources["target_chunk_seconds"] == 0):
        raise ValueError("HTCondor requires an explicit chunk cap or measured throughput + target duration")
    for key in ("max_in_flight_files", "memory_budget_bytes", "estimated_worker_bytes", "request_memory_mb",
                "threads", "root_memory_budget_bytes", "duckdb_memory_budget_bytes",
                "metadata_memory_budget_bytes", "rss_hard_limit_bytes"):
        if not isinstance(resources[key], int) or resources[key] <= 0:
            raise ValueError(f"resources.{key} must be a positive integer")
    budget_sum = (resources["root_memory_budget_bytes"] + resources["duckdb_memory_budget_bytes"] +
                  resources["metadata_memory_budget_bytes"])
    if budget_sum > resources["memory_budget_bytes"]:
        raise ValueError("ROOT + DuckDB + metadata budgets exceed memory_budget_bytes")
    iceberg = data.setdefault("iceberg", {})
    iceberg.setdefault("enabled", False)
    iceberg.setdefault("require_compaction", True)
    if iceberg["enabled"]:
        for key in ("catalog", "namespace", "prefix"):
            if not iceberg.get(key):
                raise ValueError(f"iceberg.{key} is required when Iceberg commit is enabled")
        if not data.get("attach_sql"):
            raise ValueError("attach_sql is required when Iceberg commit is enabled")
        if iceberg["require_compaction"] and not data.get("compaction_sql"):
            raise ValueError("compaction_sql is required for a production Iceberg commit")
    dagster = data.setdefault("dagster", {})
    dagster.setdefault("job_name", "root4duckdb_production_job")
    dagster.setdefault("retries", 3)
    dagster.setdefault("retry_delay_seconds", 30)
    dagster.setdefault("cleanup_orphans", False)
    return data


def artifact_paths(config: dict[str, Any]) -> dict[str, str]:
    workspace = Path(config["workspace"])
    return {
        "manifest": str(workspace / "dataset-manifest.json"),
        "plan": str(workspace / "index-plan.json"),
        "chunk_inputs": str(workspace / "chunk-inputs"),
        "validation": str(workspace / "validation.json"),
        "commit_status": str(workspace / "commit-status.json"),
        "compact_status": str(workspace / "compact-status.json"),
        "cleanup_status": str(workspace / "cleanup-status.json"),
        "backend_logs": str(workspace / "backend-logs"),
    }
