"""Common validation primitives."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


@dataclass
class ProjectValidator:
    """Collects repository validation failures for one final report."""

    root: Path
    errors: list[str] = field(default_factory=list)

    def read(self, relative: str) -> str:
        """Read a project file and record a missing-file error."""
        path = self.root / relative
        if not path.is_file():
            self.errors.append(f"missing: {relative}")
            return ""
        return path.read_text()

    def require_files(self, paths: Iterable[str]) -> None:
        """Require every listed project file."""
        for relative in paths:
            if not (self.root / relative).is_file():
                self.errors.append(f"missing: {relative}")

    def require_tokens(self, label: str, text: str, tokens: Iterable[str]) -> None:
        """Require invariant tokens in a source group."""
        for token in tokens:
            if token not in text:
                self.errors.append(f"feature missing in {label}: {token}")

    def reject_tokens(self, label: str, text: str, tokens: Iterable[str]) -> None:
        """Reject obsolete or conflicting tokens in a source group."""
        for token in tokens:
            if token in text:
                self.errors.append(f"forbidden feature in {label}: {token}")

    def source_group(self, *patterns: str) -> str:
        """Join source files selected by project-relative glob patterns."""
        paths = {path for pattern in patterns for path in self.root.glob(pattern) if path.is_file()}
        return "\n".join(path.read_text() for path in sorted(paths))

    def finish(self) -> None:
        """Print the validation summary and fail on collected errors."""
        if self.errors:
            print("VALIDATION FAILED")
            for error in self.errors:
                print(f" - {error}")
            raise SystemExit(1)

        print("VALIDATION OK")
        print(f"Project: {self.root}")
        print("Version: 3.8.0")
        print("Index format: 12")
        print("Iceberg linkage: shared CLI runtime + static loadable extension")
