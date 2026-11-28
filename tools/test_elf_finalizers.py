#!/usr/bin/env python3
"""Test production Close, group collection and retirement with an observing translator.

The fixture runs the loader's own close, finalizer, retirement and image
reclamation code over library records it publishes itself: reference counts,
dependency liveness, what keeps a library resident, and finalizer order.

Real x86-64 and translated ARM64 DT_FINI_ARRAY execution is additionally required
by the existing Windows ELF loader fixtures using tests/native/trivial.c.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/elf/elf_loader.cc")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    source = args.source.read_text()
    template = (ROOT / "tests/native/elf_finalizers_host_test.cc.in").read_text()
    for placeholder, start, end in [
        ("@LOADER_MUTEX_IMPLEMENTATION@", "class LoaderMutex {", "LoaderMutex g_mutex;"),
        ("@COLLECTION_IMPLEMENTATION@", "void RunFinalizers(Library* library) {",
         "Library* LibraryForAddressLocked("),
        ("@CLOSE_IMPLEMENTATION@", "bool Close(Library* library) {",
         "Library* RetainForAddress("),
        ("@LOOKUP_GLOBAL_LOCKED_IMPLEMENTATION@", "void* LookupGlobalLocked(const char* name,",
         "struct ResolvedSymbol {"),
        ("@LOOKUP_GLOBAL_IMPLEMENTATION@", "void* LookupGlobal(const char* name) {",
         "bool IsAarch64(Library* library)"),
    ]:
        if source.count(start) != 1 or source.count(end) != 1:
            raise ValueError("ELF finalizer/lookup source boundaries changed")
        template = template.replace(placeholder, source[source.index(start):source.index(end)])
    with tempfile.TemporaryDirectory(prefix="gesher-finalizers-") as temporary:
        work = Path(temporary)
        cpp, executable = work / "test.cc", work / "test"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra",
                   "-Werror", "-pthread", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
        subprocess.run(command, check=True, timeout=90)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == "__main__":
    main()
