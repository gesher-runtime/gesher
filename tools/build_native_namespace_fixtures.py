#!/usr/bin/env python3
"""Build original well-formed APK, DEX and JNI ELF namespace fixtures.

No binary patching or malformed inputs are used. aapt2 owns binary manifest
encoding; javac/D8 own DEX input/output; a C toolchain owns the ELF files.
The ELF compiler must be an Android NDK x86-64 API31+ compiler; the helper
fixture links the official libnativehelper SDK stub and executes its real
Gesher provider at runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str]) -> None:
    print("COMMAND", json.dumps(command), flush=True)
    subprocess.run(command, check=True, timeout=120)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aapt2", required=True, type=Path)
    parser.add_argument("--android-jar", required=True, type=Path)
    parser.add_argument("--jdk", required=True, type=Path)
    parser.add_argument("--d8-jar", required=True, type=Path)
    parser.add_argument("--cc", required=True, help="Android NDK x86-64 API31+ C compiler")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    commands = []
    source = ROOT / "tests/native/native_namespace_jni_fixture.c"
    helper_source = ROOT / "tests/native/native_namespace_helper_fixture.c"
    compiler = shlex.split(args.cc)
    run(compiler + ["--version"])
    run([str(args.aapt2), "version"])
    run([str(args.jdk / "bin/java"), "-cp", str(args.d8_jar), "com.android.tools.r8.D8", "--version"])
    # Actual NDK imports preserve their public provider owner and ELF version
    # requirement. Runtime execution must reach the corresponding Gesher shim.
    for directory, value in (("alpha", 100), ("beta", 200), ("device/system/lib64", 300)):
        target = out / directory
        target.mkdir(parents=True, exist_ok=True)
        name = "libcatalog.gesher.so" if directory.startswith("device") else "libjnicollision.so"
        command = compiler + ["-shared", "-fPIC", "-nostdlib", "-Wl,--hash-style=both,-z,defs",
                "-Wl,-soname," + name, "-DFIXTURE_VALUE=" + str(value),
                "-I", str(args.jdk / "include"), "-I", str(args.jdk / "include/linux"),
                str(source), "-ldl", "-o", str(target / name)]
        run(command)
        commands.append(command)
    helper = out / "helper"
    helper.mkdir(exist_ok=True)
    command = compiler + ["-shared", "-fPIC", "-nostdlib", "-Wl,--hash-style=both,-z,defs",
            "-Wl,-soname,libjnihelper.so", "-I", str(args.jdk / "include"),
            "-I", str(args.jdk / "include/linux"), str(helper_source), "-lnativehelper",
            "-o", str(helper / "libjnihelper.so")]
    run(command)
    commands.append(command)
    (out / "device/system/etc").mkdir(parents=True, exist_ok=True)
    (out / "device/system/etc/public.libraries-gesher.txt").write_text("libcatalog.gesher.so 64\n")
    classes = out / "payload-classes"
    classes.mkdir(exist_ok=True)
    payload = ROOT / "tests/native/namespace-payload/gesher/nativepayload/Probe.java"
    run([str(args.jdk / "bin/javac"), "-source", "8", "-target", "8", "-d", str(classes), str(payload)])
    dex = out / "payload-dex"
    dex.mkdir(exist_ok=True)
    run([str(args.jdk / "bin/java"), "-cp", str(args.d8_jar), "com.android.tools.r8.D8",
         "--min-api", "23", "--lib", str(args.android_jar), "--output", str(dex),
         str(classes / "gesher/nativepayload/Probe.class")])
    with zipfile.ZipFile(out / "native-payload.jar", "w") as jar:
        jar.write(dex / "classes.dex", "classes.dex")
    cases = (("old", 31, 1, None, True), ("required31", 31, 2, "libcatalog.gesher.so", True),
             ("required30", 30, 2, "libabsent.gesher.so", True),
             ("missing31", 31, 2, "libabsent.gesher.so", True),
             ("optional31", 31, 2, "libabsent.gesher.so", False))
    def apk(name: str, sdk: int, version: int, library: str | None, required: bool,
            package: str = "com.gesher.nativepolicy", split: str | None = None) -> Path:
        directory = out / name
        directory.mkdir(parents=True, exist_ok=True)
        declaration = ("" if library is None else '<uses-native-library android:name="' + library
                       + '" android:required="' + str(required).lower() + '"/>')
        split_attributes = ("" if split is None else ' split="' + split + '" android:isFeatureSplit="true"')
        manifest = directory / "AndroidManifest.xml"
        manifest.write_text('<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                'package="' + package + '" android:versionCode="' + str(version) + '"' + split_attributes + '>'
                '<uses-sdk android:minSdkVersion="23" android:targetSdkVersion="' + str(sdk) + '"/>'
                '<application android:hasCode="false">' + declaration + '</application></manifest>')
        target = out / (name + ".apk")
        run([str(args.aapt2), "link", "-I", str(args.android_jar), "--manifest", str(manifest),
             "-o", str(target)])
        return target

    for case in cases:
        apk(*case)
    # Full, valid feature manifests place the native declaration only in the
    # split. The shared input directory deliberately contains unselected APKs;
    # selected-cluster preflight must parse only its explicit base/split set.
    split_package = "com.gesher.nativesplitpolicy"
    split_base = apk("split-declarations/base", 31, 2, None, True, split_package)
    selected = {}
    for role, library, required in (("required", "libabsent.gesher.so", True),
                                    ("optional", "libabsent.gesher.so", False),
                                    ("present", "libcatalog.gesher.so", True)):
        selected[role] = apk("split-declarations/split_" + role, 31, 2,
                library, required, split_package, "feature_" + role)
    shutil.copyfile(out / "old.apk", out / "split-declarations/unrelated.apk")
    for role in ("required", "optional"):
        with zipfile.ZipFile(out / ("split-" + role + "31.apks"), "w") as archive:
            archive.write(split_base, "base.apk")
            archive.write(selected[role], "split_native.apk")

    update_base = apk("split-update/base", 31, 2, None, True)
    update_split = apk("split-update/split_native", 31, 2, "libabsent.gesher.so", True,
                       split="feature_native")
    with zipfile.ZipFile(out / "split-update31.apks", "w") as archive:
        archive.write(update_base, "base.apk")
        archive.write(update_split, "split_native.apk")
    legacy_package = "com.gesher.nativesplitlegacy"
    apk("split-legacy/base", 30, 2, None, True, legacy_package)
    apk("split-legacy/split_native", 30, 2, "libabsent.gesher.so", True,
        legacy_package, "feature_native")
    hashes = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(out.rglob("*")) if path.is_file() and path.name != "provenance.json"}
    provenance = {"source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in (source, helper_source, payload,
                                                 Path(__file__).resolve())},
                  "tool_sha256": {label: hashlib.sha256(path.read_bytes()).hexdigest() for label, path in
                      (("aapt2", args.aapt2), ("android_jar", args.android_jar), ("d8_jar", args.d8_jar),
                       ("elf_compiler", Path(shutil.which(compiler[0]) or compiler[0]).resolve()))},
                  "elf_commands": commands, "artifact_sha256": hashes,
                  "boundary": "Valid original compiler/aapt2/D8 fixtures; build success is not ART or installation execution."}
    (out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
