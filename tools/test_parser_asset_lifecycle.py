#!/usr/bin/env python3
"""Compile real pinned parser loaders; exercise actual Java ownership with host JNI resources.

The Android-core compile and optional D8 output include the real ART fixture. Host JNI owns
ordinary fd/mmap resources and fault injection, not Android resource decoding or Windows
section semantics. Run ParserAssetLifecycleTest in the mandatory Windows suite for those.
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
LOADERS = {
    "DefaultSplitAssetLoader.java": "4ec51dfe62ef3b5c19b564a9e8a0cc3128c6e9639ef6de39f6b4081509e011da",
    "SplitAssetDependencyLoader.java": "8666e61d7c5f5739d53d95ec9c4713e281d0ad65cd9e5fca0c398f9eddb995ea",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("aosp-base", "framework-classes", "core-classes", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--r8", type=Path, help="Also compile the real fixture to DEX")
    parser.add_argument("--unpatched", action="store_true", help="Run unchanged pinned loaders as negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A host C compiler is required")
    java = args.java_home.resolve() / "bin/java"
    javac = args.java_home.resolve() / "bin/javac"
    framework = args.framework_classes.resolve()
    core = args.core_classes.resolve()
    with tempfile.TemporaryDirectory(prefix="gesher-parser-assets-") as directory:
        work = Path(directory)
        sources = []
        for name, digest in LOADERS.items():
            relative = Path("core/java/com/android/internal/pm/split") / name
            data = (args.aosp_base / relative).read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit("Expected exact android-16.0.0_r4 source: " + str(relative))
            target = work / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data); sources.append(str(target))
        if not args.unpatched:
            subprocess.run(["git", "apply", "--check", str(ROOT / "patches/frameworks/base/0783-parser-apk-assets-lifetime.patch")],
                           cwd=work, check=True)
            subprocess.run(["git", "apply", str(ROOT / "patches/frameworks/base/0783-parser-apk-assets-lifetime.patch")],
                           cwd=work, check=True)
        fixture = ROOT / "tests/dex-services/gesher/services/ParserAssetLifecycleTest.java"
        # Compile first against Android's actual core module, as the production build does.
        core_flags = ["--system=none", "--module-path", str(core)]
        packages = sorted({str(path.parent.relative_to(core)).replace(os.sep, ".")
                           for path in core.rglob("*.class") if path.name != "module-info.class"})
        for package in packages:
            core_flags += ["--add-exports", "java.base/" + package + "=ALL-UNNAMED"]
        common = [str(javac), "-source", "17", "-target", "17", "-Xlint:-options",
                  "-XDstringConcat=inline", "-encoding", "UTF-8"]
        art = work / "art"; art.mkdir()
        subprocess.run(common + core_flags + ["-cp", str(framework), "-d", str(art), *sources,
                                              str(fixture)], check=True)
        print("Actual Android-core loader/fixture compilation PASS", flush=True)
        if args.r8:
            dex = work / "dex"; dex.mkdir()
            fixture_classes = list((art / "gesher/services").glob("ParserAssetLifecycleTest*.class"))
            subprocess.run([str(java), "-cp", str(args.r8.resolve()), "com.android.tools.r8.D8",
                            "--release", "--min-api", "36", "--output", str(dex),
                            *map(str, fixture_classes)], check=True)
            print("Real ART fixture D8 PASS (" + str((dex / "classes.dex").stat().st_size) + " bytes)", flush=True)
        host = work / "host"; host.mkdir()
        tests = work / "tests"; tests.mkdir()
        classpath = str(framework) + os.pathsep + str(core)
        subprocess.run(common + ["-cp", classpath, "-d", str(host), *sources,
                                  str(ROOT / "tests/package/ParserAssetLifecycleHost.java")], check=True)
        subprocess.run(common + ["-cp", str(host) + os.pathsep + classpath, "-d", str(tests),
                                  str(fixture)], check=True)
        native = work / "libparser_assets_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-I" + str(args.java_home.resolve() / "include"),
                        "-I" + str(args.java_home.resolve() / "include/linux"),
                        str(ROOT / "tests/package/parser_assets_host.c"), "-o", str(native)], check=True)
        data = work / "data"; data.mkdir()
        subprocess.run([str(java), "-Xcheck:jni", "-Djava.io.tmpdir=" + str(work), "-cp",
                        str(host) + os.pathsep + classpath, "ParserAssetLifecycleHost",
                        str(native), str(tests), str(data)], check=True, timeout=90)


if __name__ == "__main__":
    main()
