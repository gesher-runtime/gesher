#!/usr/bin/env python3
"""Run the production auth (IAuthService) contracts of ADR 0091 on a host JVM.

The Android and system-server boundaries are stand-ins with android-16.0.0_r4's
constant values; AuthService and WindowsHelloBackend are compiled unchanged apart
from their package, imports and public class visibility. Tests cover the
sensor-less AOSP answers with no Windows Hello backend (production today), and,
with an isolated test double only, the device-credential confirmation outcomes,
cancellation, replacement, client death and callback ordering.
Use --source-ref to run the contracts against another revision of the sources.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "// PRODUCTION_BACKEND": ("app/java/com/gesher/server/WindowsHelloBackend.java",
                              "public interface WindowsHelloBackend", "interface WindowsHelloBackend"),
    "// PRODUCTION_SERVICE": ("app/java/com/gesher/server/AuthService.java",
                              "public final class AuthService", "final class AuthService"),
}


def read(relative: str, ref: str | None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", ref + ":" + relative], cwd=ROOT, text=True)
    return (ROOT / relative).read_text()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    harness = (ROOT / "tests/biometrics/AuthServiceContracts.java.in").read_text()
    for marker, (relative, public, private) in SOURCES.items():
        source = read(relative, args.source_ref)
        print(Path(relative).name, "SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
        # The harness declares java.util.* once; Android types are its stand-ins.
        source = re.sub(r"^package .*;\n|^import .*;\n", "", source, flags=re.MULTILINE)
        if public not in source:
            raise SystemExit(f"{relative}: expected '{public}'")
        harness = harness.replace(marker, source.replace(public, private, 1))
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-auth-") as temporary:
        work = Path(temporary)
        java = work / "AuthServiceContracts.java"
        java.write_text(harness)
        subprocess.run([executable("javac"), "-Xlint:all", "-Werror", "-d", str(work), str(java)],
                       check=True, timeout=60)
        subprocess.run([executable("java"), "-cp", str(work), "AuthServiceContracts"],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
