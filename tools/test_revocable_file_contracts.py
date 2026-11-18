#!/usr/bin/env python3
"""Run exact revocation/OFD and regular I/O code over real Linux OS objects.

Named shared sections and robust process-shared mutexes model the declared
Win32 boundary. This verifies admission/drain, cross-process adoption, alias
lifetime, failure and resource ownership, not Windows ABI/NTFS/Binder execution.
The native revocable_file_test and binder_probe remain the Windows gates.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def no_includes(source):
    return re.sub(r"^#(?:include|pragma once)[^\n]*\n", "", source, flags=re.MULTILINE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--io-source-ref", help="Compile an exact prior NativeFileIo as a negative control")
    args = parser.parse_args()
    base = (ROOT / "tests/native/ofd_transfer_host_test.cc.in").read_text()
    boundary, rest = base.split("@OFD_DECLARATIONS@", 1)
    boundary = boundary.replace("@PE_EXPORTS_HEADER@", str(ROOT / "compat/elf/exec/pe_exports.h"))
    boundary = boundary.replace("CloseHandle(", "HostCloseHandle(")
    boundary = boundary.replace("CreateFileMappingW(", "HostCreateFileMappingW(")
    resolver = rest.split("@OFD_IMPLEMENTATION@", 1)[1].split("namespace gesher::exec {", 1)[0]
    declarations = "\n".join(no_includes((ROOT / name).read_text()) for name in (
        "compat/posix/include/gesher/file_description.h", "compat/posix/src/ofd_internal.h",
        "compat/posix/include/gesher/revocable_file.h", "compat/posix/src/revocable_internal.h"))
    implementations = []
    for name in ("compat/posix/src/ofd.cc", "compat/posix/src/revocable_file.cc"):
        source = (ROOT / name).read_text()
        print(name + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
        implementations.append(no_includes(source))
    fd = (subprocess.check_output(["git", "show", args.io_source_ref + ":compat/posix/src/fd.cc"],
                                  cwd=ROOT, text=True) if args.io_source_ref
          else (ROOT / "compat/posix/src/fd.cc").read_text())
    io = fd[fd.index("int TransferFileHandle("):fd.index("\nint CloseFdBody(")]
    print("NativeFileIo SHA256 " + hashlib.sha256(io.encode()).hexdigest(), flush=True)
    template = (ROOT / "tests/native/revocable_file_host_test.cc.in").read_text()
    for token, value in {"@OFD_BOUNDARY@": boundary, "@DECLARATIONS@": declarations,
                         "@OFD_IMPLEMENTATION@": implementations[0], "@RESOLVER@": resolver,
                         "@REVOCABLE_IMPLEMENTATION@": implementations[1], "@NATIVE_FILE_IO@": io}.items():
        assert template.count(token) == 1, token
        template = template.replace(token, value)
    with tempfile.TemporaryDirectory(prefix="gesher-revocable-") as temporary:
        folder = Path(temporary)
        cpp, executable = folder / "revocable.cc", folder / "revocable"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-unused-variable", "-Wno-mismatched-new-delete",
            "-fexceptions", "-D_GNU_SOURCE", "-pthread", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], cwd=folder, check=True, timeout=45)


if __name__ == "__main__":
    main()
