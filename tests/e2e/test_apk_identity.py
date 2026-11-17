"""SDK metadata contracts plus an optional real, locally signed fixture APK.

The real-tool test requires GESHER_TEST_BUILD_TOOLS and GESHER_TEST_ANDROID_JAR.
CI supplies both; parser tests use isolated strings and make no runtime claims.
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("apk_identity", HERE / "apk_identity.py")
identity = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(identity)
HASH = "a" * 64
BADGING = """package: name='com.gesher.identityfixture' versionCode='7' versionName='fixture'
minSdkVersion:'23'
targetSdkVersion:'36'
application-label:'an APK-controlled display label'
native-code: 'x86_64' 'arm64-v8a'
"""


class ApkIdentityContracts(unittest.TestCase):
    def test_badging_extracts_machine_identity_without_inferring_selected_abi(self):
        result = identity.parse_badging(BADGING)
        self.assertEqual(result, {"package": "com.gesher.identityfixture", "version_code": 7,
                                 "min_sdk": 23, "target_sdk": 36,
                                 "packaged_native_abis": ["arm64-v8a", "x86_64"]})
        self.assertNotIn("selected_abi", result)

    def test_long_version_and_preview_sdk_are_preserved(self):
        result = identity.parse_badging(BADGING.replace("versionCode='7'", "versionCode='7' versionCodeMajor='2'")
                                       .replace("targetSdkVersion:'36'", "targetSdkVersion:'Baklava'"))
        self.assertEqual(result["version_code"], (2 << 32) | 7)
        self.assertEqual(result["target_sdk"], "Baklava")

    def test_absent_metadata_remains_unknown_and_no_native_libraries_is_empty(self):
        result = identity.parse_badging(BADGING.splitlines()[0])
        self.assertIsNone(result["min_sdk"])
        self.assertIsNone(result["target_sdk"])
        self.assertEqual(result["packaged_native_abis"], [])

    def test_manifest_version_uses_root_attributes_only(self):
        tree = """N: android=http://schemas.android.com/apk/res/android (line=1)
  E: manifest (line=1)
    A: http://schemas.android.com/apk/res/android:versionCode(0x0101021b)=7
    A: http://schemas.android.com/apk/res/android:versionCodeMajor(0x01010576)=0x2
      E: application (line=1)
        A: http://schemas.android.com/apk/res/android:versionCode(0x0101021b)=99
"""
        self.assertEqual(identity.parse_manifest_version(tree), (2 << 32) | 7)
        for bad in ("", tree.replace("=0x2", "=@0x7f000001"), tree.replace("=0x2", "=-1")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                identity.parse_manifest_version(bad)

    def test_duplicate_and_malformed_tool_records_are_rejected(self):
        for text in ["", BADGING + BADGING, BADGING + "targetSdkVersion:'36'\n",
                     BADGING + "native-code: 'x86'\n",
                     BADGING.replace("'arm64-v8a'", "'x86_64'"),
                     BADGING.replace("versionCode='7'", "versionCode='-1'"),
                     BADGING.replace("versionCode='7'", "versionCode='4294967296'"),
                     BADGING.replace("targetSdkVersion:'36'", "targetSdkVersion:garbled"),
                     BADGING.replace("native-code: 'x86_64' 'arm64-v8a'", "native-code: 'x86_64' garbage")]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                identity.parse_badging(text)

    def test_multisigner_output_is_complete_and_ordered(self):
        result = identity.parse_certificates(
            "Signer #2 certificate SHA-256 digest: " + "B" * 64 + "\n"
            "Signer #1 certificate SHA-256 digest: " + HASH + "\n"
            "Signer #1 certificate DN: CN=not included in public metadata\n")
        self.assertEqual(result, [HASH, "b" * 64])

    def test_missing_duplicate_and_ambiguous_signers_are_rejected(self):
        valid = "Signer #1 certificate SHA-256 digest: " + HASH + "\n"
        for output in ["", valid + valid, valid.replace("#1", "#2"), valid.replace(HASH, "bad"),
                       valid + "unexpected certificate SHA-256 digest: " + HASH]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                identity.parse_certificates(output)

    def test_tools_have_deadline_output_limit_and_nonzero_exit_is_not_parsed(self):
        with self.assertRaisesRegex(ValueError, "failed.*exit 7"):
            identity.run_tool([sys.executable, "-c", "print('pretend success'); raise SystemExit(7)"], "fixture")
        with self.assertRaisesRegex(ValueError, "deadline"):
            identity.run_tool([sys.executable, "-c", "import time; time.sleep(3)"], "fixture", timeout=1)
        with mock.patch.object(identity, "MAX_TOOL_OUTPUT", 32), self.assertRaisesRegex(ValueError, "limit"):
            identity.run_tool([sys.executable, "-c", "print('x' * 33)"], "fixture")

    def test_explicit_missing_tools_do_not_silently_fall_back(self):
        with tempfile.TemporaryDirectory() as temporary, self.assertRaisesRegex(ValueError, "required"):
            identity.build_tools(Path(temporary))


class RealSdkIdentity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tools = os.environ.get("GESHER_TEST_BUILD_TOOLS")
        android = os.environ.get("GESHER_TEST_ANDROID_JAR")
        if not tools or not android:
            if os.environ.get("GESHER_REQUIRE_REAL_APK_IDENTITY_TEST") == "1":
                raise RuntimeError("real SDK identity test requires build-tools and android.jar")
            raise unittest.SkipTest("real SDK fixture requires GESHER_TEST_BUILD_TOOLS and GESHER_TEST_ANDROID_JAR")
        cls.tools, cls.android = identity.build_tools(Path(tools)), Path(android)

    def test_real_signed_apk_and_tampered_package(self):
        with tempfile.TemporaryDirectory(prefix="gesher-sdk-fixture-") as temporary:
            root = Path(temporary)
            manifest = root / "AndroidManifest.xml"
            manifest.write_text('''<manifest xmlns:android="http://schemas.android.com/apk/res/android"
  package="com.gesher.identityfixture" android:versionCode="7" android:versionCodeMajor="2" android:versionName="fixture">
  <uses-sdk android:minSdkVersion="23" android:targetSdkVersion="36" />
  <application android:label="Isolated SDK metadata fixture" android:hasCode="false" />
</manifest>''', encoding="utf-8")
            apk = root / "fixture.apk"
            aapt = self.tools / ("aapt2.exe" if os.name == "nt" else "aapt2")
            identity.run_tool([str(aapt), "link", "-I", str(self.android), "--manifest", str(manifest),
                               "-o", str(apk)], "fixture APK build")
            # These are inert fixture bytes, not executable native libraries. The
            # collector describes packaging only and the fixture is never run.
            with zipfile.ZipFile(apk, "a") as archive:
                archive.writestr("lib/x86_64/libfixture.so", b"isolated ABI inventory fixture")
                archive.writestr("lib/arm64-v8a/libfixture.so", b"isolated ABI inventory fixture")
            key = root / "fixture.jks"
            keytool = shutil.which("keytool")
            java = shutil.which("java")
            if not keytool:
                self.fail("keytool is required for the real SDK fixture")
            identity.run_tool([keytool, "-genkeypair", "-alias", "fixture", "-keystore", str(key),
                "-storepass", "fixture-only", "-keypass", "fixture-only", "-keyalg", "RSA",
                "-keysize", "2048", "-validity", "2", "-dname", "CN=Isolated SDK test fixture"], "fixture key")
            signer = [java, "-jar", str(self.tools / "lib/apksigner.jar")]
            identity.run_tool(signer + ["sign", "--ks", str(key), "--ks-pass", "pass:fixture-only",
                "--key-pass", "pass:fixture-only", str(apk)], "fixture signature")
            pin = identity.hash_file(apk)
            version = (2 << 32) | 7
            result = identity.collect(apk, "com.gesher.identityfixture", version, pin, self.tools)
            self.assertEqual(result["apk_sha256"], pin)
            self.assertEqual(result["version_code"], version)
            self.assertEqual(result["min_sdk"], 23)
            self.assertEqual(result["target_sdk"], 36)
            self.assertEqual(result["packaged_native_abis"], ["arm64-v8a", "x86_64"])
            self.assertEqual(len(result["signer_certificate_sha256"]), 1)
            self.assertEqual(result["tools"]["apksigner_jar_sha256"], identity.hash_file(self.tools / "lib/apksigner.jar"))
            for package, bad_version, digest in [("different.package", version, pin),
                                             ("com.gesher.identityfixture", 8, pin),
                                             ("com.gesher.identityfixture", version, "0" * 64)]:
                with self.subTest(package=package, version=bad_version, digest=digest), self.assertRaises(ValueError):
                    identity.collect(apk, package, bad_version, digest, self.tools)
            # Updating the hash does not make an invalid signature verified.
            with zipfile.ZipFile(apk, "a") as archive:
                archive.writestr("assets/tampered", b"changed after signing")
            with self.assertRaisesRegex(ValueError, "apksigner failed"):
                identity.collect(apk, "com.gesher.identityfixture", version, identity.hash_file(apk), self.tools)


if __name__ == "__main__":
    unittest.main()
