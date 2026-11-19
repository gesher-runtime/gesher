#!/usr/bin/env python3
"""Run the production input-method service's unavailable-handwriting callbacks on a JVM.

No native adapters or handwriting providers are substituted. Direct Java callbacks
exercise unsupported completion and RemoteException propagation; generated Binder
request/callback parceling is a separate Windows ART fixture, not a host claim.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior production service as a negative control")
    args = parser.parse_args()
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-handwriting-unavailable-") as directory:
        work = Path(directory)
        relative = "app/java/com/gesher/server/InputMethodManagerService.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "InputMethodManagerService.java"
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                    cwd=ROOT, check=True, capture_output=True).stdout)
        print("InputMethodManagerService.java sha256="
                + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
        classes = work / "classes"
        classes.mkdir()
        fixture = ROOT / "tests/dex-services/gesher/services/HandwritingUnavailableContractTest.java"
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), str(source), str(fixture)],
                       check=True, timeout=60)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "-cp", str(classes) + os.pathsep + classpath,
                        "gesher.services.HandwritingUnavailableContractTest", "--host"],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
