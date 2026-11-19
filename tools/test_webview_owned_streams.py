#!/usr/bin/env python3
"""Exercise production WebView content ownership at explicit COM/task boundaries.

The complete OwnedStream and unchanged extracted Host/Do functions run on the
host. The COM adapter models interface ownership, not Windows marshaling. The
baseline SHCreateMemStream adapter counts its documented full-payload copy.
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


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("Production function boundary changed: " + signature)
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref", help="Exact earlier Host dispatch; helper contract suite still uses current OwnedStream")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--measure", action="store_true", help="Run only the same dispatch measurements, without new allocation budgets")
    args = parser.parse_args()
    relative = "compat/webview/webview_host.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT).decode()
              if args.source_ref else (ROOT / relative).read_text())
    helper = ROOT / "compat/webview/owned_stream.h"
    com = (ROOT / "compat/webview/com_util.h").read_text()
    com = com[com.index("template <typename T>\nclass Com"):com.index("// A string WebView2 allocated")]
    template = (ROOT / "tests/webview/owned_stream_host.cc.in").read_text()
    replacements = {
        "@OWNED_STREAM@": str(helper), "@COM_POINTER@": com,
        "@DO@": function(source, "void Do("),
        "@NAVIGATE@": function(source, "void Host::Navigate("),
        "@RESPOND@": function(source, "void Host::RespondToRequest("),
    }
    for key, value in replacements.items():
        template = template.replace(key, value)
    print("Host dispatch SHA256:", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("Owned stream SHA256:", hashlib.sha256(helper.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-webview-owned-") as folder:
        work = Path(folder)
        cpp, executable = work / "contracts.cc", work / "contracts"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-mismatched-new-delete", "-fno-exceptions", "-pthread",
            "-I", str(ROOT / "tests/webview/owned-stream-host"), str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(executable)] + (["measure"] if args.measure else []), check=False, timeout=60).returncode


if __name__ == "__main__":
    raise SystemExit(main())
