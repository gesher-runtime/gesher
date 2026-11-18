#!/usr/bin/env python3
"""Run the production page-port script with Node's real MessageChannel transport.

Only window and chrome.webview are adapters. Native structured cloning, port
transfer, queued delivery, ArrayBuffer and MessageEvent.ports use Node's web API.
This is not a Windows WebView2 or Android WebView end-to-end test.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default="node")
    parser.add_argument("--source-ref")
    args = parser.parse_args()
    path = "app/java/com/gesher/app/webview/PageScripts.java"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                     cwd=ROOT, text=True) if args.source_ref else (ROOT / path).read_text())
    start = source.index("    static final String PORTS = ")
    end = source.index("\n    /**", start)
    # The Java constant consists only of concatenated string literals. Decode
    # those exact literals instead of maintaining a test copy of the script.
    script = "".join(json.loads(part) for part in re.findall(r'"(?:\\.|[^"\\])*"', source[start:end]))
    print("PageScripts.PORTS SHA256", hashlib.sha256(script.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-page-ports-") as temporary:
        file = Path(temporary) / "ports.js"
        file.write_text(script)
        subprocess.run([args.node, str(ROOT / "tests/webmessage/page_port_script_test.cjs"), str(file)],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
