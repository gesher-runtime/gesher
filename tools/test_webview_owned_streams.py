#!/usr/bin/env python3
"""Exercise WebView content ownership and navigation at explicit COM/task boundaries.

The complete OwnedStream, extracted Host/Do functions and NavigationStarting
handler run on the host. COM and event adapters model ownership and ordering,
not Windows marshaling or WebView2 execution. The baseline SHCreateMemStream
adapter counts its documented full-payload copy.
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
    navigate = function(source, "void Host::Navigate(")
    declaration = navigate[:navigate.index("{")].strip().replace("void Host::", "void ")
    declaration = declaration.replace("int64_t number", "int64_t number = 0") + ";"
    numbered = "  int64_t api_number = 0;" in source
    legacy_number = "void Host::NumberNextNavigate(" in source
    state = ""
    callback = ""
    if numbered:
        start = source.index("  int64_t api_number = 0;")
        state = source[start:source.index("  std::wstring main_document;", start)]
        start = source.index("[weak](", source.index("wv->add_NavigationStarting("))
        end = source.index("\n                      })", start) + len("\n                      }")
        callback = "auto NavigationStartHandler(std::weak_ptr<Impl> weak) { return " + source[start:end] + "; }"
    resource_match = re.findall(r"^\s*bool main = (document && .*);$", source, re.MULTILINE)
    if len(resource_match) != 1:
        raise ValueError("Production main-frame resource boundary changed")
    # Historical source controls preserve their original two independent posts.
    # Current source sends the number in the same actual Navigate call.
    numbered_call = ("if (number > 0) host.NumberNextNavigate(number);\n"
                     "  host.Navigate(url, headers, method, std::move(body));"
                    if legacy_number else
                    "host.Navigate(url, headers, method, std::move(body), number);")
    replacements = {
        "@OWNED_STREAM@": str(helper), "@COM_POINTER@": com,
        "@DO@": function(source, "void Do("),
        "@NAVIGATE_DECLARATION@": declaration,
        "@NAVIGATE@": navigate,
        "@NUMBERED_NAVIGATION@": "1" if numbered else "0",
        "@NAVIGATION_STATE@": state,
        "@NAV_START_CALLBACK@": callback,
        "@NUMBER_DECLARATION@": "void NumberNextNavigate(int64_t number);" if legacy_number else "",
        "@NUMBER_FUNCTION@": function(source, "void Host::NumberNextNavigate(") if legacy_number else "",
        "@NUMBERED_CALL@": numbered_call,
        "@DOCUMENT_MATCH@": function(source, "bool SameDocumentUrl(") if "bool SameDocumentUrl(" in source else "",
        "@RESOURCE_MATCH@": resource_match[0],
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
