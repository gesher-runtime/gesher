#!/usr/bin/env python3
"""Run the production memmem wrapper/search with binary, guard-page and work gates.

The shared C cases also run via the actual imported libc in the Windows x86-64
and translated ARM64 native suite. This host gate compiles the real wrapper and
helper separately; only the second, work-counting copy is instrumented. Timing
uses the unmodified production source and a git-pinned baseline, in alternating
order. Host component CPU is not Windows or whole-application performance.
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
BASELINE = "751d82310f7c4c470c098ccebfd5d2d04699a0cd"
# Verbatim eight-line implementation from BASELINE. Keeping this frozen small
# control makes the normal CI gate independent of checkout history; explicit
# comparisons verify it against the requested Git object before measuring.
BASELINE_WRAPPER = """void* BIONIC_SYSV Memmem(const void* h, size_t hn, const void* n, size_t nn) {
  if (nn == 0) return const_cast<void*>(h);
  auto hp = static_cast<const uint8_t*>(h);
  for (size_t i = 0; i + nn <= hn; ++i) {
    if (std::memcmp(hp + i, n, nn) == 0) return const_cast<uint8_t*>(hp + i);
  }
  return nullptr;
}
"""


def wrapper(source: str) -> str:
    begin = "void* BIONIC_SYSV Memmem("
    end = "\n// FORTIFY ("
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("memmem wrapper boundaries changed")
    if source.count('ENTRY("memmem", Memmem);') != 1:
        raise ValueError("the production memmem export changed")
    return source[source.index(begin):source.index(end)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--compare-ref", help="verify the frozen baseline against this Git commit")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    header = (ROOT / "compat/bionic/memmem_search.h").read_text(encoding="utf-8")
    source = (ROOT / "compat/bionic/shim.cc").read_text(encoding="utf-8")
    old_wrapper = BASELINE_WRAPPER
    if args.compare_ref:
        baseline = subprocess.check_output(["git", "show", f"{args.compare_ref}:compat/bionic/shim.cc"],
                                           cwd=ROOT, text=True)
        if wrapper(baseline) != old_wrapper:
            raise ValueError("requested baseline differs from the frozen comparison implementation")
    observed = header.replace("#pragma once", "").replace("namespace gesher::bionic::search", "namespace observed")
    changes = {
        "std::memchr": "ObservedMemchr",
        "std::memcmp": "ObservedMemcmp",
        "*haystack == *needle": "(++compared, *haystack == *needle)",
        "const uint8_t a = needle[candidate + matched];":
            "++compared; const uint8_t a = needle[candidate + matched];",
        "needle[right] == haystack[right]": "(++compared, needle[right] == haystack[right])",
        "needle[left - 1] == haystack[left - 1]": "(++compared, needle[left - 1] == haystack[left - 1])",
    }
    for needle, replacement in changes.items():
        if needle not in observed:
            raise ValueError("work-count observation boundary changed: " + needle)
        observed = observed.replace(needle, replacement)
    template = (ROOT / "tests/native/memmem_host.cc.in").read_text(encoding="utf-8")
    template = template.replace("@BASELINE@", "void* Memmem(const void*, size_t, const void*, size_t);")
    template = template.replace("@COUNTED_BASELINE@", old_wrapper.replace("std::memcmp", "ObservedMemcmp"))
    template = template.replace("@OBSERVED@", observed)
    production = ('#include "memmem_search.h"\n#define BIONIC_SYSV\nnamespace production {\n'
                  'namespace search = gesher::bionic::search;\n' + wrapper(source) + "\n}\n")
    print(f"memmem header SHA256 {hashlib.sha256(header.encode()).hexdigest()}", flush=True)
    print(f"memmem wrapper SHA256 {hashlib.sha256(wrapper(source).encode()).hexdigest()}", flush=True)
    print(f"baseline wrapper SHA256 {hashlib.sha256(old_wrapper.encode()).hexdigest()}", flush=True)
    print(f"comparison ref {args.compare_ref or BASELINE} (Git verified: {bool(args.compare_ref)})", flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-memmem-") as temporary:
        folder = Path(temporary)
        (folder / "production.cc").write_text(production, encoding="utf-8")
        (folder / "baseline.cc").write_text(
            '#include <cstddef>\n#include <cstdint>\n#include <cstring>\n#define BIONIC_SYSV\n'
            'namespace baseline {\n' + old_wrapper + '\n}\n', encoding="utf-8")
        (folder / "contracts.cc").write_text(template, encoding="utf-8")
        compiler = shlex.split(args.cxx)
        flags = ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
                 "-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "tests/native")]
        subprocess.run(compiler + flags + ["-fstack-usage", "-Wframe-larger-than=256", "-c", "production.cc", "-o", "production.o"],
                       cwd=folder, check=True, timeout=120)
        usage = (folder / "production.su").read_text(encoding="utf-8")
        frames = [int(line.split("\t")[-2]) for line in usage.splitlines()]
        if not frames or max(frames) > 256:
            raise ValueError("production frame exceeds 256 bytes: " + usage)
        print(f"normal production maximum frame bytes {max(frames)}", flush=True)
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            subprocess.run(compiler + flags + ["-c", "production.cc", "-o", "production.o"],
                           cwd=folder, check=True, timeout=120)
        executable = folder / ("contracts.exe" if os.name == "nt" else "contracts")
        command = compiler + flags + ["contracts.cc", "baseline.cc", "production.o", "-o", str(executable)]
        if os.name == "nt" and not args.sanitize:
            command += ["-static"]
        subprocess.run(command, cwd=folder, check=True, timeout=120)
        subprocess.run([str(executable)] + (["measure"] if args.measure else []),
                       check=True, timeout=180)


if __name__ == "__main__":
    main()
