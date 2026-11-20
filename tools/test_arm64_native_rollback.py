#!/usr/bin/env python3
"""Execute native ARM64 rollback methods with explicit host-only boundaries.

Production method bodies are extracted unchanged from dynarmic_bridge.cc.
Only Win32 identity/cache-flush calls and already-attached TLS blocks are
provided by the fixture. This does not execute ARM64 instructions or validate
Windows native TLS attachment. Use the actual AArch64 cross-build separately.
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


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("native ARM64 rollback production boundary changed")
    return source[source.index(begin):source.index(end, source.index(begin))]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "compat/arm64/dynarmic_bridge.cc").read_text(encoding="utf-8")
    sections = {
        "@REGIONS@": section(source, "struct CodeRegion {", "uint64_t NativeCall("),
        "@TLS@": section(source, "constexpr size_t kTcbSize =", "void* g_tlsdesc_static ="),
        "@CLASSIFY@": section(source, "int NativeIsGuestCode(", 'extern "C" uint64_t GesherRunAarch64Program('),
    }
    template = (ROOT / "tests/native/arm64_native_rollback_host.cc.in").read_text(encoding="utf-8")
    for marker, production in sections.items():
        if template.count(marker) != 1:
            raise ValueError("native ARM64 rollback fixture marker changed")
        template = template.replace(marker, production)
        print(marker + " production SHA256 " + hashlib.sha256(production.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-arm64-native-rollback-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = folder / "test"
        cpp.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
            str(cpp), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=45)


if __name__ == "__main__":
    main()
