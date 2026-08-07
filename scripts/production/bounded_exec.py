#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def children(pid: int) -> list[int]:
    result: list[int] = []
    task_dir = Path(f"/proc/{pid}/task")
    if not task_dir.exists():
        return result
    seen: set[int] = set()
    for child_file in task_dir.glob("*/children"):
        try:
            values = [int(v) for v in child_file.read_text().split()]
        except (OSError, ValueError):
            continue
        for value in values:
            if value not in seen:
                seen.add(value)
                result.append(value)
    return result


def process_tree(root: int) -> set[int]:
    pending = [root]
    seen: set[int] = set()
    while pending:
        pid = pending.pop()
        if pid in seen:
            continue
        seen.add(pid)
        pending.extend(children(pid))
    return seen


def rss_bytes(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    return 0


def tree_rss(root: int) -> int:
    return sum(rss_bytes(pid) for pid in process_tree(root))


def terminate_group(process: subprocess.Popen[bytes], grace: float) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + grace
    while process.poll() is None and time.monotonic() < deadline:
        time.sleep(0.2)
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a command with an RSS guard and durable report")
    parser.add_argument("--rss-limit-bytes", type=int, required=True)
    parser.add_argument("--poll-seconds", type=float, default=1.0)
    parser.add_argument("--term-grace-seconds", type=float, default=20.0)
    parser.add_argument("--report", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command or args.rss_limit_bytes <= 0:
        parser.error("a command and positive --rss-limit-bytes are required")
    started = time.time_ns()
    process = subprocess.Popen(args.command, start_new_session=True)
    peak = 0
    reason = "exited"
    while process.poll() is None:
        current = tree_rss(process.pid)
        peak = max(peak, current)
        if current > args.rss_limit_bytes:
            reason = "rss_limit_exceeded"
            terminate_group(process, args.term_grace_seconds)
            break
        time.sleep(args.poll_seconds)
    rc = process.wait()
    if reason == "rss_limit_exceeded" and rc == 0:
        rc = 137
    report = {
        "format": "root4duckdb-bounded-exec-v1",
        "command": args.command,
        "state": "succeeded" if rc == 0 else "failed",
        "reason": reason,
        "return_code": rc,
        "rss_limit_bytes": args.rss_limit_bytes,
        "peak_rss_bytes": peak,
        "started_at_ns": started,
        "finished_at_ns": time.time_ns(),
    }
    target = Path(args.report)
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix(target.suffix + ".tmp")
    temp.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    os.replace(temp, target)
    raise SystemExit(rc)


if __name__ == "__main__":
    main()
