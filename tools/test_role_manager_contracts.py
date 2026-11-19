#!/usr/bin/env python3
"""Exercise actual RoleManager with explicit host-only Binder identity boundaries."""
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework-classes", type=Path, required=True)
    parser.add_argument("--app-classes", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes))
    for path in (args.app_classes, args.framework_classes):
        if not path.exists():
            parser.error(f"Missing compiled classes: {path}")
    java = str(args.java_home / "bin/java") if args.java_home else "java"
    javac = str(args.java_home / "bin/javac") if args.java_home else "javac"
    sources = [root / "app/java/android/app/role/RoleManager.java",
               root / "app/java/android/app/role/OnRoleHoldersChangedListener.java",
               root / "tests/dex-services/gesher/services/RoleManagerUnsupportedTest.java"]
    sources += sorted((root / "tests/role-manager-host").rglob("*.java"))
    for path in sources[:3]:
        print(f"{path.relative_to(root)} SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()}",
              flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-role-manager-") as temporary:
        subprocess.run([javac, "-cp", classpath, "-d", temporary,
                        *map(str, sources)], check=True, timeout=120)
        runtime_classpath = temporary + os.pathsep + classpath
        for entry in ("gesher.services.IndependentReqa", "gesher.services.AdmissionIdentityProbe"):
            subprocess.run([java, "-cp", runtime_classpath, entry], check=True, timeout=60)


if __name__ == "__main__":
    main()
