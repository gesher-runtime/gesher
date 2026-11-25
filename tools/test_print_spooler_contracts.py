#!/usr/bin/env python3
"""Run the production print job and PrintDocumentAdapter lifecycle contracts.

PrintSpooler is compiled unchanged apart from its package; the adapter and the
Windows host are scripted stand-ins in tests/print/PrintSpoolerHost.java.in.
Covers job states, the spooler's adapter call order, layout -> write with page
ranges, cancellation at each stage, time-outs, failures and restart.
Use --source-ref to run against another revision.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--java-home", default=os.environ.get("JAVA_HOME"))
    args = parser.parse_args()
    relative = "app/java/com/gesher/server/PrintSpooler.java"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative],
                                      cwd=ROOT, text=True) if args.source_ref
              else (ROOT / relative).read_text())
    print("PrintSpooler SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    source = re.sub(r"^package .*;\n", "", source, flags=re.MULTILINE)
    if re.search(r"^import (?!java\.)", source, flags=re.MULTILINE):
        raise SystemExit("PrintSpooler must stay plain Java for this harness")
    executable = lambda name: str(Path(args.java_home) / "bin" / name) if args.java_home else name
    with tempfile.TemporaryDirectory(prefix="gesher-print-spooler-") as temporary:
        work = Path(temporary)
        (work / "PrintSpooler.java").write_text(source)
        host = work / "PrintSpoolerHost.java"
        host.write_text((ROOT / "tests/print/PrintSpoolerHost.java.in").read_text())
        subprocess.run([executable("javac"), "-d", str(work), str(work / "PrintSpooler.java"),
                        str(host)], check=True, timeout=120)
        subprocess.run([executable("java"), "-cp", str(work), "PrintSpoolerHost"],
                       check=True, timeout=120)


if __name__ == "__main__":
    main()
