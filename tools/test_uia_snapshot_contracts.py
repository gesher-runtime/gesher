#!/usr/bin/env python3
"""Run production UIA snapshot/publication code at an explicit mock COM boundary.

The real C++ queue, traversal, retention, actions and JNI snapshot entry run on
the host. This covers failure/timeout ownership, not Windows COM marshaling or
WebView2's actual accessibility provider; the Windows Web3 track covers those.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("Production function boundary changed: " + signature)
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref", help="Run the same checks against an earlier production source")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    relative = "compat/accessibility/uia_client.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT).decode()
              if args.source_ref else (ROOT / relative).read_text())
    production = source[source.index("std::mutex g_mutex;"):source.index("// --- JNI")]
    production += function(source, "jstring UiaClient_nativeSnapshot(")
    constants = sorted(set(re.findall(r"\bUIA_\w+(?:PropertyId|PatternId)\b", production)))
    template = (ROOT / "tests/accessibility/uia_snapshot_host.cc.in").read_text()
    template = template.replace("@CONSTANTS@", "enum { " + ", ".join(constants) + " };")
    template = template.replace("@PRODUCTION@", production)
    print("Production UIA source SHA256:", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-uia-snapshot-") as folder:
        work = Path(folder)
        cpp, executable = work / "contracts.cc", work / "contracts"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-pthread", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(executable)], check=False, timeout=45).returncode


if __name__ == "__main__":
    raise SystemExit(main())
