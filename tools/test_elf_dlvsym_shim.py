#!/usr/bin/env python3
"""Run actual libdl dispatch/export/error code with explicit lookup seams.

Validates dlvsym's three-argument export, version forwarding, and thread-local
error lifetime. This is not the complete-loader ELF suite or Windows shim gate.
--source-ref compiles the old shim's corresponding section as a negative control.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def extract(body: str) -> tuple[str, str, str]:
    state = re.search(r"thread_local std::string t_dlerror;\s*thread_local bool t_has_dlerror = false;", body)
    if state is None:
        raise ValueError("production libdl error state marker changed")
    begin = body.index("// dlsym:")
    implementation = body[begin:body.index("struct BionicDlInfo", begin)]
    entries = []
    for name in ("dlsym", "dlvsym", "dlerror", "dlclose"):
        found = re.findall(r'^[ \t]*ENTRY\("' + name + r'",[^\n]+;', body, flags=re.MULTILINE)
        if len(found) != 1:
            raise ValueError("expected one production export for " + name)
        entries.append(found[0])
    return state.group(), implementation, "\n".join(entries)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="Git revision containing the complete production shim")
    args = parser.parse_args()
    reference = "working-tree snapshot"
    if args.source_ref:
        reference = subprocess.check_output(
            ["git", "rev-parse", "--verify", "--end-of-options", args.source_ref + "^{commit}"],
            cwd=ROOT, text=True).strip()
        body = subprocess.check_output(["git", "show", reference + ":compat/bionic/shim.cc"],
                                       cwd=ROOT, text=True)
    else:
        body = (ROOT / "compat/bionic/shim.cc").read_text(encoding="utf-8")
    state, implementation, entries = extract(body)
    template = (ROOT / "tests/native/elf_dlvsym_shim_host.cc.in").read_text(encoding="utf-8")
    generated = template.replace("@ERROR_STATE@", state).replace(
        "@LIBDL_IMPLEMENTATION@", implementation).replace("@LIBDL_ENTRIES@", entries)
    print("BOUNDARY production libdl dispatch/exports; explicit lookup seams; no ELF/Windows execution", flush=True)
    print("SOURCE_REF " + reference, flush=True)
    for name, text in (("complete_shim", body), ("production_libdl", state + implementation + entries),
                       ("fixture", template), ("generated_unit", generated)):
        print("SHA256 " + name + " " + hashlib.sha256(text.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-dlvsym-shim-") as temporary:
        directory = Path(temporary)
        unit, executable = directory / "dlvsym.cc", directory / "dlvsym"
        unit.write_text(generated, encoding="utf-8")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-pthread", str(unit), "-o", str(executable)]
        runtime = os.environ.copy()
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            if os.name != "nt":
                command += ["-fno-pie", "-no-pie"]
            previous = runtime.get("ASAN_OPTIONS", "")
            runtime["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
            print("SANITIZERS address,undefined; fatal findings; detect_leaks=0 (ptrace boundary)", flush=True)
        print("COMPILE_COMMAND " + json.dumps(command), flush=True)
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=30, env=runtime)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print("dlvsym shim unit failed (exit " + str(error.returncode) + ")", flush=True)
        raise SystemExit(error.returncode if error.returncode > 0 else 1)
