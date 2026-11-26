#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; All rights reserved.
"""Exercise production popup handoff and retirement at declared COM/STA boundaries.

Extracts the real pending-popup state, CompleteNewWindow, Host::Destroy and
document-script/Stop/failure dispatch. The native controller's final close and
opener COM arguments are explicit fixture boundaries. No JavaScript or real
Windows/WebView2 execution is claimed by this portable test.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_webview_document_script import section

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref", help="Run contracts against an earlier production revision")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    path = "compat/webview/webview_host.cc"
    current = (ROOT / path).read_text()
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                     cwd=ROOT, text=True) if args.source_ref else current)
    production = "\n".join((
        section(source, "void Fail(Impl* impl,", "std::wstring Headers("),
        section(source, "void Host::Run(Command command)", "void Host::SetBounds("),
        section(source, "void Host::SetDocumentScript(", "void Host::RespondToRequest("),
        section(source, "void Host::CompleteNewWindow(", "void Host::CompleteBasicAuth("),
        section(source, "void Host::Destroy()", "void Host::Navigate("),
    ))
    start = source.index("[weak](", source.index("wv->add_NewWindowRequested("))
    end = source.index("\n                      })", start) + len("\n                      }")
    production += ("\nauto NewWindowRequestHandler(std::weak_ptr<Impl> weak) { return "
                   + source[start:end] + "; }\n")
    # Historical controls predate the popup bookkeeping. Supplying its unused
    # fields/member lets the same fixture observe their actual dispatch paths.
    state_source = source if "  uint64_t new_window_ticket = 0;" in source else current
    state = section(state_source, "  uint64_t new_window_ticket = 0;", "  void Report(")
    if "void DrainPendingOperations(" in source:
        drain = "  DrainPendingOperations(impl.get());"
    else:
        controller = source[source.index("void OnController("):]
        drain = section(controller, "  auto pending = std::move(impl->pending);", "  auto waiting =")
    template = (ROOT / "tests/webview/popup_script_host.cc.in").read_text()
    rendered = (template.replace("@PRODUCTION@", production)
                .replace("@POPUP_STATE@", state).replace("@DRAIN_PENDING@", drain))
    print("Popup production SHA256:", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-popup-script-") as temporary:
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
