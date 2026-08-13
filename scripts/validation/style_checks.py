"""Language-level style and syntax checks."""

from __future__ import annotations

import re
import subprocess

from validation.project_validator import ProjectValidator


def check_style(validator: ProjectValidator) -> None:
    """Validate C++ boundaries plus Python and shell syntax."""
    _check_cpp(validator)
    _check_python(validator)
    _check_shell(validator)
    _check_executables(validator)


def _check_cpp(validator: ProjectValidator) -> None:
    """Apply useful JSF-derived header and source hygiene rules."""
    sources = list((validator.root / "src").rglob("*.cpp")) + list((validator.root / "src").rglob("*.hpp"))
    for source in sources:
        text = source.read_text()
        relative = source.relative_to(validator.root)
        for line_number, line in enumerate(text.splitlines(), 1):
            if len(line) > 120:
                validator.errors.append(f"C++ line exceeds 120 columns: {relative}:{line_number}")
        if source.suffix == ".hpp" and not text.startswith("#pragma once"):
            validator.errors.append(f"header does not use #pragma once: {relative}")
        if re.search(r"\bstatic\s+inline\b", text):
            validator.errors.append(f"static-inline implementation remains: {relative}")
        if source.suffix == ".hpp" and source.name != "root_iceberg_internal.hpp" and re.search(
            r"\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{", text
        ):
            validator.errors.append(f"non-template implementation remains in header: {relative}")
        if re.search(r"^\s*#ifndef\s+\w+_H(?:PP)?_?\s*$", text, re.MULTILINE):
            validator.errors.append(f"legacy header guard remains: {relative}")
        if re.search(r"^\s*(?:if|for|while)\s*\([^\n]+\)\s*$", text, re.MULTILINE):
            validator.errors.append(f"unbraced control statement remains: {relative}")
        if source.name != "root_headers.hpp" and re.search(
            r'^\s*#include\s*[<"](?:Rtypes|T[A-Za-z0-9_]*\.h)', text, re.MULTILINE
        ):
            validator.errors.append(f"ROOT header bypasses root_headers.hpp: {relative}")
        if re.search(r"/(?:home|Users)/", text):
            validator.errors.append(f"machine-specific path: {relative}")
        if re.search(r"^\s*//\s*={4,}", text, re.MULTILINE):
            validator.errors.append(f"decorative separator comment: {relative}")
        if re.search(r"//[^\n]*[А-Яа-яЁё]", text):
            validator.errors.append(f"untranslated source comment: {relative}")


def _check_python(validator: ProjectValidator) -> None:
    """Compile every Python control-plane and test module."""
    scripts = list((validator.root / "scripts").rglob("*.py")) + [
        validator.root / "orchestration/dagster/definitions.py",
        validator.root / "test/production/test_control_plane.py",
    ]
    for script in scripts:
        if not script.is_file():
            continue
        try:
            compile(script.read_text(), str(script), "exec")
        except Exception as exc:  # pragma: no cover - diagnostic path
            validator.errors.append(f"Python syntax failed: {script.relative_to(validator.root)}: {exc}")


def _check_shell(validator: ProjectValidator) -> None:
    """Parse every shell entry point with Bash."""
    scripts = list(validator.root.glob("*.sh")) + list((validator.root / "scripts").rglob("*.sh"))
    for script in scripts:
        try:
            subprocess.run(["bash", "-n", str(script)], check=True, capture_output=True, text=True)
        except subprocess.CalledProcessError as exc:
            message = exc.stderr.strip() or exc.stdout.strip()
            validator.errors.append(f"shell syntax failed: {script.relative_to(validator.root)}: {message}")


def _check_executables(validator: ProjectValidator) -> None:
    """Require executable bits on public command entry points."""
    scripts = (
        "build-iceberg.sh",
        "build-root4duckdb.sh",
        "setup-source-tree.sh",
        "run-duckdb.sh",
        "scripts/check-iceberg.sh",
    )
    for relative in scripts:
        path = validator.root / relative
        if path.is_file() and not path.stat().st_mode & 0o111:
            validator.errors.append(f"script is not executable: {relative}")
