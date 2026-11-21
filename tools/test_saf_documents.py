#!/usr/bin/env python3
"""Exercise production SAF transport, ATM results and durable grants on host files.

The explicit fixture context and native adapter provide service lookup, Binder
identity/allocation and diagnostics only. Windows track 94 separately opens real
native descriptors and dialogs; this host runner does not claim remote Binder/ART.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared native host boundary")
    cp = os.pathsep.join(str(p.resolve()) for p in
                        (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    with tempfile.TemporaryDirectory(prefix="gesher-saf-host-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libsaf_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/saf/framework_host.c"), "-o", str(library)], check=True)
        sources = [ROOT / "app/java/com/gesher/server" / (name + ".java") for name in
                   ("DocumentsStorageProvider", "DocumentPicker", "WindowsHost", "ContentProviders",
                    "ActivityManagerService", "ActivityTaskManagerService", "PackageManagerService")]
        sources += [ROOT / "tests/dex-services/gesher/services/FixtureServiceContext.java",
                    ROOT / "tests/dex-services/gesher/services/SafDocumentsContractTest.java",
                    ROOT / "tests/saf/SafDocumentsHost.java",
                    ROOT / "tests/dex-services/gesher/services/DetachedNativeUiContractTest.java"]
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-source", "17",
                        "-target", "17", "-cp", cp, "-d", str(classes), *map(str, sources)], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        (work / "data").mkdir()
        environment["GESHER_MICROG_SPOOF"] = "1"  # Explicit isolated constructed-package fixture.
        environment["GESHER_LOCALES"] = "en-US"
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        cleaner_inputs = list((args.core_classes / "sun/misc").glob("Cleaner*.class"))
        if not cleaner_inputs:
            raise ValueError("Android core input has no Cleaner classes")
        for entry in cleaner_inputs:
            shutil.copy2(entry, cleaner / entry.name)
        command = [str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-Djava.io.tmpdir=" + str(work),
                        "-cp", str(classes) + os.pathsep + cp, "SafDocumentsHost", str(library)]
        subprocess.run(command, env=environment, check=True, timeout=90)
        detached = command[:-2] + ["gesher.services.DetachedNativeUiContractTest", str(library)]
        subprocess.run(detached, env=environment, check=True, timeout=45)
        restart = work / "restart"
        restart.mkdir()
        for phase in ("write", "read", "released"):
            subprocess.run(command + [phase, str(restart)], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
