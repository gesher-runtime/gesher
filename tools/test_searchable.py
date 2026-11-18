#!/usr/bin/env python3
"""Exercise production searchable lookup with explicit native identity and resource boundaries."""
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
    for name in ("framework-classes", "core-classes", "app-classes", "apksig", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--old-visibility-control", action="store_true",
                        help="Restore the reviewed PR's package-equality denial as a negative control")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("A C compiler is required for the existing host JNI boundary adapters")
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    run_searchable(java_home=args.java_home, classpath=classpath, cc=cc,
                   old_visibility_control=args.old_visibility_control)


def run_searchable(*, java_home: Path, classpath: str, cc: str,
                   old_visibility_control: bool = False) -> None:
    with tempfile.TemporaryDirectory(prefix="gesher-searchable-") as temporary:
        work = Path(temporary); classes = work / "classes"; classes.mkdir()
        library = work / "libsearchable_host.so"
        subprocess.run([cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-shared", "-fPIC",
                        "-I" + str(java_home / "include"), "-I" + str(java_home / "include/linux"),
                        str(ROOT / "tests/uri/framework_host.c"), "-o", str(library)], check=True)
        service = ROOT / "app/java/com/gesher/server/SearchManagerService.java"
        if old_visibility_control:
            contents = service.read_text()
            needle = "        boolean system = mCallers.isSystem();"
            assert contents.count(needle) == 1
            contents = contents.replace(needle, "        if (!mCallers.isSystem() && !name.equals(mCallers.callerPackage())) return null;\n" + needle)
            service = work / "SearchManagerService.java"
            service.write_text(contents)
        sources = [service, ROOT / "app/java/com/gesher/server/PackageManagerService.java",
                   ROOT / "app/java/com/gesher/server/PackageVisibility.java",
                   ROOT / "tests/search/SearchableHost.java"]
        subprocess.run([str(java_home / "bin/javac"), "-encoding", "UTF-8", "-cp", classpath,
                        "-d", str(classes), *map(str, sources)], check=True)
        environment = os.environ.copy()
        environment["ANDROID_DATA"] = str(work / "data")
        java = [str(java_home / "bin/java"), "-cp", str(classes) + os.pathsep + classpath,
                "gesher.search.SearchableHost", str(library)]
        environment["GESHER_MICROG_SPOOF"] = "1"
        subprocess.run(java, env=environment, check=True, timeout=90)
        environment["GESHER_MICROG_SPOOF"] = "0"
        subprocess.run([*java, "legacy"], env=environment, check=True, timeout=90)


if __name__ == "__main__":
    main()
