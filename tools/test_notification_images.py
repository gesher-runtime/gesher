#!/usr/bin/env python3
"""Run the complete notification image owner, optionally with the real service/framework.

The standalone suite uses the real filesystem and an explicitly synthetic encoder.
The service suite uses constructed package metadata and the existing JNI boundary
adapter. Neither suite substitutes production behavior for ordinary applications.
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
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--java-home", type=Path)
    p.add_argument("--apksig", type=Path)
    for name in ("framework-classes", "core-classes", "app-classes"):
        p.add_argument("--" + name, type=Path)
    p.add_argument("--service-source-ref", help="Original service/icons for the unchanged cancellation probe")
    args = p.parse_args()
    java_home = args.java_home or Path(shutil.which("javac") or "").resolve().parent.parent
    java, javac = [str(java_home / "bin" / name) for name in ("java", "javac")]
    with tempfile.TemporaryDirectory(prefix="gesher-notification-images-") as temp:
        work = Path(temp); classes = work / "standalone"; classes.mkdir()
        subprocess.run([javac, "-encoding", "UTF-8", "-d", str(classes),
                        str(ROOT / "app/java/com/gesher/server/NotificationImageStore.java"),
                        str(ROOT / "tests/notification-images/SystemServer.java"),
                        str(ROOT / "tests/notification-images/NotificationImageStoreHost.java")], check=True)
        subprocess.run([java, "-cp", str(classes), "com.gesher.server.NotificationImageStoreHost",
                        str(work / "owner-files")], check=True, timeout=45)
        inputs = (args.app_classes, args.framework_classes, args.core_classes)
        if not any(inputs):
            return
        if not all(inputs):
            p.error("Supply all three --app-classes, --framework-classes and --core-classes")
        if args.apksig is None:
            p.error("The actual service fixture requires --apksig")
        cp = os.pathsep.join(str(x.resolve()) for x in (*inputs, args.apksig))
        service = work / "service"; service.mkdir()
        sources = []
        for name in ("NotificationImageStore", "NotificationIcons", "NotificationManagerService", "NotificationStore"):
            relative = f"app/java/com/gesher/server/{name}.java"
            source = ROOT / relative
            if args.service_source_ref and name in ("NotificationIcons", "NotificationManagerService"):
                source = work / f"{name}.java"
                source.write_bytes(subprocess.check_output(["git", "show", args.service_source_ref + ":" + relative], cwd=ROOT))
            sources.append(str(source))
        subprocess.run([javac, "-encoding", "UTF-8", "-cp", cp, "-d", str(service), *sources,
                        str(ROOT / "tests/dex-services/com/gesher/server/NotificationImagePublicationTest.java")], check=True)
        library = work / "libnotification_image_host.so"
        subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(ROOT / "tests/notification/framework_host.c"), "-o", str(library)], check=True)
        env = os.environ.copy(); env["ANDROID_DATA"] = str(work / "service-data")
        env["GESHER_MICROG_SPOOF"] = "0"; env.pop("GESHER_BINDER_UID", None)
        command = [java, "-cp", str(service) + os.pathsep + cp,
                   "com.gesher.server.NotificationImagePublicationTest", str(library)]
        if args.service_source_ref: command.append("cancellation-only")
        subprocess.run(command, env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
