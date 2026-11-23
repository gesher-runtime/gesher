#!/usr/bin/env python3
"""Run complete production log cache/assembly against declared host or real Windows boundaries.

Windows also compiles actual system_properties.cc and its environment owner,
tests mapped generations and area switches across processes, and observes real
file operations. --source-ref
compiles the exact baseline sources/headers with this same fixture. Its raw
timings are observations, not a noisy speed-ratio gate. Host mode cannot measure
Windows file I/O, process memory or the mapped-area implementation.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--measure-only", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    compiler = shlex.split(args.cxx)
    windows = os.name == "nt" or "mingw" in compiler[0] or bool(args.output and args.output.suffix.lower() == ".exe")

    def read(path: str) -> str:
        if args.source_ref:
            content = subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True)
        else:
            content = (ROOT / path).read_text(encoding="utf-8")
        print(f"SOURCE {path} sha256={hashlib.sha256(content.encode()).hexdigest()}", flush=True)
        return content

    def region(path: str, start: str, end: str) -> str:
        # Copy complete native helper bodies, without a behavioral replacement.
        # Fail closed if the production boundaries move or become ambiguous.
        content = read(path)
        assert content.count(start) == content.count(end) == 1, path
        begin, finish = content.index(start), content.index(end)
        assert begin < finish, path
        result = content[begin:finish]
        print(f"REGION {path} sha256={hashlib.sha256(result.encode()).hexdigest()}", flush=True)
        return result

    with tempfile.TemporaryDirectory(prefix="gesher-structured-logging-") as temporary:
        work = Path(temporary)
        (work / "include/gesher").mkdir(parents=True)
        for name in ("logging", "system_properties"):
            for source, destination in ((f"compat/posix/src/{name}.cc", work / f"{name}.cc"),
                                        (f"compat/posix/include/gesher/{name}.h", work / f"include/gesher/{name}.h")):
                content = read(source)
                destination.write_text(content, encoding="utf-8")
        # #118 routes property-session publication through the shared POSIX
        # owner. Stage that real dependency only when the selected source uses
        # it, retaining historical --source-ref builds without a new shim.
        environment_owner = windows and "#include <gesher/environment.h>" in (work / "system_properties.cc").read_text()
        environment_sources = []
        if environment_owner:
            for name in ("environment", "font_environment"):
                source = work / f"{name}.cc"
                source.write_text(read(f"compat/posix/src/{name}.cc"), encoding="utf-8")
                environment_sources.append(source)
                (work / f"include/gesher/{name}.h").write_text(
                    read(f"compat/posix/include/gesher/{name}.h"), encoding="utf-8")
            (work / "environment_internal.h").write_text(read("compat/posix/src/environment_internal.h"), encoding="utf-8")
            (work / "include/errno.h").write_text(read("compat/posix/include/errno.h"), encoding="utf-8")
            # The enclosing unistd/errno units also implement unrelated runtime
            # services. Extract their complete dependency bodies from the same
            # source revision; UCRT calls, ownership and error mapping stay real.
            native = "#include <windows.h>\n#include <cerrno>\n#include <cstdlib>\n#include <cstring>\n"
            native += "#include <gesher/environment.h>\n#include <gesher/font_environment.h>\nnamespace {\n"
            native += region("compat/posix/src/unistd.cc", "bool KeepEnvironmentString(", '\nextern "C" {\n')
            native += region("compat/posix/src/errno.cc", 'extern "C" int gesher_errno_from_crt(', '\nextern "C" int gesher_rmdir(')
            source = work / "environment_native.cc"
            source.write_text(native, encoding="utf-8")
            environment_sources.append(source)
        revised = "struct Revision" in (work / "include/gesher/system_properties.h").read_text()
        flags = ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
                 "-I", str(work / "include"), f"-DLOGGING_HAS_REVISION={int(revised)}",
                 f"-DLOGGING_HAS_ENVIRONMENT_OWNER={int(environment_owner)}"]
        if windows:
            flags += ["-D_WIN32_WINNT=0x0A00", "-static"]
        else:
            flags += ["-I", str(ROOT / "tests/logging/host")]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        objects = []
        sources = [work / "logging.cc", ROOT / "tests/logging/contracts.cc"]
        sources.append(work / "system_properties.cc" if windows else ROOT / "tests/logging/host_boundary.cc")
        sources += environment_sources
        for i, source in enumerate(sources):
            obj = work / f"source{i}.o"
            extra = ["-include", str(ROOT / "tests/logging/windows_calls.h")] if windows and source in (work / "logging.cc", work / "system_properties.cc") else []
            if source == work / "environment.cc":
                extra += ["-fexceptions"]  # Same per-source override as posix.cmake.
            subprocess.run(compiler + flags + extra + ["-c", str(source), "-o", str(obj)], check=True, timeout=120)
            objects.append(str(obj))
        output = args.output.resolve() if args.output else work / ("test.exe" if windows else "test")
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(compiler + flags + objects + (["-lpsapi"] if windows else []) + ["-o", str(output)], check=True, timeout=120)
        if args.compile_only:
            print(f"Compiled real Windows structured logger contracts: {output}", flush=True)
            return 0
        environment = os.environ.copy()
        if os.name == "nt" and Path(compiler[0]).is_absolute():
            environment["PATH"] = str(Path(compiler[0]).parent) + os.pathsep + environment.get("PATH", "")
        return subprocess.run([str(output)] + (["--measure-only"] if args.measure_only else []),
                              env=environment, timeout=180).returncode


if __name__ == "__main__":
    raise SystemExit(main())
