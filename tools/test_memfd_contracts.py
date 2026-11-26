#!/usr/bin/env python3
"""Run exact memfd storage and both dispatch functions over a Linux Win32 model.

The backend uses real files, descriptors, mappings and memory-map permissions.
It cannot prove Windows delete-on-close or executable mapping behavior; the
unchanged Os fixture and shared Android ELF tests remain mandatory.

A second fixture compiles the whole of memfd_seals.cc over a separate model of
the Win32 calls that decide F_ADD_SEALS(F_SEAL_WRITE): LockFileEx locks (real
OFD locks of private open descriptions, so they conflict, stack and vanish with
a process as on Windows), DuplicateHandle, and the GESHER.SEALS extended
attribute (storage shared by every process of the test). It exercises the
writable-mapping lease: EBUSY while a writable shared mapping lives, release
on unmap/partial unmap/failed map/process death, and the serialization of seal
additions against each other and against writable mmap, across threads and
across forked processes. It is not a Windows runtime: NTFS EAs, real LockFileEx
ownership and VirtualAlloc2 mappings are covered only by the Windows CI jobs.
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


def lease_template(seals):
    """Whole production memfd_seals.cc, minus the headers the model replaces."""
    for header in ('#include <io.h>\n', '#include "internal.h"\n', '#include "record_lock_liveness.h"\n'):
        if seals.count(header) != 1:
            raise ValueError("memfd_seals.cc include boundary changed: " + header.strip())
        seals = seals.replace(header, "")
    template = (ROOT / "tests/native/memfd_lease_host_test.cc.in").read_text()
    if template.count("@MEMFD_SEALS@") != 1:
        raise ValueError("template boundary changed: @MEMFD_SEALS@")
    return template.replace("@MEMFD_SEALS@", seals)


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
    seals = source("compat/posix/src/memfd_seals.cc")
    template = (ROOT / "tests/native/memfd_host_test.cc.in").read_text()
    for marker, value in (("@MEMFD_BACKEND@", backend), ("@POSIX_DISPATCH@", dispatcher),
                          ("@BIONIC_DISPATCH@", guest)):
        if template.count(marker) != 1:
            raise ValueError("template boundary changed: " + marker)
        template = template.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix="gesher-memfd-") as temporary:
        directory = Path(temporary)
        # The guest's mapping-lease assertions run unchanged against the Linux
        # kernel as well as in the Windows ELF jobs. The model below cannot
        # establish native byte-lock or mapping behavior by itself.
        reference = directory / "memfd-seals-reference"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            str(ROOT / "tests/native/memfd_seals_reference.c"), "-o", str(reference), "-ldl"
        ], check=True, timeout=120)
        subprocess.run([str(reference)], check=True, timeout=30)
        cpp, executable = directory / "memfd.cc", directory / "memfd"
        cpp.write_text(template)
        flags = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-D_GNU_SOURCE", "-pthread",
            "-I", str(ROOT / "tests/native")]
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        command = flags + [str(cpp), "-o", str(executable)] + sanitizer
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        lease_cpp, lease_executable = directory / "lease.cc", directory / "lease"
        lease_cpp.write_text(lease_template(seals))
        lease_command = flags + [str(lease_cpp), "-o", str(lease_executable)] + sanitizer
        subprocess.run(lease_command, check=True, timeout=120)
        failures = []
        cases = [(name, executable) for name in ("direct", "posix", "guest", "faults")]
        cases += [(name, lease_executable) for name in ("basics", "lifetime", "races")]
        for case, binary in cases:
            result = subprocess.run([str(binary), case], cwd=directory, env=environment,
                                    text=True, capture_output=True, timeout=120)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failures.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
