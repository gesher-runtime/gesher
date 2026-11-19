#!/usr/bin/env python3
"""Exercise the real ARM64 TLS publication/attach/park lifecycle on Windows.

The native fixture copies production methods unchanged into an Env boundary
without a JIT. Optional --dll also runs real guest code and nested callbacks in
the built translator. --compile-only supports the Windows CI artifact path.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("ARM64 TLS source boundary changed")
    return source[source.index(begin):source.index(end, source.index(begin))]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dll", type=Path)
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    if os.name != "nt" and not args.compile_only:
        parser.error("this fixture exercises actual Windows virtual memory")
    path = "compat/arm64/dll/translator.cc"
    source = (subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / path).read_text(encoding="utf-8"))
    tls = section(source, "constexpr size_t kTcbSize =", "// --- the exclusive monitor")
    attach = section(source, "  void Attach() {", "  // The thread is exiting.")
    detach = section(source, "  void Detach() {", "  // Whether another thread may reuse")
    park = section(source, "  void Park() {", "  // One Jit per nesting depth:")
    lease = section(source, "struct EnvRelease {", "uint64_t EnterGuest(")
    template = (ROOT / "tests/native/arm64_tls_lifecycle_windows.cc.in").read_text(encoding="utf-8")
    template = template.replace("@TLS@", tls).replace("@ATTACH@", attach).replace("@DETACH@", detach)
    template = template.replace("@PARK@", park).replace("@LEASE@", lease)
    with tempfile.TemporaryDirectory(prefix="gesher-arm64-tls-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = args.output.resolve() if args.output else folder / "test.exe"
        executable.parent.mkdir(parents=True, exist_ok=True)
        cpp.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", "-static", "-pthread", "-I", str(ROOT / "compat/arm64/include"),
                                         "-I", str(ROOT / "compat/arm64/dll"),
                                         "-DHAS_TLS_ADDRESS=" + str(int("void* ApiTlsAddress(" in source)),
                                         str(cpp), "-o", str(executable), "-lpsapi"]
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            print(f"Compiled ARM64 TLS fixture: {executable}", flush=True)
        else:
            run = [str(executable), "measure" if args.measure else "gate"]
            if args.dll:
                run.append(str(args.dll.resolve()))
            subprocess.run(run, check=True, timeout=90)


if __name__ == "__main__":
    main()
