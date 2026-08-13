#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import time
from pathlib import Path

from pipeline_io import detected_memory_bytes


def quote_condor(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def success_valid(chunks_dir: Path, chunk_id: str, plan: dict[str, object]) -> bool:
    path = chunks_dir / chunk_id / "_SUCCESS.json"
    if not path.is_file():
        return False
    try:
        payload = json.loads(path.read_text())
    except Exception:
        return False
    return (payload.get("state") == "succeeded" and payload.get("chunk_id") == chunk_id and
            payload.get("plan_fingerprint") == plan.get("fingerprint"))


def worker_args(args: argparse.Namespace) -> list[str]:
    values = [
        args.worker,
        "--plan", str(Path(args.plan).resolve()),
        "--chunk-id", args.chunk_id,
        "--output-root", str(Path(args.chunks_dir).resolve()),
        "--project-dir", str(Path(args.project_dir).resolve()),
        "--duckdb", str(Path(args.duckdb).resolve()),
        "--threads", str(args.threads),
        "--max-in-flight-files", str(args.max_in_flight_files),
        "--memory-budget-bytes", str(args.memory_budget_bytes),
        "--estimated-worker-bytes", str(args.estimated_worker_bytes),
        "--rss-hard-limit-bytes", str(args.rss_hard_limit_bytes),
        "--root-memory-budget-bytes", str(args.root_memory_budget_bytes),
        "--duckdb-memory-budget-bytes", str(args.duckdb_memory_budget_bytes),
        "--metadata-memory-budget-bytes", str(args.metadata_memory_budget_bytes),
    ]
    return values


def run_local(command: list[str]) -> None:
    subprocess.run(command, check=True)


def run_condor(command: list[str], args: argparse.Namespace) -> None:
    work = Path(args.backend_work_dir).resolve() / args.chunk_id / f"attempt-{time.time_ns()}"
    work.mkdir(parents=True, exist_ok=True)
    log = work / "job.log"
    submit = work / "job.sub"
    submit.write_text("\n".join([
        "universe = vanilla",
        f"executable = {quote_condor(command[0])}",
        "arguments = " + " ".join(quote_condor(v) for v in command[1:]),
        f"output = {quote_condor(str(work / 'stdout.log'))}",
        f"error = {quote_condor(str(work / 'stderr.log'))}",
        f"log = {quote_condor(str(log))}",
        f"request_cpus = {args.request_cpus}",
        f"request_memory = {args.request_memory_mb}MB",
        "getenv = True",
        "should_transfer_files = NO",
        f"+JobFlavour = {quote_condor(args.job_flavour)}",
        "queue 1",
        "",
    ]))
    subprocess.run([args.condor_submit, "-terse", str(submit)], check=True)
    wait = subprocess.run([args.condor_wait, str(log)])
    if wait.returncode != 0:
        raise RuntimeError(f"condor_wait failed for chunk {args.chunk_id}: {wait.returncode}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Execute one idempotent index chunk")
    parser.add_argument("--backend", choices=("local", "htcondor"), required=True)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--chunk-id", required=True)
    parser.add_argument("--chunks-dir", required=True)
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--duckdb", required=True)
    parser.add_argument("--worker")
    parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--max-in-flight-files", type=int, default=0)
    parser.add_argument("--memory-budget-bytes", type=int, default=0)
    parser.add_argument("--estimated-worker-bytes", type=int, default=0)
    parser.add_argument("--rss-hard-limit-bytes", type=int, default=0)
    parser.add_argument("--root-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--duckdb-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--metadata-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--backend-work-dir", default="work/condor")
    parser.add_argument("--request-cpus", type=int, default=0)
    parser.add_argument("--request-memory-mb", type=int, default=0)
    parser.add_argument("--job-flavour", default="workday")
    parser.add_argument("--condor-submit", default="condor_submit")
    parser.add_argument("--condor-wait", default="condor_wait")
    args = parser.parse_args()
    if args.request_cpus <= 0:
        args.request_cpus = args.threads
    if args.request_memory_mb <= 0:
        args.request_memory_mb = max(1, detected_memory_bytes() // 1_000_000)
    request_bytes = args.request_memory_mb * 1_000_000
    if args.rss_hard_limit_bytes <= 0:
        args.rss_hard_limit_bytes = request_bytes * 95 // 100
    if args.memory_budget_bytes <= 0:
        args.memory_budget_bytes = request_bytes * 85 // 100
    if args.estimated_worker_bytes <= 0:
        args.estimated_worker_bytes = 256 * 1024 * 1024
    if args.root_memory_budget_bytes <= 0:
        args.root_memory_budget_bytes = args.memory_budget_bytes * 50 // 100
    if args.duckdb_memory_budget_bytes <= 0:
        args.duckdb_memory_budget_bytes = args.memory_budget_bytes * 40 // 100
    if args.metadata_memory_budget_bytes <= 0:
        args.metadata_memory_budget_bytes = (args.memory_budget_bytes - args.root_memory_budget_bytes -
                                             args.duckdb_memory_budget_bytes)
    if args.max_in_flight_files <= 0:
        args.max_in_flight_files = max(1, min(
            args.threads, args.root_memory_budget_bytes // args.estimated_worker_bytes))
    project = Path(args.project_dir).resolve()
    args.worker = args.worker or str(project / "scripts/production/index_chunk.sh")
    plan = json.loads(Path(args.plan).read_text())
    if args.chunk_id not in {row["chunk_id"] for row in plan["chunks"]}:
        raise SystemExit(f"unknown chunk_id: {args.chunk_id}")
    chunks = Path(args.chunks_dir).resolve()
    if success_valid(chunks, args.chunk_id, plan):
        print(f"[OK] chunk already complete: {args.chunk_id}")
        return
    command = worker_args(args)
    started = time.monotonic()
    if args.backend == "local":
        run_local(command)
    else:
        run_condor(command, args)
    if not success_valid(chunks, args.chunk_id, plan):
        raise SystemExit(f"chunk finished without valid _SUCCESS: {args.chunk_id}")
    print(f"[OK] {args.backend} chunk {args.chunk_id} completed in {time.monotonic() - started:.1f}s")


if __name__ == "__main__":
    main()
