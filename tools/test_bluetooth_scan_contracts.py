#!/usr/bin/env python3
"""Run the production no-offload BLE scan contracts with a deterministic queue.

The Android/Windows boundaries are stand-ins; the complete scanner class is
compiled unchanged apart from its package, imports and public class visibility.
Use --source-ref to verify that the regressions fail before the repair.
Tests preserve immediate scans, reject unsupported settings before registration,
and distinguish posted callback failures from synchronous PendingIntent errors.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    relative = "app/java/android/bluetooth/le/BluetoothLeScanner.java"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative],
                                     cwd=ROOT, text=True) if args.source_ref
              else (ROOT / relative).read_text())
    print("BluetoothLeScanner SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    source = re.sub(r"^package .*;\n|^import (?!java\.).*;\n", "", source, flags=re.MULTILINE)
    # The harness declares java.util.* once before both top-level classes.
    source = re.sub(r"^import java\..*;\n", "", source, flags=re.MULTILINE)
    source = source.replace("public final class BluetoothLeScanner", "final class BluetoothLeScanner")
    template = (ROOT / "tests/bluetooth/BluetoothScanHost.java.in").read_text()
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-bluetooth-scan-") as temporary:
        work = Path(temporary)
        java = work / "BluetoothScanHost.java"
        java.write_text(template.replace("// PRODUCTION_SCANNER", source))
        subprocess.run([executable("javac"), "-d", str(work), str(java)], check=True, timeout=60)
        subprocess.run([executable("java"), "-cp", str(work), "BluetoothScanHost"],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
