#!/usr/bin/env python3
"""Test actual shortcut update/validation methods with a declared host storage boundary.

The service's query/load/publish bodies are replaced only in a temporary host
copy; real ShortcutInfo, AppShell.dynamicIntent and all update methods execute.
DynamicShortcutTest uses the same fixture on Windows with no such adapters.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def replace_body(source: str, signature: str, replacement: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("host adapter signature not unique: " + signature)
    start = source.index("{", source.index(signature))
    depth, end = 1, start + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[:start + 1] + "\n" + replacement + "\n    " + source[end - 1:]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework-classes", required=True, type=Path)
    parser.add_argument("--core-classes", required=True, type=Path)
    parser.add_argument("--app-classes", required=True, type=Path)
    parser.add_argument("--java-home", required=True, type=Path)
    parser.add_argument("--source-ref", help="Use an old service source as the same-harness negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("C compiler required for declared initialization boundaries")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix="gesher-shortcut-updates-") as temporary:
        work = Path(temporary)
        classes = work / "classes"; classes.mkdir()
        library = work / "libshortcut_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(args.java_home / "include"), "-I" + str(args.java_home / "include/linux"),
                        str(ROOT / "tests/notification/framework_host.c"), "-o", str(library)], check=True)
        relative = "app/java/com/gesher/server/DesktopShortcutService.java"
        source = (ROOT / relative).read_text()
        if args.source_ref:
            source = subprocess.run(["git", "show", args.source_ref + ":" + relative], cwd=ROOT,
                                    capture_output=True, text=True, check=True).stdout
        for signature, body in (
                ("private ApkPackage query(String pkg, int userId)", "return ShortcutUpdateHost.publisher(pkg, userId);"),
                ("private LinkedHashMap<String,ShortcutInfo> load(ApkPackage pkg)", "return ShortcutUpdateHost.load();"),
                ("private boolean publish(ApkPackage owner, Map<String,ShortcutInfo> snapshot)", "return ShortcutUpdateHost.publish(snapshot);")):
            source = replace_body(source, signature, body)
        adapted = work / "DesktopShortcutService.java"; adapted.write_text(source)
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), str(adapted),
                        str(ROOT / "tests/shortcuts/ShortcutUpdateHost.java"),
                        str(ROOT / "tests/dex-services/gesher/services/ShortcutUpdateContractTest.java")], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        environment["GESHER_SHORTCUTS"] = "0"
        subprocess.run([str(args.java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.server.ShortcutUpdateHost", str(library)],
                       check=True, env=environment, timeout=60)


if __name__ == "__main__":
    main()
