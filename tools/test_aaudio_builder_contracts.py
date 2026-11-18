#!/usr/bin/env python3
"""Run production AAudio builder, callback and query logic at a declared device seam.

The test observes actual parameters passed to fake Track/Capture factories;
no WASAPI runtime/device qualification is claimed. --negative-control bypasses
builder validation in a disposable copy and must fail unchanged assertions.
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
def section(source, start, end):
    if source.count(start) != 1 or source.count(end) != 1: raise ValueError("source boundary changed: " + start)
    a = source.index(start); return source[a:source.index(end, a)]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "compat/media/ndk/aaudio.cc").read_text()
    # Namespace wrappers are omitted; function/structure bodies are verbatim.
    types = section(source, "// aaudio/AAudio.h", "struct AAudioStream {")
    types = types.removesuffix("}  // namespace\n\n")
    sections = {
        "TYPES": types,
        "STREAM": section(source, "struct AAudioStream {", "namespace {\n\n// AAUDIO_USAGE_"),
        "CALLBACKS": section(source, "int StreamForUsage(", "int64_t Now()") + section(source, "void InputDisconnected(", "// --- AAudioStreamBuilder"),
        "BUILDERS": section(source, "aaudio_result_t SYSV CreateStreamBuilder(", "// --- AAudioStream ---------------------------------------------------------------------"),
        "CLOSE": section(source, "aaudio_result_t ReleaseStream(", "// A timeout is a duration"),
        "QUERIES": section(source, "aaudio_result_t SYSV SetBufferSizeInFrames(", "const char* SYSV StateToText("),
    }
    if args.negative_control:
        sections["BUILDERS"] = sections["BUILDERS"].replace("const aaudio_result_t validation = ValidateBuilder(*b);", "const aaudio_result_t validation = kOk;")
    template = (ROOT / "tests/media/aaudio_builder_host.cc.in").read_text()
    for name, code in sections.items():
        print(name + " production SHA256 " + hashlib.sha256(code.encode()).hexdigest(), flush=True)
        template = template.replace("@" + name + "@", code)
    with tempfile.TemporaryDirectory(prefix="gesher-aaudio-builder-") as tmp:
        folder = Path(tmp); cpp = folder / "aaudio.cc"; cpp.write_text(template); executable = folder / "test"
        subprocess.run(shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
                       "-fsanitize=undefined", "-fno-sanitize-recover=undefined", str(cpp), "-o", str(executable)], check=True, timeout=120)
        result = subprocess.run([str(executable)], check=False, timeout=30)
        return result.returncode
if __name__ == "__main__": raise SystemExit(main())
