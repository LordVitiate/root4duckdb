#!/usr/bin/env python3
"""Read-only direct/indexed ROOT parallelism benchmark (DuckDB 2.0).

The materialized CTE is intentional: it exercises DuckDB's CTE sink.
No index rebuild and no changes to the ROOT files or production DB.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import resource
import subprocess
import sys
import time


def q(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def query(args, reader: str, workers: int, ordered: bool, profile: Path) -> str:
    header = f"""
LOAD {q(str(args.extension))};
SET threads={workers};
SET preserve_insertion_order={'true' if ordered else 'false'};
SET root_max_in_flight_files={args.max_in_flight_files};
SET root_memory_limit={q(args.root_memory_limit)};
PRAGMA enable_profiling='json';
SET profiling_output={q(str(profile))};
"""
    if reader == "direct":
        return header + f"""
WITH sampled AS MATERIALIZED (
    SELECT entry_id, vecTrack_idx, chi2tot
    FROM read_root(
        {q(args.direct_root)},
        dictionary := {q(args.dictionary)},
        path_prefix := '/PaEvent/vecTrack',
        reader_mode := 'auto'
    )
    WHERE entry_id < {args.direct_entries}
)
SELECT count(*)::BIGINT AS rows, 1::BIGINT AS files,
       bit_xor(hash(entry_id, vecTrack_idx, chi2tot)) AS row_hash,
       sum(chi2tot::DOUBLE) AS value_sum
FROM sampled;
"""
    # Do not specify max_open_files := 2: it would cap the worker limit.
    return header + f"""
WITH sampled AS MATERIALIZED (
    SELECT source_id, entry_id, vecTrack_idx, Cov_idx, value
    FROM read_root_dataset(
        {q(args.catalog)}, '/PaEvent/vecTrack/vecTPar/Cov',
        dictionary := {q(args.dictionary)}, reader_mode := 'auto'
    )
    WHERE entry_id < {args.indexed_entries} AND Cov_idx IN (0, 3, 5)
)
SELECT count(*)::BIGINT AS rows, count(DISTINCT source_id)::BIGINT AS files,
       bit_xor(hash(source_id, entry_id, vecTrack_idx, Cov_idx, value)) AS row_hash,
       sum(value::DOUBLE) AS value_sum
FROM sampled;
"""


def threads_cpu(pid: int) -> tuple[dict[int, int], int]:
    """CPU jiffies for each Linux TID, including dynamically created workers."""
    taskdir = Path(f"/proc/{pid}/task")
    try:
        tasks = list(taskdir.iterdir())
    except OSError:
        return {}, 0
    values = {}
    for task in tasks:
        try:
            fields = (task / "stat").read_text().rsplit(") ", 1)[1].split()
            values[int(task.name)] = int(fields[11]) + int(fields[12])
        except (OSError, IndexError, ValueError):
            continue
    return values, len(tasks)


def resident_high_water(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    except OSError:
        pass
    return 0


def extra_info(profile: Path) -> dict:
    try:
        document = json.loads(profile.read_text())
    except (OSError, ValueError):
        return {}
    found = {}

    def visit(node):
        if isinstance(node, dict):
            values = node.get("extra_info", {})
            if isinstance(values, dict):
                for field in ("ROOT Worker Limit", "Worker Limit", "Opened ROOT Files",
                              "Decoded Values", "Coalesced Read Tasks", "File-affinity Groups"):
                    if field in values:
                        found[field] = values[field]
            for child in node.values():
                visit(child)
        elif isinstance(node, list):
            for child in node:
                visit(child)

    visit(document)
    return found


def run(args, output: Path, reader: str, workers: int, ordered: bool) -> dict:
    label = f"{reader}-threads{workers}-order{int(ordered)}"
    sql = output / f"{label}.sql"
    profile = output / f"{label}.profile.json"
    result = output / f"{label}.csv"
    errors = output / f"{label}.stderr"
    sql.write_text(query(args, reader, workers, ordered, profile))
    begin = time.monotonic()
    old_usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    peak_threads = peak_active = peak_rss = 0
    tick_rate = os.sysconf("SC_CLK_TCK")
    with sql.open("rb") as source, result.open("wb") as stdout, errors.open("wb") as stderr:
        process = subprocess.Popen(
            [str(args.runner), "-unsigned", "-bail", "-csv", "-noheader", ":memory:"],
            cwd=args.repo, stdin=source, stdout=stdout, stderr=stderr
        )
        previous = {}
        last_sample = begin
        while process.poll() is None:
            current = time.monotonic()
            jiffies, thread_count = threads_cpu(process.pid)
            peak_threads = max(peak_threads, thread_count)
            peak_rss = max(peak_rss, resident_high_water(process.pid))
            delta = current - last_sample
            if previous and delta > 0:
                active = sum(
                    1 for tid, ticks in jiffies.items()
                    if (ticks - previous.get(tid, ticks)) / (tick_rate * delta) >= 0.2
                )
                peak_active = max(peak_active, active)
            previous, last_sample = jiffies, current
            time.sleep(0.25)
        status = process.wait()
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    with result.open(newline="") as handle:
        values = next(csv.reader(handle), [])
    metrics = extra_info(profile)
    record = {
        "case": label, "exit": status, "wall_s": round(time.monotonic() - begin, 3),
        "cpu_user_s": round(usage.ru_utime - old_usage.ru_utime, 3),
        "cpu_system_s": round(usage.ru_stime - old_usage.ru_stime, 3),
        "peak_rss_kb": peak_rss, "peak_linux_threads": peak_threads,
        "peak_active_threads": peak_active,
        "admitted_root_workers": metrics.get("ROOT Worker Limit", metrics.get("Worker Limit", "")),
        "opened_root_files": metrics.get("Opened ROOT Files", ""),
        "decoded_root_values": metrics.get("Decoded Values", ""),
        "rows": values[0] if len(values) == 4 else "",
        "files": values[1] if len(values) == 4 else "",
        "row_hash": values[2] if len(values) == 4 else "",
        "value_sum": values[3] if len(values) == 4 else "",
    }
    print(json.dumps(record), flush=True)
    if status or len(values) != 4:
        print(f"[FAIL] {label}: exit={status}, result={values}, diagnostic={errors}",
              file=sys.stderr, flush=True)
        try:
            message = errors.read_text(errors="replace").strip()
        except OSError as exc:
            message = str(exc)
        if message:
            print(message[-12000:], file=sys.stderr, flush=True)
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--mode", choices=("direct", "indexed", "both"), default="both")
    parser.add_argument("--threads", default="1,4,14")
    parser.add_argument("--order", choices=("true", "false", "both"), default="true")
    parser.add_argument("--direct-root", default="/eos/experiment/compass/data/2022/W08/slot1/mDST/mDST-297667-1-8.root")
    parser.add_argument("--catalog", default="/eos/experiment/compass/scratch/SSS/bakur_hodo/all_periods/hodo_w02_100/cov_iceberg")
    parser.add_argument("--dictionary", default="/eos/experiment/compass/scratch/SSS/ontop-test/phast.8.051/lib/libPhast.so")
    parser.add_argument("--direct-entries", type=int, default=1048576)
    parser.add_argument("--indexed-entries", type=int, default=128)
    parser.add_argument("--max-in-flight-files", type=int, default=14)
    parser.add_argument("--root-memory-limit", default="4GB")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    args.repo = args.repo.resolve()
    args.runner = args.repo / "run-duckdb.sh"
    args.extension = args.repo / "build/release/extension/root/root.duckdb_extension"
    readers = ("direct", "indexed") if args.mode == "both" else (args.mode,)
    for path in (args.runner, args.extension, Path(args.dictionary)):
        if not path.is_file():
            parser.error(f"missing required file: {path}")
    if "direct" in readers and not Path(args.direct_root).is_file():
        parser.error(f"direct ROOT file not found: {args.direct_root}")
    if "indexed" in readers and not Path(args.catalog).exists():
        parser.error(f"indexed catalog not found: {args.catalog}")
    workers = [int(part) for part in args.threads.split(",")]
    if not workers or min(workers) < 1 or args.direct_entries < 1 or args.indexed_entries < 1:
        parser.error("thread counts and entry counts must be positive")
    orders = (True, False) if args.order == "both" else (args.order == "true",)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = args.out or Path(f"/tmp/root4duckdb-{os.getuid()}/bench-parallel-{stamp}")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    print(f"[BENCH] {output} modes={readers} threads={workers} order={orders}", flush=True)
    all_ok = True
    reference = {}
    summary = output / "summary.tsv"
    with summary.open("w", newline="") as file:
        writer = None
        for reader in readers:
            for ordered in orders:
                for worker_count in workers:
                    row = run(args, output, reader, worker_count, ordered)
                    if writer is None:
                        writer = csv.DictWriter(file, fieldnames=list(row), delimiter="\t")
                        writer.writeheader()
                    writer.writerow(row)
                    file.flush()
                    key = (reader, ordered)
                    check = (row["rows"], row["files"], row["row_hash"])
                    if row["exit"] or not row["rows"]:
                        all_ok = False
                        print("[STOP] query failed; remaining thread settings would not "
                              "produce meaningful measurements", file=sys.stderr, flush=True)
                        return 1
                    elif key in reference and reference[key] != check:
                        print(f"[MISMATCH] {key}: {check} != {reference[key]}", file=sys.stderr)
                        all_ok = False
                    else:
                        reference[key] = check
    print(f"[BENCH] saved {summary}", flush=True)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
