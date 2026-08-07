from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROD = ROOT / "scripts/production"


class ControlPlaneTests(unittest.TestCase):
    def run_ok(self, *args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, *args], cwd=cwd or ROOT, check=True, text=True,
                              capture_output=True)

    def test_discovery_and_plan_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            data = temp / "data"
            data.mkdir()
            for name, size in (("a.root", 5), ("a.root.001", 7), ("b.root", 11)):
                (data / name).write_bytes(b"x" * size)
            (data / "ignore.txt").write_text("x")
            manifest1 = temp / "manifest1.json"
            manifest2 = temp / "manifest2.json"
            for target in (manifest1, manifest2):
                self.run_ok(str(PROD / "discover_dataset.py"), "--input", str(data / "*.root*"),
                            "--input", str(data / "a.root"), "--output", str(target))
            first = json.loads(manifest1.read_text())
            second = json.loads(manifest2.read_text())
            self.assertEqual(first["fingerprint"], second["fingerprint"])
            self.assertEqual(first["file_count"], 3)
            self.assertEqual([Path(row["uri"]).name for row in first["files"]],
                             ["a.root", "a.root.001", "b.root"])
            dictionary = temp / "dictionary.so"
            dictionary.write_bytes(b"dictionary")
            paths = temp / "paths.json"
            paths.write_text('["/PaEvent/vecParticle/flags","/PaEvent/vecHeader/value"]')
            plan1 = temp / "plan1.json"
            plan2 = temp / "plan2.json"
            base = [str(PROD / "plan_chunks.py"), "--manifest", str(manifest1), "--tree", "PaEvent",
                    "--paths-file", str(paths), "--dictionary", str(dictionary), "--max-files", "2",
                    "--max-bytes", "100", "--output"]
            self.run_ok(*base, str(plan1))
            self.run_ok(*base, str(plan2))
            one = json.loads(plan1.read_text())
            two = json.loads(plan2.read_text())
            self.assertEqual(one["fingerprint"], two["fingerprint"])
            self.assertEqual([row["chunk_id"] for row in one["chunks"]],
                             [row["chunk_id"] for row in two["chunks"]])
            self.assertEqual(one["chunk_count"], 2)

    def test_adaptive_entry_selection(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "entries.csv"
            source.write_text("source_id,entry_id\na,1\na,2\na,3\na,4\na,10\na,20\nb,7\n")
            output = temp / "selection.json"
            self.run_ok(str(PROD / "build_entry_selection.py"), "--input", str(source),
                        "--output", str(output), "--min-run", "4")
            payload = json.loads(output.read_text())
            self.assertEqual(payload["a"]["ranges"], [[1, 5]])
            self.assertEqual(payload["a"]["entries_delta"], {"base": 10, "deltas": [10]})
            self.assertEqual(payload["b"]["entries_delta"], {"base": 7, "deltas": []})

    def test_plan_has_no_hidden_file_or_byte_cap(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            files = []
            for index in range(12):
                path = temp / f"{index:02d}.root"
                path.write_bytes(b"x" * 10)
                files.append({"uri": str(path), "source_id": f"s{index}", "size": 10})
            manifest = temp / "manifest.json"
            manifest.write_text(json.dumps({
                "format": "root4duckdb-dataset-manifest-v2", "fingerprint": "manifest",
                "files": files,
            }))
            dictionary = temp / "dictionary.so"; dictionary.write_bytes(b"dictionary")
            paths = temp / "paths.json"; paths.write_text('["/PaEvent/x"]')
            plan_path = temp / "plan.json"
            result = self.run_ok(str(PROD / "plan_chunks.py"), "--manifest", str(manifest),
                                 "--tree", "PaEvent", "--paths-file", str(paths),
                                 "--dictionary", str(dictionary), "--threads", "4",
                                 "--request-memory-mb", "2000", "--output", str(plan_path))
            plan = json.loads(plan_path.read_text())
            self.assertEqual(plan["max_files"], 0)
            self.assertEqual(plan["max_bytes"], 0)
            self.assertEqual(plan["chunk_count"], 1)
            self.assertGreaterEqual(plan["max_in_flight_files"], 1)
            self.assertIn("one node-sized chunk", result.stdout)

    def test_plan_derives_chunk_bytes_from_measured_rate(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            manifest = temp / "manifest.json"
            manifest.write_text(json.dumps({
                "format": "root4duckdb-dataset-manifest-v2", "fingerprint": "manifest",
                "files": [
                    {"uri": str(temp / f"{index}.root"), "source_id": f"s{index}", "size": 60}
                    for index in range(4)
                ],
            }))
            dictionary = temp / "dictionary.so"; dictionary.write_bytes(b"dictionary")
            paths = temp / "paths.json"; paths.write_text('["/PaEvent/x"]')
            plan_path = temp / "plan.json"
            self.run_ok(str(PROD / "plan_chunks.py"), "--manifest", str(manifest),
                        "--tree", "PaEvent", "--paths-file", str(paths),
                        "--dictionary", str(dictionary), "--target-chunk-seconds", "10",
                        "--throughput-bytes-per-second", "10", "--request-memory-mb", "2000",
                        "--output", str(plan_path))
            plan = json.loads(plan_path.read_text())
            self.assertEqual(plan["max_bytes"], 100)
            self.assertEqual(plan["chunk_count"], 4)

    def test_config_rejects_overcommitted_budgets(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            paths = temp / "paths.json"; paths.write_text('["/x"]')
            dictionary = temp / "dictionary.so"; dictionary.write_bytes(b"x")
            config = {
                "format": "root4duckdb-production-config-v1", "project_dir": str(ROOT),
                "inputs": [str(temp / "*.root")], "tree": "PaEvent",
                "paths_file": str(paths), "dictionary": str(dictionary),
                "resources": {"memory_budget_bytes": 10, "root_memory_budget_bytes": 6,
                              "duckdb_memory_budget_bytes": 6, "metadata_memory_budget_bytes": 1},
            }
            config_path = temp / "config.json"; config_path.write_text(json.dumps(config))
            script = "from pipeline_config import load_config; load_config(r'%s')" % config_path
            result = subprocess.run([sys.executable, "-c", script], cwd=PROD, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("budgets exceed", result.stderr)

    def test_htcondor_requires_worker_resources_and_chunk_calibration(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            paths = temp / "paths.json"; paths.write_text('["/x"]')
            dictionary = temp / "dictionary.so"; dictionary.write_bytes(b"x")
            base = {
                "format": "root4duckdb-production-config-v1", "project_dir": str(ROOT),
                "inputs": [str(temp / "*.root")], "tree": "PaEvent",
                "paths_file": str(paths), "dictionary": str(dictionary),
                "execution_backend": "htcondor",
            }
            config_path = temp / "config.json"; config_path.write_text(json.dumps(base))
            script = "from pipeline_config import load_config; load_config(r'%s')" % config_path
            missing_memory = subprocess.run([sys.executable, "-c", script], cwd=PROD,
                                            text=True, capture_output=True)
            self.assertNotEqual(missing_memory.returncode, 0)
            self.assertIn("request_memory_mb is required", missing_memory.stderr)
            base["resources"] = {"request_memory_mb": 2000}
            config_path.write_text(json.dumps(base))
            missing_calibration = subprocess.run([sys.executable, "-c", script], cwd=PROD,
                                                 text=True, capture_output=True)
            self.assertNotEqual(missing_calibration.returncode, 0)
            self.assertIn("requires an explicit chunk cap", missing_calibration.stderr)

    def test_bounded_exec_report(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            report = Path(temp_dir) / "report.json"
            self.run_ok(str(PROD / "bounded_exec.py"), "--rss-limit-bytes", "1000000000",
                        "--report", str(report), "--", sys.executable, "-c", "print('ok')")
            payload = json.loads(report.read_text())
            self.assertEqual(payload["state"], "succeeded")
            self.assertEqual(payload["return_code"], 0)

    def test_local_backend_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            chunks = temp / "chunks"
            chunk_id = "abc123"
            plan = {"fingerprint": "planfp", "chunks": [{"chunk_id": chunk_id}]}
            plan_path = temp / "plan.json"; plan_path.write_text(json.dumps(plan))
            worker = temp / "worker.py"
            worker.write_text(textwrap.dedent("""\
                #!/usr/bin/env python3
                import json, pathlib, sys
                args=dict(zip(sys.argv[1::2],sys.argv[2::2]))
                out=pathlib.Path(args['--output-root'])/args['--chunk-id']; out.mkdir(parents=True,exist_ok=True)
                plan=json.load(open(args['--plan']))
                (out/'_SUCCESS.json').write_text(json.dumps({'state':'succeeded','chunk_id':args['--chunk-id'],
                    'plan_fingerprint':plan['fingerprint']}))
            """))
            worker.chmod(0o755)
            duckdb = temp / "duckdb"; duckdb.write_text("#!/bin/sh\nexit 0\n"); duckdb.chmod(0o755)
            command = [str(PROD / "execute_chunk.py"), "--backend", "local", "--plan", str(plan_path),
                       "--chunk-id", chunk_id, "--chunks-dir", str(chunks), "--project-dir", str(ROOT),
                       "--duckdb", str(duckdb), "--worker", str(worker)]
            self.run_ok(*command)
            second = self.run_ok(*command)
            self.assertIn("already complete", second.stdout)

    def test_dagster_is_orchestrator_not_dagman(self) -> None:
        text = (ROOT / "orchestration/dagster/definitions.py").read_text()
        self.assertIn("DynamicOut", text)
        self.assertIn("discover_and_plan().map(index_chunk)", text)
        self.assertIn("validate_all_chunks(chunks.collect())", text)
        self.assertIn("single_iceberg_commit", text)
        self.assertIn("compact_committed_snapshot", text)
        self.assertIn('{"valid", "validated", "succeeded"}', text)
        self.assertIn("execution_backend", (ROOT / "scripts/production/pipeline_config.py").read_text())
        runner = (ROOT / "scripts/production/run_pipeline.py").read_text()
        self.assertIn('"--root-memory-budget-bytes"', runner)
        self.assertIn('"--metadata-memory-budget-bytes"', runner)
        self.assertNotIn("condor_submit_dag", text)
        self.assertNotIn("airflow", text.lower())

    def test_dagster_config_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            paths = temp / "paths.json"; paths.write_text('["/x"]')
            dictionary = temp / "dictionary.so"; dictionary.write_bytes(b"x")
            root_file = temp / "sample.root"; root_file.write_bytes(b"root")
            config = {
                "format": "root4duckdb-production-config-v1", "project_dir": str(ROOT),
                "inputs": [str(root_file)], "tree": "PaEvent",
                "paths_file": str(paths), "dictionary": str(dictionary),
            }
            config_path = temp / "config.json"; config_path.write_text(json.dumps(config))
            script = (
                "from pipeline_config import load_config; "
                f"c=load_config(r'{config_path}'); "
                "assert c['dagster']['job_name']=='root4duckdb_production_job'; "
                "assert c['dagster']['retries']==3"
            )
            self.run_ok("-c", script, cwd=PROD)



if __name__ == "__main__":
    unittest.main()
