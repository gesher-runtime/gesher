#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run actual Bitmap parcel/storage code with pinned hwui ownership methods.

The complete production JNI/helper units and unmodified selected methods/class
declaration from Android16 r4 execute over explicit Skia/Java transport seams.
Descriptors, mmap, seals, cross-process transfer and the checked JVM are real.
The separate Windows public SDK/Binder fixture qualifies the actual platform.
"""
from __future__ import annotations
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import urllib.request
from test_codec_lifecycle_contracts import method

ROOT = Path(__file__).resolve().parents[1]
PIN = "https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/"
SOURCES = {
    "Bitmap.cpp": ("libs/hwui/hwui/Bitmap.cpp", "8c85f818808825f8633ba5e2239564bf6956c385ad7365b85755e96f5380c9f4"),
    "Bitmap.h": ("libs/hwui/hwui/Bitmap.h", "fd67663f5ccd83e7c8c4ddf13b2433b2c3bf8091261c971c5b770ab97e12e2b7"),
    "Bitmap.java": ("graphics/java/android/graphics/Bitmap.java", "f3bcf7f924e1dec7266ade88c6cff03be6bada3124d740ee05dba4fe3d6d18e7"),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-dir", type=Path, help="Cached exact pinned Bitmap.cpp/.h/.java")
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="ASan/UBSan native storage; JVM always uses CheckJNI")
    parser.add_argument("--source-ref", help="Unmodified production baseline; run the same pixel/work chain")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--keep-source-dir", type=Path, help="Retain generated native source and explicit headers for review")
    args = parser.parse_args()
    java = args.java_home or Path(shutil.which("javac") or "missing").resolve().parent.parent
    fixture = ROOT / "tests/bitmap-shared"
    with tempfile.TemporaryDirectory(prefix="gesher-shared-bitmap-") as directory:
        work = Path(directory)
        reference = {}
        for name, (relative, expected) in SOURCES.items():
            raw = (args.reference_dir / name).read_bytes() if args.reference_dir else base64.b64decode(
                urllib.request.urlopen(PIN + relative + "?format=TEXT", timeout=45).read())
            actual = hashlib.sha256(raw).hexdigest()
            if actual != expected: raise SystemExit("Pinned Android source mismatch: " + name + " " + actual)
            reference[name] = raw.decode()
            print("ANDROID " + name + " " + actual, flush=True)
        pinned = work / SOURCES["Bitmap.cpp"][0]
        pinned.parent.mkdir(parents=True); pinned.write_text(reference["Bitmap.cpp"])
        if not args.source_ref:
            patch = ROOT / "patches/frameworks/base/0785-bitmap-shared-storage-gesher.patch"
            subprocess.run(["git", "apply", "--check", str(patch)], cwd=work, check=True)
            subprocess.run(["git", "apply", str(patch)], cwd=work, check=True)
        source = pinned.read_text()
        print("PATCHED Bitmap.cpp " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
        markers = ["uint64_t Bitmap::getId(PixelStorageType type)", "bool Bitmap::computeAllocationSize(",
            "sk_sp<Bitmap> Bitmap::allocateAshmemBitmap(SkBitmap* bitmap)",
            "sk_sp<Bitmap> Bitmap::allocateHeapBitmap(SkBitmap* bitmap)",
            "sk_sp<Bitmap> Bitmap::allocateHeapBitmap(const SkImageInfo& info)",
            "sk_sp<Bitmap> Bitmap::allocateHeapBitmap(size_t size,",
            "sk_sp<Bitmap> Bitmap::createFrom(const SkImageInfo& info, SkPixelRef& pixelRef)",
            "sk_sp<Bitmap> Bitmap::createFrom(const SkImageInfo& info, size_t rowBytes, int fd, void* addr,",
            "static SkImageInfo validateAlpha(",
            "Bitmap::Bitmap(void* address, size_t size,",
            "Bitmap::Bitmap(SkPixelRef& pixelRef,", "Bitmap::Bitmap(void* address, int fd,",
            "Bitmap::~Bitmap()", "int Bitmap::getAshmemFd() const", "size_t Bitmap::getAllocationByteCount() const",
            "void Bitmap::getSkBitmap(SkBitmap* outBitmap)"]
        if args.source_ref:
            markers.insert(2, "sk_sp<Bitmap> Bitmap::allocateAshmemBitmap(size_t size,")
        allocation = source[source.index("typedef sk_sp<Bitmap> (*AllocPixelRef)"):source.index("std::string Bitmap::getAshmemId")]
        # Preserve the pinned function's signed height/size_t comparison exactly;
        # silence only that GCC diagnostic around the unmodified AOSP methods.
        methods = '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wsign-compare"\nnamespace android {\n'
        methods += allocation + "\n" + "\n\n".join(method(source, m) for m in markers)
        methods += "\n" + source[source.index("std::mutex Bitmap::mLock{}"):source.rindex("}  // namespace android")]
        methods += "\n} // namespace android\n#pragma GCC diagnostic pop\n"
        include = work / "include"; (include / "hwui").mkdir(parents=True)
        (include / "hwui/Bitmap.h").write_text(reference["Bitmap.h"])
        shutil.copyfile(fixture / "skia_boundary.h", include / "skia_boundary.h")
        for name in ("SkBitmap", "SkColorFilter", "SkColorSpace", "SkData", "SkImage", "SkImageInfo", "SkPixelRef", "SkRefCnt"):
            (include / (name + ".h")).write_text('#pragma once\n#include "skia_boundary.h"\n')
        for name in ("cutils/compiler.h", "utils/StrongPointer.h"):
            path = include / name; path.parent.mkdir(exist_ok=True); path.write_text('#pragma once\n#include "skia_boundary.h"\n')
        (include / "GraphicsJNI.h").write_text('''#pragma once
#include <jni.h>
#include <hwui/Bitmap.h>
namespace android::bitmap {
constexpr int kBitmapCreateFlag_Premultiplied=1, kBitmapCreateFlag_Mutable=2;
Bitmap& toBitmap(jlong);
jobject createBitmap(JNIEnv*, Bitmap*, int, jbyteArray, jobject, int);
}
''')
        native = include / "nativehelper"; native.mkdir()
        (native / "JNIHelp.h").write_text('''#pragma once
#include <jni.h>
int jniThrowException(JNIEnv*, const char*, const char*);
int jniThrowRuntimeException(JNIEnv*, const char*);
int jniThrowNullPointerException(JNIEnv*, const char*);
int jniRegisterNativeMethods(JNIEnv*, const char*, const JNINativeMethod*, int);
''')
        (native / "JNIPlatformHelp.h").write_text('''#pragma once
#include <jni.h>
jobject jniCreateFileDescriptor(JNIEnv*, int);
int jniGetFDFromFileDescriptor(JNIEnv*, jobject);
''')
        (include / "log").mkdir(); (include / "log/log.h").write_text('#pragma once\n#define ALOGW(...) ((void)0)\n')
        for name in ("bitmap_parcel.cc", "shared_bitmap.cc", "shared_bitmap.h"):
            relative = "compat/hwui/surfaces/" + name
            if args.source_ref and name != "bitmap_parcel.cc": continue
            data = subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT) if args.source_ref else (ROOT / relative).read_bytes()
            (work / name).write_bytes(data)
            print("PRODUCTION " + name + " " + hashlib.sha256(data).hexdigest(), flush=True)
        host = (fixture / "host.cc.in").read_text().replace("@AOSP_BITMAP_METHODS@", methods)
        host = host.replace("@STORAGE_MAIN@", (fixture / "storage_main.cc.in").read_text())
        host = "#define BITMAP_BASELINE " + str(int(bool(args.source_ref))) + "\n" + host
        unit = work / "host.cc"; unit.write_text(host)
        if args.keep_source_dir:
            shutil.copytree(work, args.keep_source_dir)
        compiler = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-write-strings", "-DGESHER",
            "-I" + str(include), "-I" + str(java / "include"), "-I" + str(java / "include/linux")]
        if not args.source_ref:
            command = compiler + [str(unit), "-o", str(work / "storage")]
            if args.sanitize: command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(work / "storage")], check=True, timeout=30)
        library = work / "libbitmap_shared_host.so"
        subprocess.run(compiler + ["-shared", "-fPIC", str(unit), "-o", str(library)], check=True, timeout=120)
        java_sources = work / "java"; shutil.copytree(fixture / "java", java_sources)
        bitmap = java_sources / "android/graphics/Bitmap.java.in"
        assert bitmap.read_text().count("@AS_SHARED@") == 1
        bitmap.with_suffix("").write_text(bitmap.read_text().replace("@AS_SHARED@", method(reference["Bitmap.java"], "public Bitmap asShared()")))
        classes = work / "classes"; classes.mkdir()
        subprocess.run([str(java / "bin/javac"), "-d", str(classes), *map(str, java_sources.rglob("*.java"))], check=True, timeout=60)
        result = subprocess.run([str(java / "bin/java"), "-Xcheck:jni", "-cp", str(classes), "bitmaphost.Host", str(library),
                                 "work" if args.source_ref else "contracts"], capture_output=True, text=True, timeout=120)
        warnings = [line for line in result.stdout.splitlines() if line.startswith("WARNING in native method:")]
        if args.source_ref:
            # The original JNI source omits checks between Java Parcel calls.
            # Preserve/record that exact baseline; do not fix it for the work
            # comparison or relabel its CheckJNI warnings as a contract pass.
            print("BASELINE work-only CheckJNI warnings: " + str(len(warnings)), flush=True)
            print("\n".join(line for line in result.stdout.splitlines() if line.startswith("WORK ")), flush=True)
        else:
            print(result.stdout, end="", flush=True)
        if result.stderr: print(result.stderr, end="", flush=True)
        unexpected_warning = any(not line.startswith("WARNING in native method: JNI call made without checking exceptions when required to from Call") for line in warnings)
        if result.returncode != 0 or result.stderr or unexpected_warning or (warnings and not args.source_ref):
            raise SystemExit("Checked JVM failed or reported JNI warnings")
        measurements = [json.loads(line.removeprefix("WORK ")) for line in result.stdout.splitlines() if line.startswith("WORK ")]
        assert len(measurements) == 6
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=True)
            (args.output_dir / "measurements.json").write_text(json.dumps(measurements, indent=2) + "\n")
            (args.output_dir / "jvm.log").write_text(result.stdout + result.stderr)
        print("Bitmap baseline pixel/work control passed" if args.source_ref else
              "Bitmap shared production storage/JNI pixel and ownership gates passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
