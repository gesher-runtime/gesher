#!/usr/bin/env python3
"""Run SDK36 Wi-Fi configuration callers over freshly built production value classes.

The host adapter supplies native initialization/logging and a real elapsed clock. It does not implement
Parcel, certificates, Wi-Fi services, or the value contracts. The identical Java
fixture requires native Parcel when run by the Windows ART gate. --source-ref
replaces only the old WifiConfiguration class after compiling the same dependency
family, so the missing SDK copy constructor is an executable negative control.
"""
from __future__ import annotations

import argparse
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAMILY = ("WifiConfiguration", "WifiEnterpriseConfig", "SecurityParams", "ParcelUtil",
          "OuiKeyedData", "util/PersistableBundleUtils")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "sdk-jar", "framework-classes", "core-classes", "app-classes"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--source-ref", help="Compile the old WifiConfiguration as a negative control")
    parser.add_argument("--output-dir", type=Path, help="Retain evidence in a new, empty directory")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the declared initialization boundary")
    classpath = os.pathsep.join(str(path.resolve()) for path in
                               (args.app_classes, args.framework_classes, args.core_classes))
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with contextlib.ExitStack() as stack:
        if args.output_dir:
            work = args.output_dir.resolve()
            work.mkdir(parents=True, exist_ok=False)
        else:
            work = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="gesher-wifi-config-")))
        classes, callers = work / "classes", work / "sdk-callers"
        classes.mkdir()
        callers.mkdir()
        library = work / "libwifi_configuration_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home.resolve() / "include"),
                        "-I" + str(args.java_home.resolve() / "include/linux"),
                        str(ROOT / "tests/wifi-configuration/framework_host.c"), "-o", str(library)],
                       check=True)
        # No framework/app classes on this compilation path: the SDK binds every public call site.
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp",
                        str(args.sdk_jar.resolve()), "-d", str(callers),
                        str(ROOT / "tests/dex-services/gesher/services/WifiConfigurationContractTest.java"),
                        str(ROOT / "tests/dex-services/gesher/services/WifiConfigurationCertificates.java")],
                       check=True)
        sources = [str(ROOT / "app/java/android/net/wifi" / (name + ".java")) for name in FAMILY]
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *sources], check=True)
        if args.source_ref:
            relative = "app/java/android/net/wifi/WifiConfiguration.java"
            source = work / "old-source" / "WifiConfiguration.java"
            source.parent.mkdir()
            source.write_bytes(subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                              cwd=ROOT, capture_output=True, check=True).stdout)
            subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp",
                            os.pathsep.join((str(classes), classpath)), "-d", str(classes),
                            str(source)], check=True)
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-cp", str(callers),
                        "-d", str(callers), str(ROOT / "tests/wifi-configuration/WifiConfigurationHost.java")],
                       check=True)
        subprocess.run([java, "-XX:-UsePerfData", "-XX:+UnlockDiagnosticVMOptions",
                        "-XX:-BytecodeVerificationRemote", "-cp",
                        os.pathsep.join((str(classes), str(callers), classpath)),
                        "gesher.services.WifiConfigurationHost", str(library), str(classes)],
                       check=True, timeout=90)
        print("Public SDK36 callers; actual production Wi-Fi configuration providers; "
              "real host certificate parsing. Native Parcel is a separate ART gate.")


if __name__ == "__main__":
    main()
