#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise real microG APK verification, generation-local reuse and content tampering.

Pass the official Companion and exact legacy GSF APKs pinned by gesher-app-microg.ps1.
No network access or Android parser/native-code substitution occurs inside this runner.
The Java host constructs manifest metadata; the ART MicrogTest retains parser coverage.
An unmodified --app-classes baseline fails the additional APK-hashing assertion.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig", "companion-apk", "gsf-apk"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if compiler is None:
        parser.error("A C compiler is required for host native boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-microg-identity-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libmicrog_identity_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/appops/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), str(ROOT / "tests/package/MicrogIdentityHost.java")], check=True)
        # A disposable non-microG signer: an installed com.android.vending such as Google's.
        keystore = work / "other-signer.p12"
        subprocess.run([str(args.java_home / "bin/keytool"), "-genkeypair", "-keystore", str(keystore),
                        "-storetype", "PKCS12", "-storepass", "fixture-only", "-alias", "fixture",
                        "-keyalg", "RSA", "-keysize", "2048", "-validity", "30",
                        "-dname", "CN=Gesher disposable signing test", "-noprompt"],
                       check=True, stdout=subprocess.DEVNULL)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData", "--patch-module",
                   "jdk.unsupported=" + str(work / "cleaner"), "-cp", str(classes) + os.pathsep + classpath,
                   "com.gesher.server.MicrogIdentityHost", str(library), str(args.companion_apk.resolve()),
                   str(args.gsf_apk.resolve()), str(ROOT / "tests/dex-services/fixtures"), str(work),
                   str(keystore)]
        for mode in ("0", "1"):
            env = os.environ.copy(); env["ANDROID_DATA"] = str(work / ("data-" + mode))
            env["GESHER_MICROG_SPOOF"] = mode; env.pop("GESHER_BINDER_UID", None)
            subprocess.run(command, env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
