#!/usr/bin/env python3
"""Compile the complete ARM API front end; verify TLS forwarding/version gates.

DLL discovery and the API table are explicit host boundaries. The translated
storage/Attach methods run in test_arm64_pending_rollback.py, and real Windows
guest-pointer identity runs in test_arm64_tls_lifecycle.py --dll and the ELF
version fixtures. This runner does not execute a Windows DLL or ARM code.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-arm64-tls-api-") as temporary:
        folder = Path(temporary)
        sources = ("compat/arm64/arm64_runtime.cc", "compat/arm64/include/gesher/arm64_runtime.h",
                   "compat/arm64/include/gesher/arm64_translator.h", "compat/elf/include/gesher/elf_loader.h",
                   "tests/native/arm64_tls_address_api_host.cc")
        for source in sources:
            target = folder / source
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / source, target)
            print("SOURCE_SHA256 " + source + " " + hashlib.sha256(target.read_bytes()).hexdigest(), flush=True)
        windows = folder / "windows.h"
        windows.write_text("#pragma once\nusing HMODULE = void*;\nusing FARPROC = void (*)();\n"
                           "HMODULE LoadLibraryA(const char*);\nFARPROC GetProcAddress(HMODULE, const char*);\n",
                           encoding="utf-8")
        executable = folder / "test"
        flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(folder), "-I", str(folder / "compat/arm64/include"),
                 "-I", str(folder / "compat/elf/include")]
        environment = os.environ.copy()
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            previous = environment.get("ASAN_OPTIONS", "")
            environment["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
            print("ASan/UBSan fatal findings; LeakSanitizer disabled at ptrace boundary", flush=True)
        command = shlex.split(args.cxx) + flags + [str(folder / sources[0]),
                  str(folder / sources[-1]), "-o", str(executable)]
        subprocess.run(command, check=True, timeout=120)
        for mode in ("current", "old-prefix", "rejected", "missing-address", "missing-dll", "missing-init"):
            subprocess.run([str(executable), mode], env=environment, check=True, timeout=15)


if __name__ == "__main__":
    main()
