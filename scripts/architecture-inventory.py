#!/usr/bin/env python3
"""Inventory project-owned C++ types and files without external build dependencies.

Lexical inventory: forward declarations excluded; local/nested types counted.
A Clang AST audit is required for a fully semantic class/struct analysis.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path

TYPE = re.compile(
    r"^[ \t]*(class|struct|enum(?:[ \t]+class)?)"
    r"[ \t]+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)(.*)$"
)


def source_module(relative: Path) -> str:
    parts = relative.parts
    if len(parts) >= 3 and parts[:2] == ("include", "root4duckdb"):
        return parts[2]
    if len(parts) >= 3 and parts[0] == "src":
        return parts[1]
    return "root"


def type_definitions(lines: list[str]) -> list[dict[str, object]]:
    """Find definitions, ignoring forward declarations and 'struct stat' variables."""
    found: list[dict[str, object]] = []
    for number, line in enumerate(lines):
        match = TYPE.match(line)
        if match is None:
            continue
        kind, name, suffix = match.groups()
        suffix = suffix.strip()
        if not suffix:
            suffix = next(
                (text.strip() for text in lines[number + 1:number + 7]
                 if text.strip() and not text.lstrip().startswith("//")),
                "",
            )
        if not re.match(r"^(?:\{|:|final\b)", suffix):
            continue
        found.append({
            "kind": "enum" if kind.startswith("enum") else kind,
            "name": name,
            "line": number + 1,
        })
    return found


def inventory(root: Path) -> dict[str, object]:
    source_root = root / "src"
    include_root = root / "include"
    for directory in (source_root, include_root):
        if not directory.is_dir():
            raise ValueError(f"missing source directory: {directory}")
    all_files = sorted(path for directory in (source_root, include_root)
                       for path in directory.rglob("*") if path.is_file())
    files: Counter[str] = Counter()
    types: Counter[str] = Counter()
    locations = {"cpp": Counter(), "hpp": Counter(), "tpp": Counter()}
    modules: dict[str, Counter[str]] = {}
    local_types = []
    largest = []
    for path in all_files:
        suffix = path.suffix
        if suffix not in (".cpp", ".hpp", ".tpp"):
            files["other"] += 1
            continue
        category = suffix.removeprefix(".")
        files[category] += 1
        relative = path.relative_to(root)
        lines = path.read_text(encoding="utf-8").splitlines()
        largest.append({"path": relative.as_posix(), "lines": len(lines)})
        module = modules.setdefault(source_module(relative), Counter())
        module[category] += 1
        for item in type_definitions(lines):
            kind = str(item["kind"])
            types[kind] += 1
            locations[category][kind] += 1
            module[kind] += 1
            if suffix == ".cpp":
                local_types.append({"path": relative.as_posix(), **item})
    return {
        "project_files": len(all_files),
        "file_counts": dict(sorted(files.items())),
        "type_definitions": dict(sorted(types.items())),
        "type_locations": {key: dict(sorted(value.items())) for key, value in locations.items()},
        "modules": {key: dict(sorted(value.items())) for key, value in sorted(modules.items())},
        "cpp_local_types": local_types,
        "largest_files": sorted(largest, key=lambda item: (-item["lines"], item["path"]))[:12],
        "method": "lexical definitions; includes nested/private types; excludes forward declarations",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    report = inventory(args.root)
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return
    print(f"Project-owned src/ and include/ files: {report['project_files']}")
    for kind, count in report["file_counts"].items():
        print(f"  {kind}: {count}")
    print("Type definitions:")
    for kind, count in report["type_definitions"].items():
        h = report["type_locations"]["hpp"].get(kind, 0)
        c = report["type_locations"]["cpp"].get(kind, 0)
        print(f"  {kind}: {count} (headers: {h}; sources: {c})")
    print("Largest files:")
    for item in report["largest_files"]:
        print(f"  {item['lines']:4} {item['path']}")
    print("Implementation-local type definitions (review, not necessarily violations):")
    for item in report["cpp_local_types"]:
        print(f"  {item['path']}:{item['line']} {item['kind']} {item['name']}")


if __name__ == "__main__":
    main()
