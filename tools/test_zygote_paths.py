#!/usr/bin/env python3
"""Exercise production Zygote path conversion with only its native executable-path boundary adapted.

The JVM check does not emulate Windows file operations or COM. The separate
DynamicShortcutTest still creates and removes a real Unicode ShellLink on ART.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Compile prior Zygote source as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if cc is None:
        parser.error("A C compiler is required for the single native host boundary")
    with tempfile.TemporaryDirectory(prefix="gesher-zygote-path-") as temporary:
        work = Path(temporary)
        classes = work / "classes"
        classes.mkdir()
        library = work / "libzygote_path_host.so"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"),
                        "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/zygote/path_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/Zygote.java"
        source = ROOT / relative
        if args.source_ref:
            source = work / "Zygote.java"
            prior = subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                   cwd=ROOT, check=True, capture_output=True)
            source.write_bytes(prior.stdout)
        subprocess.run([str(args.java_home / "bin/javac"), "-J-XX:-UsePerfData", "-J-Djava.io.tmpdir=" + str(work),
                        "-encoding", "UTF-8", "-d", str(classes), str(source),
                        str(ROOT / "app/java/com/gesher/app/VmCommandLine.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ZygotePathTest.java"),
                        str(ROOT / "tests/zygote/ZygotePathHost.java")], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData", "-Djava.io.tmpdir=" + str(work),
                        "-cp", str(classes), "ZygotePathHost", str(library)], check=True, timeout=30)


if __name__ == "__main__":
    main()
