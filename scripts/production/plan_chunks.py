#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path

from pipeline_io import canonical_json_bytes, detected_memory_bytes, sha256_file, write_json_atomic

FORMAT = "root4duckdb-index-plan-v2"
INDEX_VERSION = 12


def load_paths(path: Path) -> list[str]:
    data = json.loads(path.read_text())
    if not isinstance(data, list) or not data or not all(isinstance(v, str) and v.startswith("/") for v in data):
        raise ValueError("paths file must contain a non-empty JSON array of absolute logical paths")
    return sorted(set(data))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--tree", required=True)
    parser.add_argument("--paths-file", required=True)
    parser.add_argument("--dictionary", required=True)
    parser.add_argument("--max-files", type=int, default=0,
                        help="explicit file cap; 0 means no hidden file-count limit")
    parser.add_argument("--max-bytes", type=int, default=0,
                        help="explicit byte cap; 0 derives it from a measured rate or leaves it unlimited")
    parser.add_argument("--target-chunk-seconds", type=int, default=0)
    parser.add_argument("--throughput-bytes-per-second", type=int, default=0,
                        help="measured aggregate index throughput of one worker node")
    parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--max-in-flight-files", type=int, default=0)
    parser.add_argument("--memory-budget-bytes", type=int, default=0)
    parser.add_argument("--estimated-worker-bytes", type=int, default=0)
    parser.add_argument("--request-memory-mb", type=int, default=0)
    parser.add_argument("--root-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--duckdb-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--metadata-memory-budget-bytes", type=int, default=0)
    parser.add_argument("--rss-hard-limit-bytes", type=int, default=0)
    parser.add_argument("--output", required=True)
    parser.add_argument("--emit-dir")
    args = parser.parse_args()

    manifest = json.loads(Path(args.manifest).read_text())
    if manifest.get("format") != "root4duckdb-dataset-manifest-v2":
        raise SystemExit("unsupported dataset manifest")
    dictionary = Path(args.dictionary).resolve()
    if not dictionary.is_file():
        raise SystemExit(f"dictionary not found: {dictionary}")
    paths = load_paths(Path(args.paths_file))
    chunk_limits = (
        args.max_files,
        args.max_bytes,
        args.target_chunk_seconds,
        args.throughput_bytes_per_second,
    )
    if any(limit < 0 for limit in chunk_limits):
        raise SystemExit("chunk limits cannot be negative")
    if (args.target_chunk_seconds == 0) != (args.throughput_bytes_per_second == 0):
        raise SystemExit("target chunk seconds and measured throughput must be supplied together")
    detected = detected_memory_bytes()
    if args.request_memory_mb <= 0:
        args.request_memory_mb = max(1, detected // 1_000_000)
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
    if args.max_bytes == 0 and args.target_chunk_seconds:
        args.max_bytes = args.target_chunk_seconds * args.throughput_bytes_per_second
    limits = (args.threads, args.max_in_flight_files, args.memory_budget_bytes,
              args.estimated_worker_bytes, args.request_memory_mb, args.root_memory_budget_bytes,
              args.duckdb_memory_budget_bytes, args.metadata_memory_budget_bytes,
              args.rss_hard_limit_bytes)
    if any(value <= 0 for value in limits):
        raise SystemExit("derived resource budgets must be positive")
    separated = (args.root_memory_budget_bytes + args.duckdb_memory_budget_bytes +
                 args.metadata_memory_budget_bytes)
    if separated > args.memory_budget_bytes:
        raise SystemExit("ROOT + DuckDB + metadata budgets exceed memory_budget_bytes")
    if args.memory_budget_bytes > args.rss_hard_limit_bytes:
        raise SystemExit("memory_budget_bytes must not exceed rss_hard_limit_bytes")

    chunks: list[dict[str, object]] = []
    current: list[dict[str, object]] = []
    current_bytes = 0
    dictionary_fingerprint = sha256_file(dictionary)

    def flush() -> None:
        nonlocal current, current_bytes
        if not current:
            return
        material = {
            "manifest_fingerprint": manifest["fingerprint"],
            "tree": args.tree,
            "paths": paths,
            "dictionary_fingerprint": dictionary_fingerprint,
            "index_version": INDEX_VERSION,
            "files": [row["source_id"] for row in current],
        }
        chunk_id = hashlib.sha256(canonical_json_bytes(material)).hexdigest()[:24]
        chunks.append({
            "chunk_id": chunk_id,
            "state": "planned",
            "files": current,
            "file_count": len(current),
            "estimated_bytes": current_bytes,
        })
        current = []
        current_bytes = 0

    for row in manifest["files"]:
        size = int(row.get("size", 0))
        file_cap = args.max_files > 0 and len(current) >= args.max_files
        byte_cap = args.max_bytes > 0 and current_bytes + size > args.max_bytes
        if current and (file_cap or byte_cap):
            flush()
        current.append(row)
        current_bytes += size
    flush()

    plan: dict[str, object] = {
        "format": FORMAT,
        "version": 2,
        "index_version": INDEX_VERSION,
        "manifest": str(Path(args.manifest).resolve()),
        "manifest_fingerprint": manifest["fingerprint"],
        "tree": args.tree,
        "logical_paths": paths,
        "dictionary": str(dictionary),
        "dictionary_fingerprint": dictionary_fingerprint,
        "max_files": args.max_files,
        "max_bytes": args.max_bytes,
        "target_chunk_seconds": args.target_chunk_seconds,
        "throughput_bytes_per_second": args.throughput_bytes_per_second,
        "threads": args.threads,
        "max_in_flight_files": args.max_in_flight_files,
        "memory_budget_bytes": args.memory_budget_bytes,
        "estimated_worker_bytes": args.estimated_worker_bytes,
        "request_memory_mb": args.request_memory_mb,
        "root_memory_budget_bytes": args.root_memory_budget_bytes,
        "duckdb_memory_budget_bytes": args.duckdb_memory_budget_bytes,
        "metadata_memory_budget_bytes": args.metadata_memory_budget_bytes,
        "rss_hard_limit_bytes": args.rss_hard_limit_bytes,
        "chunks": chunks,
        "chunk_count": len(chunks),
    }
    plan["fingerprint"] = hashlib.sha256(canonical_json_bytes(plan)).hexdigest()
    output = Path(args.output)
    write_json_atomic(output, plan)
    if args.emit_dir:
        emit = Path(args.emit_dir)
        emit.mkdir(parents=True, exist_ok=True)
        for chunk in chunks:
            (emit / f"{chunk['chunk_id']}.uris").write_text(
                "".join(f"{row['uri']}\n" for row in chunk["files"])
            )
    print(f"[OK] {len(chunks)} chunks -> {output}")
    print(f"[OK] fingerprint={plan['fingerprint']}")
    if args.max_files == 0 and args.max_bytes == 0:
        print("[WARN] no chunk cap was supplied: the plan intentionally contains one node-sized chunk")


if __name__ == "__main__":
    main()
