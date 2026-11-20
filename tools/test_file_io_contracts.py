#!/usr/bin/env python3
"""Exercise the exact regular-file adapter with real Linux files at Win32 calls.

NativeFileIo is production code. Its shared OS boundary is also used by the
positioned-I/O fixture. Linux append/write/read/dup provide real bytes and
kernel positions; only Windows CI proves WriteFile/NTFS's native semantics.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "compat/posix/src/fd.cc").read_text()
    first, last = "int TransferFileHandle(", "\nint CloseFdBody("
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("native file-I/O boundaries changed")
    implementation = source[source.index(first):source.index(last)]
    route_begin, route_end = "void ReleaseCanceledSnapshot(", "// The caller owns an exact native object reference"
    if source.count(route_begin) != 1 or source.count(route_end) != 1:
        raise ValueError("native pipe-route boundaries changed")
    pipe_route = source[source.index(route_begin):source.index(route_end)]
    snapshot_begin, snapshot_end = "int ReadFileDescription(", "\n}  // namespace gesher\n\nextern \"C\" {"
    if source.count(snapshot_begin) != 1 or source.count(snapshot_end) != 1:
        raise ValueError("snapshot file-I/O boundaries changed")
    snapshot_io = source[source.index(snapshot_begin):source.index(snapshot_end)]
    shared = (ROOT / "tests/native/positioned_io_host_test.cc.in").read_text()
    boundary_end = "#define pread tested_pread"
    if shared.count(boundary_end) != 1:
        raise ValueError("shared Windows I/O boundary changed")
    boundary = shared[:shared.index(boundary_end)]
    template = (ROOT / "tests/native/file_io_contract_host_test.cc.in").read_text()
    for marker, value in {"@WINDOWS_IO_BOUNDARY@": boundary, "@NATIVE_FILE_IO@": implementation,
                          "@NATIVE_PIPE_ROUTE@": pipe_route,
                          "@SNAPSHOT_FILE_IO@": snapshot_io}.items():
        if template.count(marker) != 1:
            raise ValueError("file-I/O fixture marker changed")
        template = template.replace(marker, value)
    print("Production NativeFileIo SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-file-io-") as temporary:
        folder = Path(temporary)
        cpp, executable = folder / "file_io.cc", folder / "file_io"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-D_GNU_SOURCE", "-pthread", str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        return subprocess.run([str(executable)], cwd=folder, env=environment, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
