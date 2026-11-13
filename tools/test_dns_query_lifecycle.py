#!/usr/bin/env python3
"""Check exact raw-DNS ownership code and app callers with deterministic fd reuse.

The production Query/Transfer/result/cancel bodies are extracted verbatim.
Queries are seeded with local socketpairs and completed packets; there is no
DNS server, Windows adapter, ART or uncontrolled descriptor allocator here.
Only close observation (delegating to real close), NDK forwarding and an app
poll timeout are seams. Real Windows/NDK/wire/proxy paths remain in track 54.
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
PRODUCTION = "compat/posix/src/network_dns.cc"
APP = "tests/native/web3_network.c"


def method(source: str, marker: str) -> str:
    if source.count(marker) != 1:
        raise ValueError("source boundary changed: " + marker)
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    # Selected methods have no braces in comments or string literals.
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise ValueError("unbalanced source method: " + marker)
    return source[start:end]


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("source section changed: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def read_source(path: str, ref: str | None = None) -> str:
    if ref:
        return subprocess.run(["git", "show", ref + ":" + path], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout
    return (ROOT / path).read_text(encoding="utf-8")


def generate(production: str, app: str, omit_close: str | None = None) -> str:
    result = method(production, 'extern "C" int gesher_res_nresult(')
    cancel = method(production, 'extern "C" void gesher_res_cancel(')
    if omit_close:
        selected = result if omit_close == "result" else cancel
        if selected.count("  close(fd);") != 1:
            raise ValueError("close-omission control boundary changed")
        selected = selected.replace("  close(fd);", "  // Negative control: omit query close.")
        if omit_close == "result":
            result = selected
        else:
            cancel = selected
    consumers = []
    for marker in ("static int consume_result(", "static void cancel_query("):
        if marker in app:
            consumers.append(method(app, marker))
    small = section(app, "  struct pollfd short_result", '  stage = "DNS cancellation')
    cancellation = section(app, '  stage = "DNS cancellation',
                           '  stage = "invalid query flags/network";')
    query_guard = "  if (!ok) goto done;\n"
    if cancellation.count(query_guard) < 1:
        raise ValueError("app cancellation guard changed")
    cancellation = cancellation.split(query_guard, 1)[1]
    cleanup = section(app, "done:\n  failure_errno = errno;\n",
                      "  android_setprocnetwork(NETWORK_UNSPECIFIED);\n  (*env)->ReleaseStringUTFChars")
    cleanup = cleanup.removeprefix("done:\n  failure_errno = errno;\n")
    parts = {
        "@QUERY_STATE@": section(production, "struct Query {", "bool Transfer("),
        "@TRANSFER@": method(production, "bool Transfer("),
        "@NRESULT@": result,
        "@CANCEL@": cancel,
        "@APP_CONSUMERS@": "\n\n".join(consumers),
        "@APP_RESULT@": method(app, "static int result("),
        "@APP_SMALL@": small,
        "@APP_CANCELLATION@": cancellation,
        "@APP_CLEANUP@": cleanup,
    }
    template = (ROOT / "tests/native/dns_query_lifecycle_host.cc.in").read_text(encoding="utf-8")
    for marker, text in parts.items():
        if template.count(marker) != 1:
            raise ValueError("fixture boundary changed: " + marker)
        template = template.replace(marker, text)
    return template


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--production-source-ref")
    parser.add_argument("--app-source-ref", help="compile an earlier real app fixture as a control")
    parser.add_argument("--case", choices=("all", "ownership", "app"), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    production = read_source(PRODUCTION, args.production_source_ref)
    app = read_source(APP, args.app_source_ref)
    for name, source in ((PRODUCTION, production), (APP, app)):
        print(name + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-dns-query-") as temporary:
        work = Path(temporary)

        def build(name: str, omission: str | None = None) -> Path:
            source = work / (name + ".cc")
            binary = work / name
            source.write_text(generate(production, app, omission), encoding="utf-8")
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(source), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer"]
            subprocess.run(command, check=True, timeout=120)
            return binary

        def run(binary: Path, case: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run([str(binary), case], capture_output=True, text=True,
                                  check=False, timeout=30)

        binary = build("dns-query")
        cases = ("ownership", "app") if args.case == "all" else (args.case,)
        for case in cases:
            outcome = run(binary, case)
            print(outcome.stdout, end="", flush=True)
            if outcome.returncode:
                failed = True
                print(outcome.stderr, end="", flush=True)
        if args.case != "app":
            for omission, case in (("result", "result-success"), ("cancel", "cancel-basic")):
                outcome = run(build("omit-" + omission + "-close", omission), case)
                expected = {
                    "FAIL " + omission + ": exactly one query descriptor close",
                    "FAIL " + omission + ": descriptor closed before return",
                }
                found = {line for line in outcome.stdout.splitlines() if line.startswith("FAIL ")}
                summary = re.search(r"dns query [a-z-]+: \d+ checks, 2 failures", outcome.stdout)
                if outcome.returncode != 1 or found != expected or not summary or outcome.stderr:
                    failed = True
                    print("FAIL " + omission + " close-omission control\n" +
                          outcome.stdout + outcome.stderr, flush=True)
                else:
                    print("PASS " + omission +
                          " close-omission control: compiled; both ownership assertions reject it",
                          flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
