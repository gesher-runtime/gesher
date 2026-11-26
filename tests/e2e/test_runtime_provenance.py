"""Exercise the package receipt and e2e reporter with isolated fixture bytes."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from runtime_provenance import (
    DIST_FILES, PACKAGE_FILES, RECEIPT_NAME, REQUIRED_FILES, create_receipt,
    file_sha256, verify_runtime,
)
from test_coverage import coverage, fixture

REVISION = "a" * 40


class RuntimeProvenanceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.dist = self.root / "dist"
        self.package = self.root / "package"
        self.installed = self.root / "installed"
        self.installer = self.root / "installer" / "gesher-setup-0.2.0.exe"
        self.installer.parent.mkdir()
        self.installer.write_bytes(b"isolated installer fixture")
        for directory, names in ((self.dist, DIST_FILES), (self.package, PACKAGE_FILES),
                                 (self.installed, REQUIRED_FILES)):
            for name in names:
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(f"fixture: {name}".encode())
        self.receipt = create_receipt(self.dist, self.package, self.installer, REVISION)
        self.receipt_path = self.installer.parent / RECEIPT_NAME
        self.write_receipt()

    def write_receipt(self):
        self.receipt_path.write_text(json.dumps(self.receipt), encoding="utf-8")

    def verify(self):
        return verify_runtime(self.installed, self.installer, REVISION)

    def test_measures_required_package_inputs_again_after_installation(self):
        observed = self.verify()
        self.assertEqual(observed["source_revision"], REVISION)
        self.assertEqual(observed["installer_sha256"], file_sha256(self.installer))
        self.assertEqual(observed["provenance_sha256"], file_sha256(self.receipt_path))
        self.assertEqual(observed["installed_files"], self.receipt["files_sha256"])
        self.assertEqual(observed["installed_files"].keys(), REQUIRED_FILES)

    def test_missing_package_file_prevents_pass(self):
        (self.package / "gesher-app.ps1").unlink()
        with self.assertRaises(FileNotFoundError):
            create_receipt(self.dist, self.package, self.installer, REVISION)

    def test_package_command_creates_checkout_bound_receipt(self):
        self.receipt_path.unlink()
        command = [
            sys.executable, str(Path(__file__).with_name("runtime_provenance.py")),
            "capture", "--dist", str(self.dist), "--package", str(self.package),
            "--installer", str(self.installer),
        ]
        environment = {**os.environ, "GITHUB_ACTIONS": "true", "GITHUB_SHA": REVISION}
        invalid = subprocess.run(command, env={**environment, "GITHUB_SHA": ""},
                                 capture_output=True, check=False, timeout=10)
        self.assertEqual(invalid.returncode, 1)
        self.assertFalse(self.receipt_path.exists())
        subprocess.run(command, env=environment, capture_output=True, check=True, timeout=10)
        self.assertEqual(json.loads(self.receipt_path.read_bytes()), self.receipt)

    def test_missing_receipt_is_rejected(self):
        self.receipt_path.unlink()
        with self.assertRaises(FileNotFoundError):
            self.verify()

    def test_different_source_is_rejected_even_with_matching_runtime_bytes(self):
        self.receipt["source_revision"] = "b" * 40
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "different checkout revision"):
            self.verify()

    def test_modified_component_is_rejected(self):
        (self.installed / "framework/x86_64/boot-framework.oat").write_bytes(b"old boot image")
        with self.assertRaisesRegex(ValueError, "boot-framework.oat"):
            self.verify()

    def test_omitted_component_fingerprint_is_rejected_even_if_file_exists(self):
        self.receipt["files_sha256"].pop("gesher_arm64.dll")
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "required component fingerprints"):
            self.verify()

    def test_duplicate_receipt_keys_are_rejected(self):
        raw = self.receipt_path.read_text(encoding="utf-8")
        self.receipt_path.write_text('{"schema_version": 1, ' + raw[1:], encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate runtime provenance field"):
            self.verify()

    def test_large_receipt_is_rejected(self):
        self.receipt_path.write_bytes(b" " * 32768)
        with self.assertRaisesRegex(ValueError, "too large"):
            self.verify()

    def test_runner_proof_and_independent_report_must_agree(self):
        manifest, observations = fixture()
        measured = self.verify()
        observations["runtime"] = {
            **measured, "installer_exit_code": 0, "installation_mode": "installer",
        }
        report = coverage.summarize(manifest, observations, observations["manifest_sha256"],
                                    require_runtime_provenance=True, verified_runtime=measured)
        self.assertTrue(report["gate_passed"])
        self.assertEqual(report["compatibility_matrix"][0]["runtime_source_revision"], REVISION)
        self.assertEqual(report["summary"]["full_functional_compatibility"]["passed"], 0)
        changes = [
            ("installer_exit_code", 1), ("installer_exit_code", False),
            ("installation_mode", "existing_install"), ("source_revision", "c" * 40),
            ("installer_sha256", "c" * 64), ("installed_files", {}), ("provenance_sha256", None),
        ]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(observations)
                bad["runtime"][key] = value
                report = coverage.summarize(manifest, bad, bad["manifest_sha256"],
                                            require_runtime_provenance=True, verified_runtime=measured)
                self.assertFalse(report["gate_passed"])
                self.assertIsNone(report["compatibility_matrix"][0]["runtime_source_revision"])

    def test_real_reporter_rejects_changed_or_ambiguous_installer(self):
        manifest, observations = fixture()
        manifest_path = self.root / "apps.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        observations["manifest_sha256"] = file_sha256(manifest_path)
        observations["runner"]["test_checkout_sha"] = REVISION
        observations["runtime"] = {
            **self.verify(), "installer_exit_code": 0, "installation_mode": "installer",
        }
        observations_path = self.root / "observations.json"
        observations_path.write_text(json.dumps(observations), encoding="utf-8")
        command = [
            sys.executable, str(Path(__file__).with_name("coverage.py")),
            "--manifest", str(manifest_path), "--observations", str(observations_path),
            "--out-dir", str(self.root), "--installer-dir", str(self.installer.parent),
            "--installed-runtime", str(self.installed),
        ]
        environment = {**os.environ, "GITHUB_ACTIONS": "true", "GITHUB_SHA": REVISION}
        for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GESHER_E2E_INVOCATION_ID"):
            environment.pop(key, None)

        def report(expected_status):
            result = subprocess.run(command, env=environment, capture_output=True, text=True,
                                    check=False, timeout=10)
            self.assertEqual(result.returncode, expected_status, result.stdout + result.stderr)
            return json.loads((self.root / "coverage.json").read_text(encoding="utf-8"))

        self.assertTrue(report(0)["gate_passed"])
        self.installer.write_bytes(b"different installer bytes; installed tree still unchanged")
        self.assertIn("installer bytes differ from package provenance", str(report(1)["validation_errors"]))
        self.installer.with_name("gesher-setup-0.2.1.exe").touch()
        self.assertIn("expected one installer", str(report(1)["validation_errors"]))


if __name__ == "__main__":
    unittest.main()
