#!/usr/bin/env python3
"""Compile complete POSIX libdl routing with explicit Win32/ELF hook seams.

No PE code executes here. The valid input ELF is used only to observe dispatch;
the complete ELF loader and real guest call sites have separate integration gates.
Public host symbols are renamed so this fixture cannot interpose the Linux
process loader or sanitizer runtime.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--elf", required=True, type=Path, help="an original valid compiler-built ELF DSO")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    fixture = args.elf.resolve()
    print("BOUNDARY complete POSIX dlfcn.cc; explicit Win32 and ELF-hook seams; no PE execution", flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-posix-libdl-") as temporary:
        folder = Path(temporary)
        includes = folder / "include"
        (includes / "gesher").mkdir(parents=True)
        paths = ["compat/posix/src/dlfcn.cc", "compat/posix/include/dlfcn.h",
                 "compat/posix/include/gesher/builtin_library.h", "compat/posix/include/gesher/elf_dlopen.h",
                 "tests/native/posix_libdl_host_platform.h", "tests/native/posix_libdl_host.cc.in"]
        contents = {path: (ROOT / path).read_bytes() for path in paths}
        for path, body in contents.items():
            print("SHA256 " + path + " " + hashlib.sha256(body).hexdigest(), flush=True)
        (folder / "dlfcn.cc").write_bytes(contents[paths[0]])
        for path in paths[1:4]:
            relative = Path(path).relative_to("compat/posix/include")
            (includes / relative).write_bytes(contents[path])
        (includes / "windows.h").write_bytes(contents[paths[4]])
        (includes / "psapi.h").write_text('#include "windows.h"\n')
        (folder / "internal.h").write_text('#include "windows.h"\n')
        unit = folder / "driver.cc"
        unit.write_text(contents[paths[5]].decode().replace("@PRODUCTION_SOURCE@", "dlfcn.cc"))
        app = folder / "app"
        app.mkdir()
        guest = app / "libjavacore.so"
        shutil.copy2(fixture, guest)
        print("GUEST_SHA256 " + str(fixture) + " " + hashlib.sha256(guest.read_bytes()).hexdigest(), flush=True)
        executable = folder / "posix-libdl"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-pthread", "-I", str(includes)]
        command += ["-D" + name + "=gesher_fixture_" + name
                    for name in ("dlopen", "dlsym", "dlclose", "dlerror", "dladdr")]
        environment = os.environ.copy()
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                        "-fno-pie", "-no-pie"]
            previous = environment.get("ASAN_OPTIONS", "")
            environment["ASAN_OPTIONS"] = previous + (":" if previous else "") + "detect_leaks=0"
            print("SANITIZERS address,undefined; fatal findings; LSan disabled under ptrace", flush=True)
        command += [str(unit), "-o", str(executable)]
        print("COMPILE_COMMAND " + json.dumps(command), flush=True)
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable), str(guest)], check=True, timeout=30, env=environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
