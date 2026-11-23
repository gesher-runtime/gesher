#!/usr/bin/env python3
"""Exercise production Wi-Fi permissions and ConnectivityManager privacy on the host.

The fixture controls Context permissions, local Binder service responses and Handler
scheduling. It does not replace manager methods, the callback registry or framework
value objects. Server authorization, Windows/ART, Parcel transport and physical
adapters are separate integration gates.
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
    for name in ("java-home", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--case", default="all", choices=("all", "helper", "wifi", "connectivity", "wifi-no-access"))
    parser.add_argument("--source-ref", help="Compile baseline managers as a negative control")
    parser.add_argument("--cc", help="Host C compiler (llvm-mingw on Windows)")
    args = parser.parse_args()
    cc = args.cc or shutil.which("cc")
    if not cc:
        parser.error("A host C compiler is required for the declared JNI boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    java_home = args.java_home.resolve()
    with tempfile.TemporaryDirectory(prefix="gesher-wifi-access-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        windows = os.name == "nt"
        native = work / ("wifi_access_host.dll" if windows else "libwifi_access_host.so")
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared",
                        *([] if windows else ["-fPIC"]), "-I" + str(java_home / "include"),
                        "-I" + str(java_home / "include" / ("win32" if windows else "linux")),
                        str(ROOT / "tests/wifi-access/framework_host.c"), "-o", str(native),
                        *(["-lwinpthread", "-static"] if windows else [])], check=True)
        production = (
            "app/java/com/gesher/net/WifiAccess.java",
            "app/java/com/gesher/net/IWindowsWifi.java",
            "app/java/android/net/wifi/WifiManager.java",
            "app/java/android/net/ConnectivityManager.java",
        )
        sources = []
        for relative in production:
            source = ROOT / relative
            if args.source_ref and relative.endswith(("/WifiManager.java", "/ConnectivityManager.java")):
                source = work / source.name
                source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                                 cwd=ROOT, capture_output=True, check=True).stdout)
            sources.append(str(source))
        if args.source_ref:
            # Pre-repair managers called the backend directly. Compile that real
            # dependency only for the negative control; repaired facades never do.
            sources.append(str(ROOT / "app/java/com/gesher/net/WindowsWifi.java"))
        sources += [str(ROOT / "tests/wifi-access/WifiAccessHost.java")]
        subprocess.run([str(java_home / "bin/javac"), "-J-XX:-UsePerfData", "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), *sources], check=True)
        cleaner = work / "cleaner/sun/misc"
        cleaner.mkdir(parents=True)
        inputs = list((args.core_classes.resolve() / "sun/misc").glob("Cleaner*.class"))
        if not inputs:
            raise ValueError("Android core input has no Cleaner classes")
        for entry in inputs:
            shutil.copy2(entry, cleaner / entry.name)
        subprocess.run([str(java_home / "bin/java"), "-XX:-UsePerfData",
                        "--patch-module", "jdk.unsupported=" + str(work / "cleaner"),
                        "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.net.WifiAccessHost", str(native), args.case], check=True, timeout=45)


if __name__ == "__main__":
    main()
