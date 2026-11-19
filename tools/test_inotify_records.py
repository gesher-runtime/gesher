#!/usr/bin/env python3
"""Run unchanged inotify record processing with explicit host identity boundaries.

Native Windows watches remain covered by posix_probe. These bounded records
exercise rename identity, one-shot lifetime and truthful event-loss reporting.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile old production source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    relative = "compat/posix/src/inotify.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / relative).read_text())
    digest = hashlib.sha256(source.encode()).hexdigest()
    print(relative + " SHA256 " + digest, flush=True)
    template = (ROOT / "tests/native/inotify_records_host_test.cc.in").read_text()
    for start, end, marker in [
        ("struct NotifyExtended {", "constexpr int kReadDirectoryNotifyExtendedInformation", "@PRODUCTION_NOTIFY_HEADER@"),
        ("void Inotify::Changes(", "void Inotify::Complete(", "@PRODUCTION_INOTIFY_CHANGES@"),
    ]:
        if source.count(start) != 1 or source.count(end) != 1 or template.count(marker) != 1:
            raise ValueError("Production inotify extraction boundaries changed")
        production = source[source.index(start):source.index(end)]
        template = template.replace(marker, production)
    with tempfile.TemporaryDirectory(prefix="gesher-inotify-records-") as temporary:
        work = Path(temporary)
        generated, binary = work / "contracts.cc", work / "contracts"
        generated.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                          "-Wno-unused-function", str(generated), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
