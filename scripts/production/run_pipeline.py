#!/usr/bin/env python3
from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
from pathlib import Path

from pipeline_config import artifact_paths, load_config


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def discover(config: dict, paths: dict[str, str]) -> str:
    command = [sys.executable, str(Path(config["project_dir"]) / "scripts/production/discover_dataset.py")]
    for value in config["inputs"]:
        command += ["--input", value]
    for query in config.get("uri_queries", []):
        command += ["--uri-query", query]
    if config.get("uri_queries"):
        command += ["--duckdb", config["duckdb"]]
    if config.get("recursive", False):
        command.append("--recursive")
    command += ["--output", paths["manifest"]]
    run(command)
    return paths["manifest"]


def plan(config: dict, paths: dict[str, str]) -> str:
    r = config["resources"]
    command = [
        sys.executable, str(Path(config["project_dir"]) / "scripts/production/plan_chunks.py"),
        "--manifest", paths["manifest"], "--tree", config["tree"],
        "--paths-file", config["paths_file"], "--dictionary", config["dictionary"],
        "--max-files", str(r["max_files"]), "--max-bytes", str(r["max_bytes"]),
        "--target-chunk-seconds", str(r["target_chunk_seconds"]),
        "--throughput-bytes-per-second", str(r["throughput_bytes_per_second"]),
        "--threads", str(r["threads"]),
        "--max-in-flight-files", str(r["max_in_flight_files"]),
        "--memory-budget-bytes", str(r["memory_budget_bytes"]),
        "--estimated-worker-bytes", str(r["estimated_worker_bytes"]),
        "--request-memory-mb", str(r["request_memory_mb"]),
        "--root-memory-budget-bytes", str(r["root_memory_budget_bytes"]),
        "--duckdb-memory-budget-bytes", str(r["duckdb_memory_budget_bytes"]),
        "--metadata-memory-budget-bytes", str(r["metadata_memory_budget_bytes"]),
        "--rss-hard-limit-bytes", str(r["rss_hard_limit_bytes"]),
        "--output", paths["plan"], "--emit-dir", paths["chunk_inputs"],
    ]
    run(command)
    return paths["plan"]


def execute_one(config: dict, paths: dict[str, str], chunk_id: str) -> None:
    r = config["resources"]
    command = [
        sys.executable, str(Path(config["project_dir"]) / "scripts/production/execute_chunk.py"),
        "--backend", config["execution_backend"], "--plan", paths["plan"],
        "--chunk-id", chunk_id, "--chunks-dir", config["chunks_dir"],
        "--project-dir", config["project_dir"], "--duckdb", config["duckdb"],
        "--threads", str(r["threads"]), "--max-in-flight-files", str(r["max_in_flight_files"]),
        "--memory-budget-bytes", str(r["memory_budget_bytes"]),
        "--estimated-worker-bytes", str(r["estimated_worker_bytes"]),
        "--rss-hard-limit-bytes", str(r["rss_hard_limit_bytes"]),
        "--root-memory-budget-bytes", str(r["root_memory_budget_bytes"]),
        "--duckdb-memory-budget-bytes", str(r["duckdb_memory_budget_bytes"]),
        "--metadata-memory-budget-bytes", str(r["metadata_memory_budget_bytes"]),
        "--request-cpus", str(r["threads"]), "--request-memory-mb", str(r["request_memory_mb"]),
        "--backend-work-dir", paths["backend_logs"],
    ]
    htcondor = config.get("htcondor", {})
    command += ["--job-flavour", htcondor.get("job_flavour", "workday")]
    run(command)


def validate(config: dict, paths: dict[str, str]) -> None:
    run([sys.executable, str(Path(config["project_dir"]) / "scripts/production/validate_chunks.py"),
         "--plan", paths["plan"], "--chunks-dir", config["chunks_dir"],
         "--output", paths["validation"]])


def commit(config: dict, paths: dict[str, str]) -> None:
    iceberg = config["iceberg"]
    if not iceberg["enabled"]:
        print("[INFO] Iceberg commit disabled")
        return
    run([str(Path(config["project_dir"]) / "scripts/production/commit_iceberg.sh"),
         "--duckdb", config["duckdb"], "--attach-sql", config["attach_sql"],
         "--catalog", iceberg["catalog"], "--namespace", iceberg["namespace"],
         "--prefix", iceberg["prefix"], "--plan", paths["plan"],
         "--chunks-dir", config["chunks_dir"], "--status", paths["commit_status"]])


def compact(config: dict, paths: dict[str, str]) -> None:
    if not config["iceberg"]["enabled"]:
        return
    command = [str(Path(config["project_dir"]) / "scripts/production/compact_iceberg.sh"),
               "--duckdb", config["duckdb"], "--attach-sql", config["attach_sql"],
               "--status", paths["compact_status"]]
    if config.get("compaction_sql"):
        command += ["--compaction-sql", config["compaction_sql"]]
    run(command)


def cleanup(config: dict, paths: dict[str, str], delete: bool) -> None:
    command = [sys.executable, str(Path(config["project_dir"]) / "scripts/production/cleanup_orphans.py"),
               "--root", config["chunks_dir"], "--output", paths["cleanup_status"]]
    if delete:
        command.append("--delete")
    run(command)


def main() -> None:
    parser = argparse.ArgumentParser(description="Scheduler-neutral production reference runner")
    parser.add_argument("--config", required=True)
    parser.add_argument("--max-parallel-chunks", type=int, default=4)
    parser.add_argument("--cleanup-orphans", action="store_true")
    args = parser.parse_args()
    config = load_config(args.config)
    paths = artifact_paths(config)
    Path(config["workspace"]).mkdir(parents=True, exist_ok=True)
    Path(config["chunks_dir"]).mkdir(parents=True, exist_ok=True)
    discover(config, paths)
    plan(config, paths)
    plan_data = json.loads(Path(paths["plan"]).read_text())
    chunk_ids = [row["chunk_id"] for row in plan_data["chunks"]]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.max_parallel_chunks) as executor:
        futures = [executor.submit(execute_one, config, paths, chunk_id) for chunk_id in chunk_ids]
        for future in concurrent.futures.as_completed(futures):
            future.result()
    validate(config, paths)
    commit(config, paths)
    compact(config, paths)
    cleanup(config, paths, args.cleanup_orphans)
    print("[OK] production pipeline completed")


if __name__ == "__main__":
    main()
