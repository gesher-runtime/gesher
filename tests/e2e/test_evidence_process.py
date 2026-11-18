"""Exercise actual PowerShell writers in separate processes, without APK execution."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("e2e_coverage", HERE / "coverage.py")
coverage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage)
PWSH = os.environ.get("GESHER_TEST_PWSH") or shutil.which("pwsh")
WRITER = r"""
param([string] $Helper, [string] $Manifest, [string] $Directory, [string] $Ready)
$ErrorActionPreference = 'Stop'
. $Helper
try {
  $data = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
  New-E2eEvidence $data $Manifest @() $Directory
  [IO.File]::WriteAllText($Ready, $script:E2eEvidence.invocation_id)
  if ([Console]::ReadLine() -eq 'complete') {
    Add-E2eApk 'fixture.process' ('a' * 64)
    foreach ($scenario in @('launch', 'association', 'shortcut')) {
      Set-E2eCheck 'fixture.process' $scenario 'running'
      Set-E2eCheck 'fixture.process' $scenario 'passed'
    }
    $script:E2eEvidence.completed = $true
    Write-E2eEvidence
  }
} finally {
  Close-E2eEvidenceRun
}
"""


class EvidenceProcessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PWSH:
            raise RuntimeError("pwsh is required for process ownership tests; set GESHER_TEST_PWSH")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gesher-evidence-process-")
        self.root = Path(self.temporary.name)
        self.out = self.root / "results"
        self.out.mkdir()
        self.writer = self.root / "writer.ps1"
        self.writer.write_text(WRITER, encoding="utf-8")
        self.manifest = {"apps": [{"package": "fixture.process", "versionName": "one",
                                   "versionCode": 1, "sha256": "a" * 64}]}
        self.manifest_path = self.root / "apps.json"
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        self.temporary.cleanup()

    def command(self, name):
        return [PWSH, "-NoLogo", "-NoProfile", "-File", str(self.writer),
                "-Helper", str(HERE / "evidence.ps1"), "-Manifest", str(self.manifest_path),
                "-Directory", str(self.out), "-Ready", str(self.root / name)]

    def start(self, name):
        process = subprocess.Popen(self.command(name), stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        marker = self.root / name
        deadline = time.monotonic() + 15
        while not marker.exists():
            if process.poll() is not None:
                stdout, stderr = process.communicate(timeout=5)
                self.fail(f"writer did not initialize: {stdout}\n{stderr}")
            if time.monotonic() >= deadline:
                self.fail("writer initialization timed out")
            time.sleep(0.01)
        return process, marker.read_text()

    def finish(self, process, instruction="stop"):
        stdout, stderr = process.communicate(instruction + "\n", timeout=15)
        self.assertEqual(process.returncode, 0, stdout + stderr)

    def observed(self):
        return json.loads((self.out / "observations.json").read_text(encoding="utf-8-sig"))

    def report(self):
        return coverage.summarize(self.manifest, self.observed(), coverage.hash_file(self.manifest_path))

    def test_concurrent_writer_cannot_clear_or_overwrite_owner_and_next_run_gets_new_id(self):
        owner, invocation = self.start("owner-ready")
        before = (self.out / "observations.json").read_bytes()
        rejected = subprocess.run(self.command("contender-ready"), input="complete\n",
                                  capture_output=True, text=True, timeout=15)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("Cannot exclusively own", rejected.stdout + rejected.stderr)
        self.assertFalse((self.root / "contender-ready").exists())
        self.assertEqual((self.out / "observations.json").read_bytes(), before)
        self.assertFalse(self.report()["gate_passed"])
        self.finish(owner, "complete")
        self.assertTrue(self.report()["gate_passed"])

        successor, successor_id = self.start("successor-ready")
        self.assertNotEqual(invocation, successor_id)
        self.assertEqual(self.observed()["invocation_id"], successor_id)
        self.assertFalse(self.report()["gate_passed"])
        self.finish(successor)

    def test_process_death_releases_ownership_without_deleting_the_lock_inode(self):
        owner, invocation = self.start("owner-ready")
        owner.kill()
        owner.communicate(timeout=10)
        self.assertTrue((self.out / ".e2e-run.lock").is_file())
        successor, successor_id = self.start("successor-ready")
        self.assertNotEqual(invocation, successor_id)
        self.assertFalse(self.report()["gate_passed"])
        self.finish(successor, "complete")
        self.assertTrue(self.report()["gate_passed"])


if __name__ == "__main__":
    unittest.main()
