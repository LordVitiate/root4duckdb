#!/usr/bin/env python3
"""Find or delete expired staging directories."""

from __future__ import annotations

import argparse
import shutil
import time
from pathlib import Path

from pipeline_io import write_json_atomic


def find_orphans(root: Path, cutoff: float) -> list[Path]:
    """Return staging directories older than the supplied epoch."""
    if not root.exists():
        return []
    return [
        item
        for item in sorted(root.iterdir())
        if item.name.startswith(".staging-") and item.is_dir() and item.stat().st_mtime <= cutoff
    ]


def main() -> None:
    """Run a dry-run or explicit orphan cleanup."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--older-than-hours", type=float, default=24)
    parser.add_argument("--delete", action="store_true")
    parser.add_argument("--output")
    args = parser.parse_args()

    root = Path(args.root)
    orphans = find_orphans(root, time.time() - args.older_than_hours * 3600)
    if args.delete:
        for path in orphans:
            shutil.rmtree(path)

    report = {
        "format": "root4duckdb-orphan-cleanup-v1",
        "root": str(root.resolve()),
        "mode": "delete" if args.delete else "dry-run",
        "orphan_count": len(orphans),
        "paths": [str(path) for path in orphans],
    }
    if args.output:
        write_json_atomic(Path(args.output), report)
    print(f"[{report['mode'].upper()}] {len(orphans)} orphan staging directories")


if __name__ == "__main__":
    main()
