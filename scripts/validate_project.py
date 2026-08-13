#!/usr/bin/env python3
"""Run repository structure, pipeline, and syntax checks."""

from __future__ import annotations

from pathlib import Path

from validation.pipeline_checks import check_pipeline
from validation.project_validator import ProjectValidator
from validation.structure_checks import check_structure
from validation.style_checks import check_style


def main() -> None:
    """Validate invariants without building external dependencies."""
    validator = ProjectValidator(Path(__file__).resolve().parents[1])
    check_structure(validator)
    check_pipeline(validator)
    check_style(validator)
    validator.finish()


if __name__ == "__main__":
    main()
