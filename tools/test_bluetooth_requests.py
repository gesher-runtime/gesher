#!/usr/bin/env python3
"""Run production Bluetooth request and intent routing contracts on a JVM.

Compiles candidate request/ATM code unchanged. The fixture uses constructed
manifests/permission state and a server-local backend; no hardware/UI is touched.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--production-root", type=Path, default=ROOT,
                        help="Candidate checkout providing unmodified production sources")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for native initialization boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                              (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-bluetooth-requests-") as directory:
        work = Path(directory); classes = work / "classes"; classes.mkdir()
        library = work / "libbluetooth_request_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/activity-finish/framework_host.c"), "-o", str(library)], check=True)
        sources = []
        for name in ("BluetoothRequests", "ActivityTaskManagerService"):
            source = args.production_root / ("app/java/com/gesher/server/" + name + ".java")
            print(name, "SHA256", hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
            sources.append(str(source))
        sources += [str(ROOT / "tests/dex-services/com/gesher/server/BluetoothRequestFixture.java"),
                    str(ROOT / "tests/bluetooth/BluetoothRequestHost.java")]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        cleaner = work / "cleaner" / "sun/misc"; cleaner.mkdir(parents=True)
        cleaner_inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not cleaner_inputs:
            raise ValueError("Android core compile input has no Cleaner classes")
        for entry in cleaner_inputs:
            shutil.copy2(entry, cleaner / entry.name)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "data")
        env["GESHER_MICROG_SPOOF"] = "1"; env["GESHER_LOCALES"] = "en-US"
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "BluetoothRequestHost", str(library)], env=env, check=True, timeout=90)


if __name__ == "__main__":
    main()
