#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; All rights reserved.
"""Exercise production document-script and navigation queues at a COM/STA boundary.

The production dispatch, script completion, readiness draining and Host command
functions are extracted verbatim. The fixture supplies controller identity,
asynchronous COM completion and an ordered STA task queue. It does not execute
JavaScript or claim Windows/WebView2 runtime coverage.
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


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1:
        raise ValueError("Production boundary changed: " + start)
    first = source.index(start)
    return source[first:source.index(end, first)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref", help="Run the same contracts against an earlier production revision")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    path = "compat/webview/webview_host.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                     cwd=ROOT, text=True) if args.source_ref else (ROOT / path).read_text())
    production = "\n".join((
        section(source, "void Fail(Impl* impl,", "std::wstring Headers("),
        section(source, "void Host::NavigateToString(", "void Host::SetBridgeEndpoint("),
        section(source, "void Host::Run(Command command)", "void Host::SetBounds("),
        section(source, "void Host::SetDocumentScript(", "void Host::RespondToRequest("),
    ))
    if "void DrainPendingOperations(" in source:
        drain = "  DrainPendingOperations(impl.get());"
    else:
        # Historical controls execute their original controller-ready loop too.
        controller = source[source.index("void OnController("):]
        drain = section(controller, "  auto pending = std::move(impl->pending);", "  auto waiting =")
    template = (ROOT / "tests/webview/document_script_host.cc.in").read_text()
    rendered = template.replace("@PRODUCTION@", production).replace("@DRAIN_PENDING@", drain)
    print("Document-script production SHA256:", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-document-script-") as temporary:
        work = Path(temporary)
        cpp, binary = work / "contracts.cc", work / ("contracts.exe" if os.name == "nt" else "contracts")
        cpp.write_text(rendered)
        command = shlex.split(args.cxx) + ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                          str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
