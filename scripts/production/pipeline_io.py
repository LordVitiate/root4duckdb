#!/usr/bin/env python3
"""Shared deterministic I/O helpers for the production pipeline."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path


def canonical_json_bytes(value: object) -> bytes:
    """Serialize a value for stable hashing."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def sha256_file(path: Path) -> str:
    """Hash a file without loading it into memory."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def detected_memory_bytes() -> int:
    """Return the tightest visible physical-memory limit."""
    values: list[int] = []
    cgroup = Path("/sys/fs/cgroup/memory.max")
    if cgroup.is_file() and (raw := cgroup.read_text().strip()).isdigit():
        values.append(int(raw))
    try:
        values.append(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"))
    except (ValueError, OSError):
        pass
    return min(values) if values else 4 * 1024**3


def write_json_atomic(path: Path, value: object, *, compact: bool = False) -> None:
    """Replace a JSON artifact only after its complete write succeeds."""
    path.parent.mkdir(parents=True, exist_ok=True)
    options = {"sort_keys": True}
    if compact:
        options["separators"] = (",", ":")
    else:
        options["indent"] = 2
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, **options) + "\n")
    os.replace(temporary, path)
