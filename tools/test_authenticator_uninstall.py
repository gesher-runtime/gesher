#!/usr/bin/env python3
"""Exercise real package/account/sync retirement and shared-store crash recovery.

Uses constructed installed metadata and an explicit account-serialization seam;
this host fixture does not exercise Windows DPAPI or Binder transport. Each case
runs in a separate temporary device profile. Pass an older --app-classes build
with --case removal-api as a behavioral negative control: the preflight rejects owned accounts.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ("removal-api", "commit", "rollback", "staging", "publication", "recovery", "collision", "retirement")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--case", choices=("all", *CASES), default="all")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("A host C compiler is required for the declared JNI initialization boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
        (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    with tempfile.TemporaryDirectory(prefix="gesher-authenticator-uninstall-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libaccount_uninstall_host.so"
        subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/sync/framework_host.c"), "-o", str(library)], check=True)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(ROOT / "tests/dex-services/gesher/services/AuthenticatorUninstallTest.java"),
                        str(ROOT / "tests/accounts/AuthenticatorUninstallHost.java")], check=True)
        cleaner = work / "cleaner/sun/misc"; cleaner.mkdir(parents=True)
        for source in (args.core_classes / "sun/misc").glob("Cleaner*.class"):
            shutil.copy2(source, cleaner / source.name)
        for case in CASES if args.case == "all" else (args.case,):
            environment = os.environ.copy()
            environment["ANDROID_DATA"] = str(work / case)
            environment["GESHER_MICROG_SPOOF"] = "1"
            environment.pop("GESHER_BINDER_UID", None)
            subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                            "-XX:+UnlockDiagnosticVMOptions", "-XX:-BytecodeVerificationRemote",
                            "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                            "-cp", str(classes) + os.pathsep + classpath,
                            "AuthenticatorUninstallHost", str(library), case], env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
