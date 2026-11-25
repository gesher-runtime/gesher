#!/usr/bin/env python3
"""Run the production intent fallback selector with real Android framework classes.

This host test does not replace Intent/Uri/Bundle or execute WebView2 navigation.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("framework-classes", "core-classes", "java-home"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--app-classes", type=Path,
                        help="Test an already-built application jar instead of compiling the helper")
    args = parser.parse_args()
    dependencies = [args.framework_classes, args.core_classes]
    if args.app_classes:
        dependencies.insert(0, args.app_classes)
    classpath = os.pathsep.join(str(path.resolve()) for path in dependencies)
    with tempfile.TemporaryDirectory(prefix="gesher-intent-fallback-") as temporary:
        classes = Path(temporary) / "classes"
        classes.mkdir()
        sources = [ROOT / "tests/webview-intents/IntentFallbackContracts.java"]
        if not args.app_classes:
            sources.insert(0, ROOT / "app/java/com/gesher/app/webview/IntentFallback.java")
        subprocess.run([str(args.java_home / "bin/javac"), "-encoding", "UTF-8",
                        "-cp", classpath, "-d", str(classes), *map(str, sources)], check=True)
        subprocess.run([str(args.java_home / "bin/java"), "-XX:-UsePerfData",
                        "-cp", str(classes) + os.pathsep + classpath,
                        "com.gesher.app.webview.IntentFallbackContracts"], check=True, timeout=30)


if __name__ == "__main__":
    main()
