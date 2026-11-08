#!/usr/bin/env python3
"""Execute the production Windows positioned-I/O adapter with Linux file I/O.

The boundary reproduces synchronous Win32 cursor updates and exercises errors,
pending completions, actual descriptor reuse and ordinary concurrent I/O. Only
the Windows native/ART regressions prove ReOpenFile/NTFS behavior.
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
    parser.add_argument("--source", type=Path, default=ROOT / "compat/posix/src/unistd.cc")
    parser.add_argument("--source-ref", help="Compile the exact git revision as a negative control")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (subprocess.check_output(["git", "show", args.source_ref + ":compat/posix/src/unistd.cc"],
                                     cwd=ROOT, text=True) if args.source_ref else args.source.read_text())
    first, last = "int fdatasync(int fd) { return fsync(fd); }", "\nint nice("
    if source.count(first) != 1 or source.count(last) != 1:
        raise ValueError("positioned-I/O source boundaries changed")
    section = source[source.index(first) + len(first):source.index(last)]
    template = (ROOT / "tests/native/positioned_io_host_test.cc.in").read_text()
    if template.count("@POSITIONED_IO_IMPLEMENTATION@") != 1:
        raise ValueError("positioned-I/O template boundary changed")
    print("Production unistd.cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-positioned-io-") as temporary:
        folder = Path(temporary)
        generated = folder / "positioned.cc"
        generated.write_text(template.replace("@POSITIONED_IO_IMPLEMENTATION@", section))
        executable = folder / "positioned"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-fno-exceptions", "-D_GNU_SOURCE", "-pthread",
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        failed = []
        for case in ("public", "faults", "concurrent", "null-faults"):
            result = subprocess.run([str(executable), case], cwd=folder, env=environment,
                                    capture_output=True, text=True, timeout=45)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed.append(case)
                print(f"FAIL {case}: exit={result.returncode}\n{result.stderr[:6000]}", flush=True)
            else:
                print("PASS " + case, flush=True)
        return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
