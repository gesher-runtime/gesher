#!/usr/bin/env python3
"""Run complete persistent-Surface JNI and InputSurface on a checked host JVM.

The Java Surface/native-window adapter is explicit; this does not exercise ART,
Skia, EGL, a Windows encoder or Binder. The same operation has a public APK gate.
Factory/release entrypoints are extracted verbatim from the production codec JNI.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
from test_codec_lifecycle_contracts import method

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/media/persistent-input-host"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    java = args.java_home or Path(shutil.which("javac") or "missing").resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gesher-persistent-jni-") as directory:
        work = Path(directory)
        units = []
        for path in ("compat/media/jni/persistent_input_surface.h", "compat/media/jni/persistent_input_surface.cc",
                     "compat/media/video/input_surface.h", "compat/media/video/input_surface.cc"):
            target = work / path
            target.parent.mkdir(parents=True, exist_ok=True)
            source = (ROOT / path).read_bytes()
            target.write_bytes(source)
            print(path + " SHA256 " + hashlib.sha256(source).hexdigest(), flush=True)
            if target.suffix == ".cc": units.append(str(target))
        queue = work / "compat/media/surface/queue_surface.h"
        queue.parent.mkdir(parents=True)
        queue.write_bytes((ROOT / "tests/media/input_surface_queue_host.h").read_bytes())
        runtime = work / "android_runtime/android_view_Surface.h"
        runtime.parent.mkdir()
        runtime.write_text('#pragma once\n#include <jni.h>\n'
                           '#include "compat/media/surface/queue_surface.h"\n'
                           'namespace android { jobject android_view_Surface_createFromSurface('
                           'JNIEnv*, const sp<Surface>&); }\n')
        codec = (ROOT / "compat/media/jni/media_codec_jni.cc").read_text()
        fixture = (FIXTURE / "jni.cc").read_text()
        for key, marker in (("JNI_CREATE", "jobject JNICALL MC_createPersistentInputSurface("),
                            ("JNI_RELEASE", "void JNICALL MC_releasePersistentInputSurface(")):
            assert fixture.count("@" + key + "@") == 1
            fixture = fixture.replace("@" + key + "@", method(codec, marker))
        host = work / "host.cc"; host.write_text(fixture)
        library = work / "libpersistent_input.so"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-write-strings", "-shared", "-fPIC", "-pthread", "-fsanitize=undefined",
            "-fno-sanitize-recover=undefined", "-I" + str(work), "-I" + str(java / "include"),
            "-I" + str(java / "include/linux"), *units, str(host), "-o", str(library)]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(java / "bin/javac"), "-nowarn", "-d", str(work),
                        *map(str, FIXTURE.glob("*.java"))], check=True, timeout=60)
        result = subprocess.run([str(java / "bin/java"), "-Xcheck:jni", "-XX:-UsePerfData",
            "-XX:-CreateCoredumpOnCrash", "-cp", str(work), "PersistentInputHost", str(library)],
            capture_output=True, text=True, timeout=60)
        print(result.stdout, end=""); print(result.stderr, end="")
        return int(result.returncode != 0 or "WARNING in native method" in result.stdout)


if __name__ == "__main__":
    raise SystemExit(main())
