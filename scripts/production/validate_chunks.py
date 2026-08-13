#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from pipeline_io import sha256_file, write_json_atomic


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True)
    parser.add_argument("--chunks-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    plan_path = Path(args.plan).resolve()
    plan = json.loads(plan_path.read_text())
    if plan.get("format") != "root4duckdb-index-plan-v2" or plan.get("index_version") != 12:
        raise SystemExit("unsupported plan")
    chunks_dir = Path(args.chunks_dir)
    expected = {row["chunk_id"]: row for row in plan["chunks"]}
    results: list[dict[str, object]] = []
    failed = False
    seen_sources: set[str] = set()
    schema_fingerprints: set[str] = set()

    dictionary = Path(plan["dictionary"])
    if not dictionary.is_file() or sha256_file(dictionary) != plan["dictionary_fingerprint"]:
        raise SystemExit("dictionary is missing or stale")

    for chunk_id, chunk in expected.items():
        directory = chunks_dir / chunk_id
        success_path = directory / "_SUCCESS.json"
        result: dict[str, object] = {"chunk_id": chunk_id, "state": "missing"}
        if success_path.is_file():
            try:
                success = json.loads(success_path.read_text())
                current = json.loads((directory / "current.json").read_text())
                expected_sources = {row["source_id"] for row in chunk["files"]}
                actual_sources = set(success.get("source_ids", []))
                tables = current.get("tables", {})
                table_paths: list[Path] = []
                checksums = success.get("checksums", {})
                checksum_ok = True
                for value in tables.values():
                    path = Path(value)
                    if not path.is_absolute():
                        path = directory / path
                    table_paths.append(path)
                    rel = str(path.relative_to(directory))
                    checksum_ok &= path.is_file() and checksums.get(rel) == sha256_file(path)
                valid = (
                    success.get("format") == "root4duckdb-chunk-success-v2"
                    and success.get("state") == "succeeded"
                    and success.get("chunk_id") == chunk_id
                    and success.get("plan_fingerprint") == plan["fingerprint"]
                    and success.get("manifest_fingerprint") == plan["manifest_fingerprint"]
                    and success.get("dictionary_fingerprint") == plan["dictionary_fingerprint"]
                    and success.get("snapshot_id") == current.get("snapshot_id")
                    and current.get("index_version") == 12
                    and current.get("chunk_id") == chunk_id
                    and current.get("manifest_fingerprint") == plan["manifest_fingerprint"]
                    and success.get("schema_fingerprint")
                    and success.get("schema_fingerprint") == current.get("schema_fingerprint")
                    and actual_sources == expected_sources
                    and not (actual_sources & seen_sources)
                    and checksum_ok
                )
                if valid:
                    seen_sources.update(actual_sources)
                    schema_fingerprints.add(success["schema_fingerprint"])
                result = {
                    "chunk_id": chunk_id,
                    "state": "succeeded" if valid else "stale",
                    "snapshot_id": current.get("snapshot_id"),
                    "source_ids": sorted(actual_sources),
                    "file_count": len(actual_sources),
                    "table_files": [str(path) for path in table_paths],
                    "checksum_ok": checksum_ok,
                    "schema_fingerprint": success.get("schema_fingerprint"),
                }
            except Exception as exc:
                result = {"chunk_id": chunk_id, "state": "invalid", "error": str(exc)}
        if result["state"] != "succeeded":
            failed = True
        results.append(result)

    all_expected_sources = {row["source_id"] for chunk in plan["chunks"] for row in chunk["files"]}
    if seen_sources != all_expected_sources:
        failed = True
    report = {
        "format": "root4duckdb-validation-v2",
        "plan": str(plan_path),
        "plan_fingerprint": plan["fingerprint"],
        "manifest_fingerprint": plan["manifest_fingerprint"],
        "state": "valid" if not failed else "invalid",
        "expected_chunks": len(expected),
        "succeeded_chunks": sum(row["state"] == "succeeded" for row in results),
        "expected_sources": len(all_expected_sources),
        "validated_sources": len(seen_sources),
        "schema_fingerprints": sorted(schema_fingerprints),
        "chunks": results,
    }
    output = Path(args.output)
    write_json_atomic(output, report)
    print(f"[{report['state'].upper()}] {report['succeeded_chunks']}/{report['expected_chunks']} chunks; "
          f"{report['validated_sources']}/{report['expected_sources']} sources")
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
