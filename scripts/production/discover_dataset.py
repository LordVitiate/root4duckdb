#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import json
import struct
import subprocess
from pathlib import Path
from typing import Iterable

from pipeline_io import canonical_json_bytes, write_json_atomic

FORMAT = "root4duckdb-dataset-manifest-v2"
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def fnv1a(data: bytes, seed: int = FNV_OFFSET) -> int:
    value = seed
    for byte in data:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def source_id(uri: str, size: int, mtime_ns: int) -> str:
    value = fnv1a(uri.encode())
    value = fnv1a(struct.pack("<Q", size), value)
    value = fnv1a(struct.pack("<q", mtime_ns), value)
    return f"{value:016x}"


def is_root_name(value: str) -> bool:
    name = value.rsplit("/", 1)[-1]
    marker = name.rfind(".root")
    if marker < 0:
        return False
    suffix = name[marker + 5 :]
    return not suffix or (suffix.startswith(".") and suffix[1:].isdigit())


def read_list(path: Path) -> list[str]:
    return [line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


def expand_one(spec: str, recursive: bool) -> Iterable[str]:
    spec = spec.strip()
    if not spec:
        return []
    if spec.startswith("["):
        payload = json.loads(spec)
        if not isinstance(payload, list) or not all(isinstance(v, str) for v in payload):
            raise ValueError("JSON input must be an array of strings")
        return [item for nested in payload for item in expand_one(nested, recursive)]
    if spec.startswith("@"):
        return [item for nested in read_list(Path(spec[1:])) for item in expand_one(nested, recursive)]
    if "://" in spec and not spec.startswith("file://"):
        return [spec] if is_root_name(spec) else []
    path = Path(spec.removeprefix("file://")).expanduser()
    if path.is_dir():
        pattern = "**/*.root*" if recursive else "*.root*"
        return [str(item.resolve()) for item in path.glob(pattern) if item.is_file() and is_root_name(item.name)]
    if path.is_file() and path.suffix.lower() in {".txt", ".list", ".manifest", ".uris"}:
        return [item for nested in read_list(path) for item in expand_one(nested, recursive)]
    return [str(Path(item).resolve()) for item in glob.glob(str(path), recursive=recursive)
            if Path(item).is_file() and is_root_name(item)]


def query_uris(duckdb: str, query: str) -> list[str]:
    command = [duckdb, "-csv", "-noheader", ":memory:", "-c", query]
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    rows: list[str] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            rows.append(line.split(",", 1)[0].strip('"'))
    return rows


def file_record(uri: str) -> dict[str, object]:
    if "://" in uri and not uri.startswith("file://"):
        normalized = uri
        size = 0
        mtime_ns = 0
    else:
        path = Path(uri.removeprefix("file://")).resolve()
        stat = path.stat()
        normalized = str(path)
        size = stat.st_size
        mtime_ns = stat.st_mtime_ns
    return {
        "source_id": source_id(normalized, size, mtime_ns),
        "uri": normalized,
        "size": size,
        "mtime_ns": mtime_ns,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", default=[],
                        help="file, directory, glob, JSON array or @URI-list")
    parser.add_argument("--duckdb", help="DuckDB CLI used with --uri-query")
    parser.add_argument("--uri-query", action="append", default=[],
                        help="SQL returning a URI in its first column")
    parser.add_argument("--output", required=True)
    parser.add_argument("--recursive", action="store_true")
    args = parser.parse_args()
    if not args.input and not args.uri_query:
        parser.error("at least one --input or --uri-query is required")
    if args.uri_query and not args.duckdb:
        parser.error("--duckdb is required with --uri-query")

    expanded = [uri for spec in args.input for uri in expand_one(spec, args.recursive)]
    for query in args.uri_query:
        expanded.extend(query_uris(args.duckdb, query))
    uris = sorted(set(expanded))
    if not uris:
        raise SystemExit("no ROOT files discovered")
    records = [file_record(uri) for uri in uris]
    ids = [row["source_id"] for row in records]
    if len(ids) != len(set(ids)):
        raise SystemExit("source_id collision in discovered dataset")
    payload: dict[str, object] = {
        "format": FORMAT,
        "version": 2,
        "files": records,
        "file_count": len(records),
        "total_bytes": sum(int(row["size"]) for row in records),
    }
    payload["fingerprint"] = f"{fnv1a(canonical_json_bytes(payload)):016x}"
    output = Path(args.output)
    write_json_atomic(output, payload)
    print(f"[OK] {len(records)} files -> {output}")
    print(f"[OK] fingerprint={payload['fingerprint']}")


if __name__ == "__main__":
    main()
