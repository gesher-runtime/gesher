"""Synthetic logs only: parser privacy, evidence levels and bounded scanning."""
from dataclasses import replace
import hashlib
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compatibility_audit as audit


class ParserTests(unittest.TestCase):
    def identifiers(self, line):
        return [item.identifier for item in audit.parse_line(line)]

    def test_java_descriptors_and_causes_normalize_to_one_gap(self):
        variants = (
            "java.lang.NoClassDefFoundError: Failed resolution of: Landroid/bluetooth/le/ScanCallback;",
            "Caused by: java.lang.ClassNotFoundException: android.bluetooth.le.ScanCallback",
            "java.lang.ClassNotFoundException: Didn't find class \"android.bluetooth.le.ScanCallback\" on path: SECRET",
            "NoClassDefFoundError: [[Landroid/bluetooth/le/ScanCallback;",
        )
        for line in variants:
            self.assertEqual(self.identifiers(line), ["android.bluetooth.le.ScanCallback"])
        self.assertEqual(self.identifiers("ClassNotFoundException: arbitrary user text"), [])

    def test_jni_signature_and_attempted_symbols(self):
        line = ("UnsatisfiedLinkError: No implementation found for long android.os.Test.nativeOpen"
                "(java.lang.String, int[], byte[][]) (tried Java_android_os_Test_nativeOpen SECRET)")
        self.assertEqual(self.identifiers(line),
                         ["long android.os.Test.nativeOpen(java.lang.String,int[],byte[][])"])
        self.assertEqual(self.identifiers("UnsatisfiedLinkError: 'void android.os.Test.close()'"),
                         ["void android.os.Test.close()"])
        for malformed in ("void android.os.Test.open(https://private/?token=SECRET)",
                          "void android.os.Test.open(java.lang.String SECRET)",
                          "void android.os.Test.open(void)",
                          "void android.os.Test.open(void[])",
                          "void[] android.os.Test.open()"):
            self.assertIsNone(audit.jni_signature(malformed))
        self.assertEqual(self.identifiers("UnsatisfiedLinkError: dlopen failed: PRIVATE-PATH"), [])

    def test_native_stubbed_is_not_called(self):
        stubbed = audit.parse_line("gesher elf: libvlc.so: 'tsearch' is not implemented (stubbed)")[0]
        called = audit.parse_line("gesher elf: called unresolved function 'tsearch'")[0]
        self.assertEqual(stubbed.identifier, called.identifier)
        self.assertEqual(stubbed.kind, "native_import_stubbed")
        self.assertEqual(called.kind, "native_unresolved_function_called")
        self.assertEqual(stubbed.library, "libvlc.so")
        data = audit.parse_line("gesher elf: loaded code accessed data symbol 'SL_IID_FOO' (+0), which nothing defines")
        self.assertEqual(data[0].kind, "native_unresolved_data_accessed")
        self.assertEqual(self.identifiers("gesher elf: called unresolved function 'not an identifier'"), [])

    def test_stand_in_and_unresolved_settings_only(self):
        self.assertEqual(self.identifiers("system_server: stand-in IWindowManager.foo not implemented; returning a default"),
                         ["IWindowManager.foo"])
        self.assertEqual(self.identifiers("UnsupportedOperationException: gesher stand-in IFoo.bar is not implemented"),
                         ["IFoo.bar"])
        self.assertEqual(audit.parse_line("gesher stand-in IFoo.bar is not implemented")[0].kind,
                         "stand_in_unimplemented")
        action = "android.settings.CHANNEL_NOTIFICATION_SETTINGS"
        self.assertEqual(self.identifiers(f"ActivityNotFoundException: No Activity found to handle Intent {{ act={action} (has extras) }}"),
                         [action])
        self.assertEqual(self.identifiers(f"startActivity Intent {{ act={action} }}"), [])
        self.assertEqual(self.identifiers("ActivityNotFoundException: Intent { act=android.intent.action.VIEW dat=https://private }"), [])

    def test_arbitrary_private_text_is_never_retained(self):
        private = "PHONE-972551234567 TOKEN-sk-abcdefghijklmnop https://private.example/?api_token=SECRET"
        lines = (f'gesher-ime: commit "{private}"',
                 f"java.lang.ClassNotFoundException: android.os.Missing {private}",
                 f"gesher elf: libvlc.so: 'tsearch' is not implemented (stubbed) {private}",
                 f"No Activity found to handle Intent {{ act=android.settings.APP_NOTIFICATION_SETTINGS extras={private} }}")
        emitted = json.dumps([audit.asdict(item) for line in lines for item in audit.parse_line(line)])
        for value in ("972551234567", "abcdefghijklmnop", "private.example", "SECRET", "gesher-ime"):
            self.assertNotIn(value, emitted)
        self.assertEqual(self.identifiers("gesher elf: called unresolved function 'ghp_abcdefghijklmnop'"), [])
        self.assertEqual(self.identifiers("ClassNotFoundException: com.example.AIzaABCDEFGHIJKLMNOPQRSTUV"), [])
        self.assertEqual(self.identifiers('gesher-ime: commit "ClassNotFoundException: com.secret.Phone972551234567"'), [])
        self.assertEqual(self.identifiers('gesher-ime: commit "gesher elf: called unresolved function \'PRIVATE_KEY\'"'), [])


class ScannerTests(unittest.TestCase):
    def make_log(self, root, name, text):
        path = Path(root) / name
        path.write_bytes(text.encode("utf-8"))
        return path

    def test_dedup_counts_evidence_hashes_and_no_input_mutation(self):
        with tempfile.TemporaryDirectory() as temp:
            one = self.make_log(temp, "org.example.one.log", "\n".join((
                "gesher elf: liba.so: 'tsearch' is not implemented (stubbed)",
                "gesher elf: called unresolved function 'tsearch'",
                'gesher-ime: commit "PHONE-972551234567"',
                "ClassNotFoundException: android.os.Missing")) + "\n")
            self.make_log(temp, "org.example.two.log", "ClassNotFoundException: android.os.Missing\n")
            self.make_log(temp, "org.example.two.log.logcat", "ClassNotFoundException: android.os.Ignored\n")
            original = one.read_bytes()
            report = audit.audit_directory(Path(temp))
            gaps = {item["gap_id"]: item for item in report["gaps"]}
            self.assertEqual(report["summary"]["gap_identifiers"], 2)
            self.assertEqual(report["summary"]["observations"], 4)
            self.assertEqual(report["summary"]["affected_apps"], 2)
            self.assertEqual(gaps["java_class:android.os.Missing"]["affected_app_count"], 2)
            native = gaps["native_symbol:tsearch"]
            self.assertEqual(native["evidence_levels"], ["import_only", "invocation_failure"])
            self.assertEqual([item["line"] for item in native["evidence"]], [1, 2])
            self.assertEqual(native["current_source_status"], "not_assessed")
            self.assertEqual(report["sources"][0]["scanned_sha256"], hashlib.sha256(original).hexdigest())
            self.assertEqual(one.read_bytes(), original)
            for rendered in (json.dumps(report), audit.markdown(report)):
                self.assertNotIn("972551234567", rendered)
                self.assertNotIn(temp, rendered)

    def test_shared_logs_do_not_invent_an_affected_app(self):
        with tempfile.TemporaryDirectory() as temp:
            self.make_log(temp, "system_server.log", "stand-in IFoo.bar not implemented\n")
            report = audit.audit_directory(Path(temp))
            self.assertEqual(report["gaps"][0]["affected_app_count"], 0)
            self.assertEqual(report["gaps"][0]["evidence"][0]["file"], "system_server.log")

    def test_unknown_filename_is_redacted_and_evidence_is_capped(self):
        with tempfile.TemporaryDirectory() as temp:
            self.make_log(temp, "PHONE-972551234567.log", "ClassNotFoundException: android.os.Missing\n" * 5)
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_evidence_per_gap=2))
            gap = report["gaps"][0]
            self.assertEqual(gap["observation_count"], 5)
            self.assertEqual(gap["evidence_samples_omitted"], 3)
            self.assertNotIn("972551234567", json.dumps(report))
            self.assertEqual(report["sources"][0]["file"], "redacted-log-0001.log")

    def test_byte_line_gap_file_and_observation_bounds(self):
        with tempfile.TemporaryDirectory() as temp:
            line = "ClassNotFoundException: android.os.One\n"
            self.make_log(temp, "org.example.one.log", line + "ClassNotFoundException: android.os.Two\n")
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_total_bytes=len(line) + 8))
            self.assertEqual(report["summary"]["bytes_scanned"], len(line) + 8)
            self.assertEqual(report["summary"]["gap_identifiers"], 1)
            self.assertEqual(report["summary"]["files_truncated"], 1)
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_lines=1))
            self.assertEqual(report["summary"]["lines_scanned"], 1)
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_gaps=1))
            self.assertEqual(report["summary"]["observations_not_aggregated"], 1)
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_observations=1))
            self.assertEqual(report["summary"]["stop_reason"], "observation_limit")
            self.make_log(temp, "org.example.two.log", line)
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_files=1))
            self.assertEqual(report["summary"]["files_scanned"], 1)
            self.assertTrue(report["summary"]["file_limit_reached"])

    def test_oversized_lines_do_not_shift_evidence_locations(self):
        with tempfile.TemporaryDirectory() as temp:
            self.make_log(temp, "org.example.one.log", "x" * 200 + "\nClassNotFoundException: android.os.One\n")
            report = audit.audit_directory(Path(temp), replace(audit.Bounds(), max_line_bytes=80))
            self.assertEqual(report["sources"][0]["oversized_lines_skipped"], 1)
            self.assertEqual(report["gaps"][0]["evidence"][0]["line"], 2)

    def test_utf16_is_explicitly_rejected_not_silently_scanned(self):
        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / "org.example.one.log").write_bytes("ClassNotFoundException: android.os.One".encode("utf-16"))
            report = audit.audit_directory(Path(temp))
            self.assertEqual(report["sources"][0]["error"], "unsupported_utf16_encoding")
            self.assertEqual(report["summary"]["observations"], 0)

    def test_cli_writes_both_reports_outside_input_only(self):
        with tempfile.TemporaryDirectory() as temp:
            logs = Path(temp) / "logs"
            logs.mkdir()
            self.make_log(logs, "org.example.one.log", "ClassNotFoundException: android.os.One\n")
            output = Path(temp) / "reports"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(audit.main([str(logs), "--out-dir", str(output)]), 0)
            self.assertTrue((output / "compatibility.json").is_file())
            self.assertTrue((output / "compatibility.md").is_file())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(audit.main([str(logs), "--out-dir", str(logs / "output")]), 1)
            self.assertFalse((logs / "output").exists())

    def test_symbolic_link_logs_are_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            original = self.make_log(temp, "original.txt", "ClassNotFoundException: android.os.One\n")
            try:
                (Path(temp) / "org.example.link.log").symlink_to(original)
            except OSError:
                self.skipTest("symbolic links unavailable on this test host")
            report = audit.audit_directory(Path(temp))
            self.assertEqual(report["summary"]["files_scanned"], 0)


if __name__ == "__main__":
    unittest.main()
