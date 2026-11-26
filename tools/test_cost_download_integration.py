#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Connect production native cost classification to the real DownloadQueue consumer.

--downloads-root explicitly selects the DownloadManager source checkout, so this
network change remains buildable before that separate provider change is merged.
The actual WindowsNetworks parser and DownloadQueue/Requests/Transfer are compiled
unchanged. Android constants, SQLite, and the queue Environment use the existing
download host boundaries; this does not exercise Windows JNI or perform a transfer.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--downloads-root", type=Path, required=True)
    parser.add_argument("--java-home", type=Path, required=True)
    parser.add_argument("--core-classes", type=Path, required=True,
                        help="Android core classes for compiling the unmodified snapshot parser")
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--native-samples", type=Path,
                        help="Use an explicitly supplied native sample log for a negative control")
    parser.add_argument("--cache-dir", type=Path,
                        default=Path(tempfile.gettempdir()) / "gesher-download-contract-inputs")
    args = parser.parse_args()
    if not args.cxx:
        parser.error("A host C++17 compiler is required")
    downloads = args.downloads_root.resolve()
    helper = downloads / "tools/test_download_manager_contracts.py"
    spec = importlib.util.spec_from_file_location("download_contract_inputs", helper)
    if spec is None or spec.loader is None:
        raise ValueError("DownloadManager host contract helper is missing")
    inputs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inputs)
    jar = inputs.fetch(args.cache_dir, "sqlite-jdbc-3.46.1.3.jar")
    aosp = [inputs.fetch(args.cache_dir, name)
            for name in ("SQLiteQueryBuilder.java", "SQLiteTokenizer.java")]
    javac = str(args.java_home.resolve() / "bin/javac")
    java = str(args.java_home.resolve() / "bin/java")
    with tempfile.TemporaryDirectory(prefix="gesher-cost-downloads-") as temporary:
        work = Path(temporary)
        native = work / ("cost-policy.exe" if os.name == "nt" else "cost-policy")
        if args.native_samples:
            native_output = args.native_samples.read_text(encoding="utf-8")
            print(f"Using supplied native samples: {args.native_samples}", flush=True)
        else:
            subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests/metered-network/cost_policy_test.cc"),
                            "-o", str(native)], check=True)
            result = subprocess.run([str(native)], check=True, capture_output=True, text=True)
            native_output = result.stdout
        print(native_output, end="", flush=True)
        samples = work / "native-cost-samples.txt"
        samples.write_text(native_output, encoding="utf-8")
        classes = work / "classes"
        classes.mkdir()
        # Compile Android-only declarations used by untouched WindowsNetworks
        # methods without moving unrelated Android core packages into java.base.
        # The runtime below uses the ordinary host JVM; these methods are not run.
        core_patch = work / "compile-core"
        for relative in ("java/io/FileDescriptor", "java/net/InetAddress"):
            original = args.core_classes.resolve() / relative
            target = core_patch / Path(relative).parent
            target.mkdir(parents=True, exist_ok=True)
            matching = list(original.parent.glob(original.name + "*.class"))
            if not matching:
                raise ValueError(f"Android core compile input lacks {relative}")
            for source in matching:
                shutil.copy2(source, target / source.name)
        subprocess.run([javac, "-J-XX:-UsePerfData", "-encoding", "UTF-8", "-proc:none",
                        "--patch-module", "java.base=" + str(core_patch),
                        "-d", str(classes),
                        str(ROOT / "app/java/com/gesher/net/WindowsNetworks.java")], check=True)
        sources = [str(downloads / f"app/java/com/gesher/server/{name}.java")
                   for name in inputs.PRODUCTION]
        sources += [str(path) for path in (downloads / "tests/downloads/standins").rglob("*.java")]
        sources += [str(path) for path in aosp]
        sources.append(str(ROOT / "tests/metered-network/UnknownCostDownloadsHost.java"))
        classpath = os.pathsep.join((str(classes), str(jar)))
        subprocess.run([javac, "-J-XX:-UsePerfData", "-nowarn", "-encoding", "UTF-8",
                        "-proc:none", "-cp", classpath, "-d", str(classes), *sources], check=True)
        subprocess.run([java, "-XX:-UsePerfData", "-cp", classpath,
                        "com.gesher.server.UnknownCostDownloadsHost", str(samples)],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
