#!/usr/bin/env python3
"""Generate public signed rotation-capability fixtures; discard all throwaway keys.

Usage: generate.py BUILD_TOOLS FRAMEWORK_RES
Requires the JDK java/keytool programs on PATH and Android SDK build-tools.
"""
import pathlib
import subprocess
import sys
import tempfile


def main():
    build_tools = pathlib.Path(sys.argv[1]).resolve()
    framework = pathlib.Path(sys.argv[2]).resolve()
    here = pathlib.Path(__file__).resolve().parent
    suffix = ".exe" if sys.platform == "win32" else ""
    signer = ["java", "-jar", str(build_tools / "lib/apksigner.jar")]

    def run(*args):
        subprocess.run([str(arg) for arg in args], check=True)

    with tempfile.TemporaryDirectory(prefix="gesher-rotation-fixtures-") as temporary:
        work = pathlib.Path(temporary)
        run(build_tools / ("aapt2" + suffix), "link", "-o", work / "linked.apk",
            "--manifest", here.parent / "fixtures/AndroidManifest.xml", "-I", framework)
        run(build_tools / ("zipalign" + suffix), "-f", "4", work / "linked.apk", work / "base.apk")
        for key in ("old", "new"):
            run("keytool", "-genkeypair", "-keystore", work / (key + ".p12"),
                "-storetype", "PKCS12", "-storepass", "fixture-only", "-alias", key,
                "-keyalg", "RSA", "-keysize", "2048", "-validity", "36500",
                "-dname", "CN=Gesher rotation capability " + key + ", O=Gesher tests")
        run(*signer, "sign", "--ks", work / "old.p12", "--ks-pass", "pass:fixture-only",
            "--v1-signing-enabled", "false", "--v3-signing-enabled", "false",
            "--out", here / "old.apk", work / "base.apk")
        for label, installed, auth in (("no-data", "false", "true"), ("no-auth", "true", "false")):
            lineage = work / (label + ".lineage")
            run(*signer, "rotate", "--out", lineage, "--old-signer", "--ks", work / "old.p12",
                "--ks-pass", "pass:fixture-only", "--set-installed-data", installed,
                "--set-auth", auth, "--new-signer", "--ks", work / "new.p12",
                "--ks-pass", "pass:fixture-only")
            run(*signer, "sign", "--ks", work / "old.p12", "--ks-pass", "pass:fixture-only",
                "--next-signer", "--ks", work / "new.p12", "--ks-pass", "pass:fixture-only",
                "--lineage", lineage, "--out", here / ("rotated-" + label + ".apk"), work / "base.apk")
        for name in ("old.apk", "rotated-no-data.apk", "rotated-no-auth.apk"):
            run(*signer, "verify", here / name)
            (here / (name + ".idsig")).unlink(missing_ok=True)


if __name__ == "__main__":
    main()
