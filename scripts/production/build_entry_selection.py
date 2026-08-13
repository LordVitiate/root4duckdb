#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

from pipeline_io import write_json_atomic


def encode(values: list[int], min_run: int) -> dict[str, object]:
    values = sorted(set(values))
    ranges: list[list[int]] = []
    sparse: list[int] = []
    i = 0
    while i < len(values):
        j = i + 1
        while j < len(values) and values[j] == values[j - 1] + 1:
            j += 1
        if j - i >= min_run:
            ranges.append([values[i], values[j - 1] + 1])
        else:
            sparse.extend(values[i:j])
        i = j
    result: dict[str, object] = {}
    if ranges:
        result["ranges"] = ranges
    if sparse:
        result["entries_delta"] = {
            "base": sparse[0],
            "deltas": [right - left for left, right in zip(sparse, sparse[1:])],
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description="Build adaptive exact entry-selection JSON")
    parser.add_argument("--input", required=True, help="CSV/TSV with source_id and entry_id")
    parser.add_argument("--output", required=True)
    parser.add_argument("--delimiter", default=",")
    parser.add_argument("--source-column", default="source_id")
    parser.add_argument("--entry-column", default="entry_id")
    parser.add_argument("--min-run", type=int, default=4)
    args = parser.parse_args()
    if args.min_run < 2:
        parser.error("--min-run must be at least 2")
    grouped: dict[str, list[int]] = defaultdict(list)
    with Path(args.input).open(newline="") as handle:
        reader = csv.DictReader(handle, delimiter=args.delimiter)
        if (
            not reader.fieldnames
            or args.source_column not in reader.fieldnames
            or args.entry_column not in reader.fieldnames
        ):
            raise SystemExit("input is missing source_id/entry_id columns")
        for line, row in enumerate(reader, 2):
            source = (row.get(args.source_column) or "").strip()
            if not source:
                raise SystemExit(f"empty source_id at line {line}")
            value = int(row[args.entry_column])
            if value < 0 or value >= 2**64 - 1:
                raise SystemExit(f"entry_id out of range at line {line}")
            grouped[source].append(value)
    payload = {source: encode(values, args.min_run) for source, values in sorted(grouped.items())}
    output = Path(args.output)
    write_json_atomic(output, payload, compact=True)
    raw_count = sum(len(values) for values in grouped.values())
    range_count = sum(len(value.get("ranges", [])) for value in payload.values())
    print(f"[OK] {raw_count} entries, {len(grouped)} sources, {range_count} dense ranges -> {output}")


if __name__ == "__main__":
    main()
