#!/usr/bin/env python3
"""Run exact memfd storage and both dispatch functions over a Linux Win32 model.

The backend uses real files, descriptors, mappings and memory-map permissions.
It cannot prove Windows delete-on-close or executable mapping behavior; the
unchanged Os fixture and shared Android ELF tests remain mandatory.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def extract(source, begin, end):
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("production source boundaries changed")
    return source[source.index(begin):source.index(end, source.index(begin))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile exact prior source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    def source(path):
        text = (subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                       cwd=ROOT, text=True) if args.source_ref
                else (ROOT / path).read_text())
        print(path + " SHA256 " + hashlib.sha256(text.encode()).hexdigest(), flush=True)
        return text
    backend = extract(source("compat/posix/src/system.cc"), "int memfd_create(",
                      "\n// --- strsignal,")
    dispatcher = source("compat/posix/src/syscall.cc")
    marker = 'extern "C" long syscall('
    if dispatcher.count(marker) != 1:
        raise ValueError("native dispatcher boundary changed")
    dispatcher = dispatcher[dispatcher.index(marker):]
    guest = extract(source("compat/bionic/shim.cc"), "int64_t BIONIC_SYSV Syscall(",
                    "\nvoid ReportUnresolved(")
    template = (ROOT / "tests/native/memfd_host_test.cc.in").read_text()
    for marker, value in (("@MEMFD_BACKEND@", backend), ("@POSIX_DISPATCH@", dispatcher),
                          ("@BIONIC_DISPATCH@", guest)):
        if template.count(marker) != 1:
            raise ValueError("template boundary changed: " + marker)
        template = template.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix="gesher-memfd-") as temporary:
        directory = Path(temporary)
        cpp, executable = directory / "memfd.cc", directory / "memfd"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-D_GNU_SOURCE", "-pthread",
            "-I", str(ROOT / "tests/native"), str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        failures = []
        for case in ("direct", "posix", "guest", "faults"):
            result = subprocess.run([str(executable), case], cwd=directory, env=environment,
                                    text=True, capture_output=True, timeout=45)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
