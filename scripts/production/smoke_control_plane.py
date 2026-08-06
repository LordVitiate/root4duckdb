#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
test_dir = root / "test" / "production"
methods = [
    "test_adaptive_entry_selection",
    "test_bounded_exec_report",
    "test_config_rejects_overcommitted_budgets",
    "test_dagster_config_defaults",
    "test_dagster_is_orchestrator_not_dagman",
    "test_discovery_and_plan_are_deterministic",
    "test_local_backend_is_idempotent",
]
env = os.environ.copy()
env["PYTHONDONTWRITEBYTECODE"] = "1"
for method in methods:
    subprocess.run(
        [sys.executable, "-m", "unittest", f"test_control_plane.ControlPlaneTests.{method}", "-v"],
        cwd=test_dir,
        check=True,
        env=env,
        timeout=120,
    )
print(f"[OK] production control-plane smoke tests: {len(methods)}/{len(methods)}")
