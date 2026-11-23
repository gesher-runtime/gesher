#!/usr/bin/env python3
"""Build original SDK-only APKs for actual Android/Windows notification delegation tests."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = {
    "owner": "gesher.notificationdelegation.owner",
    "delegate": "gesher.notificationdelegation.delegate",
    "other": "gesher.notificationdelegation.other",
    "sharedowner": "gesher.notificationdelegation.sharedowner",
    "shareddelegate": "gesher.notificationdelegation.shareddelegate",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("android-jar", "build-tools", "java-home", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    classes, dex = output / "classes", output / "dex"
    classes.mkdir(exist_ok=True); dex.mkdir(exist_ok=True)
    source = ROOT / "tests/notification-delegation/sdk/java/gesher/notificationdelegation/ControlReceiver.java"
    def run(command: list[object]) -> None:
        subprocess.run([str(value) for value in command], check=True)
    run([args.java_home / "bin/javac", "--release", 11, "-cp", args.android_jar,
         "-d", classes, source])
    jar = output / "fixture.jar"
    run([args.java_home / "bin/jar", "--create", "--file", jar, "-C", classes, "."])
    run([args.java_home / "bin/java", "-cp", args.build_tools / "lib/d8.jar",
         "com.android.tools.r8.D8", "--min-api", 29, "--lib", args.android_jar,
         "--output", dex, jar])
    key = output / "fixture.keystore"
    if not key.exists():
        run([args.java_home / "bin/keytool", "-genkeypair", "-keystore", key,
             "-storepass", "fixture-password", "-keypass", "fixture-password", "-alias", "fixture",
             "-keyalg", "RSA", "-keysize", 2048, "-validity", 3650,
             "-dname", "CN=Gesher isolated notification test"])
    receipts = []
    for name, package in PACKAGES.items():
        work = output / name; work.mkdir(exist_ok=True)
        shared = ' android:sharedUserId="gesher.notificationdelegation.shared"' if name.startswith("shared") else ""
        queries = "".join(f'<package android:name="{value}"/>' for value in PACKAGES.values())
        manifest = work / "AndroidManifest.xml"
        manifest.write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{package}"{shared}>
<uses-sdk android:minSdkVersion="29" android:targetSdkVersion="36"/>
<uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>
<queries>{queries}</queries>
<application android:label="Isolated delegation fixture" android:allowBackup="false" android:debuggable="true">
<receiver android:name="gesher.notificationdelegation.ControlReceiver" android:exported="true"/>
</application></manifest>\n''')
        unsigned, apk = work / "unsigned.apk", output / (name + ".apk")
        run([args.build_tools / "aapt2", "link", "-I", args.android_jar, "--manifest", manifest,
             "--version-code", 1, "--version-name", "1", "-o", unsigned])
        with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(dex / "classes.dex", "classes.dex")
        run([args.java_home / "bin/java", "-jar", args.build_tools / "lib/apksigner.jar", "sign",
             "--ks", key, "--ks-pass", "pass:fixture-password", "--key-pass", "pass:fixture-password",
             "--out", apk, unsigned])
        run([args.java_home / "bin/java", "-jar", args.build_tools / "lib/apksigner.jar", "verify", apk])
        receipts.append({"name": name, "package": package, "apk": str(apk),
                         "sha256": hashlib.sha256(apk.read_bytes()).hexdigest(), "shared_uid": bool(shared)})
    result = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "android_jar_sha256": hashlib.sha256(args.android_jar.read_bytes()).hexdigest(), "apks": receipts}
    (output / "receipt.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
