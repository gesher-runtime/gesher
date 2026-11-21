#!/usr/bin/env python3
"""Check unchanged production mask entry points against Android mask contracts.

Self, guest-code classification, Dispatch and queue accounting are explicit host
fixture boundaries.
The actual pending container and extracted action/claim/delivery helpers execute
alongside the mask entry points. This tests mask arithmetic, preflight, pending
records and error/output rules, not Windows thread registration or guest handler
execution. The shared native guest fixture separately exercises the real APIs.
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
        raise ValueError("missing or ambiguous function: " + signature)
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Run the same cases against a historical implementation")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    def read_source(path: str) -> str:
        return (subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                        cwd=ROOT, text=True) if args.source_ref else
                (ROOT / path).read_text())

    source = read_source("compat/bionic/signal_bionic.cc")
    shim = read_source("compat/bionic/shim_internal.h")
    pending = read_source("compat/posix/include/gesher/pending_signals.h")
    declarations = []
    for key in ("kNsig", "kSigrtMin", "kSigDfl", "kSaSiginfo", "kSaResethand",
                "kSigBlock", "kSigkill", "kSiUser", "kEAGAIN", "kEINVAL", "kENOSYS"):
        origin = shim if key in ("kEAGAIN", "kEINVAL", "kENOSYS") else source
        matches = re.findall(r"^constexpr (?:int|uintptr_t|uint32_t) " + key + r"[^\n]+",
                             origin, re.M)
        if len(matches) != 1:
            raise ValueError("missing or ambiguous constant: " + key)
        declarations.extend(matches)
    bodies = [function(source, "uint64_t Bit("),
              function(source, "int BIONIC_SYSV PthreadSigmask("),
              function(source, "int BIONIC_SYSV Sigprocmask(")]
    helpers = [function(source, "bool DefaultStops("),
               function(source, "bool ActionSupported("),
               function(source, "bool ClaimActionLocked("),
               function(source, "void Pop("),
               function(source, "int TakeDeliverable("),
               function(source, "int DeliverAll(")]
    structs = [function(source, "struct " + name + " {") + ";"
               for name in ("BionicSigaction", "BionicSiginfo", "BionicStack")]
    replacements = {
        "@DECLARATIONS@": "\n".join(declarations + structs + bodies[:1]),
        "@ACTION_HELPERS@": "\n".join(helpers[:5]),
        "@DELIVER_ALL@": helpers[5],
        "@ENTRY_POINTS@": "\n".join(bodies[1:]),
    }
    fixture = (ROOT / "tests/native/bionic_signal_mask_host_test.cc.in").read_text()
    for marker, content in replacements.items():
        if fixture.count(marker) != 1:
            raise ValueError("missing or ambiguous fixture marker: " + marker)
        fixture = fixture.replace(marker, content)
    print("production source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    print("production entry bodies SHA256 " + hashlib.sha256("\n".join(bodies).encode()).hexdigest(), flush=True)
    print("production delivery helpers SHA256 " + hashlib.sha256("\n".join(helpers).encode()).hexdigest(), flush=True)
    print("production pending container SHA256 " + hashlib.sha256(pending.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-bionic-masks-") as temporary:
        directory = Path(temporary)
        cpp, executable = directory / "mask.cc", directory / "mask"
        cpp.write_text(fixture)
        (directory / "gesher").mkdir()
        (directory / "gesher/pending_signals.h").write_text(pending)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-Wno-unused-const-variable", "-pthread", "-iquote", str(directory),
                   str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=60)
        return subprocess.run([str(executable)], timeout=10).returncode


if __name__ == "__main__":
    raise SystemExit(main())
