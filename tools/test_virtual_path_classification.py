#!/usr/bin/env python3
"""Check the production virtual-path classifier and its allocation boundary.

The classifier, component iterator, USB device-number helpers and font suffix
validator are extracted unchanged. Filesystem opens are outside this fixture;
Binder existence and block attachment are controllable boundaries.
Font targets and their final host-path publication are adapters. Windows
uses the production UTF-8 conversion, while portable builds use a byte-widening
adapter and check normalization rather than Unicode conversion. --compare-ref
also runs the previous classifier on the entire ordinary generated corpus.
USB helper and attached-alias controls use the current production header;
--source-ref and --compare-ref select the classifier source only.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATH = "compat/posix/src/path.cc"
USB_BLOCK = "compat/posix/src/usb_block_internal.h"


def read(path: str, ref: str | None = None) -> str:
    if ref:
        return subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text(encoding="utf-8")


def classifier(source: str, namespace: str) -> str:
    begin = '// The parts of an absolute path, without empty and "." components.'
    end = "// /mnt/<drive>[/...]"
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("virtual-path classification boundary changed")
    font_begin = 'static int SystemFontPath('
    font_end = 'PathKind MapPath('
    if source.count(font_begin) != 1 or source.count(font_end) != 1:
        raise ValueError('font-path validation boundary changed')
    font = source[source.index(font_begin):source.index(font_end)]
    return f"namespace {namespace} {{\n{source[source.index(begin):source.index(end)]}\n{font}\n}}\n"


def block_helpers() -> str:
    source = read(USB_BLOCK)
    begin = 'inline constexpr const char* kBlockName = "sdb";'
    end = '\nbool Attached();'
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("USB device-number helper boundaries changed")
    section = source[source.index(begin):source.index(end)]
    if section.count("inline std::string NodeFromDeviceNumber(") != 1:
        raise ValueError("USB device-number helper missing")
    print("Production USB device-number helpers SHA256 " + hashlib.sha256(section.encode()).hexdigest(), flush=True)
    return section


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--compare-ref")
    parser.add_argument("--measure", action="store_true", help="report allocation counts without enforcing zero scratch")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    internal = read("compat/posix/src/internal.h")
    start = internal.index("enum class PathKind {")
    kinds = internal[start:internal.index("};", start) + 2]
    helpers = read("compat/posix/src/internal.cc")
    start = helpers.index("std::wstring Widen(")
    widen = helpers[start:helpers.index("std::string Narrow(", start)]
    source = read("tests/native/virtual_path_classification_host.cc.in")
    if source.count("@USB_BLOCK_HELPERS@") != 1:
        raise ValueError("USB device-number fixture boundary changed")
    source = source.replace("@USB_BLOCK_HELPERS@", block_helpers())
    source = source.replace("@PATH_KIND@", kinds).replace("@WIDEN@", widen)
    source = source.replace("@PRODUCTION@", classifier(read(PATH, args.source_ref), "production"))
    source = source.replace("@REFERENCE@", classifier(read(PATH, args.compare_ref), "reference") if args.compare_ref else "")
    source = source.replace("@HAS_REFERENCE@", "1" if args.compare_ref else "0")
    with tempfile.TemporaryDirectory(prefix="gesher-virtual-path-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = folder / ("test.exe" if os.name == "nt" else "test")
        cpp.write_text(source, encoding="utf-8")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", "-pthread", str(cpp), "-o", str(executable)]
        if os.name == "nt":
            command += ["-static"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            if os.name == "nt":
                for name in ("libclang_rt.asan_dynamic-x86_64.dll", "libc++.dll", "libunwind.dll"):
                    runtime = Path(command[0]).parent / name
                    if runtime.is_file():
                        shutil.copyfile(runtime, folder / name)
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable), "measure" if args.measure else "gate"], check=True, timeout=60)


if __name__ == "__main__":
    main()
