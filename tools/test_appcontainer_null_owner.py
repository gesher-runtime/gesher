#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Exercise the exact POSIX startup NUL owner with Win32 handle operations."""
import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="cl")
    args = parser.parse_args()
    posix = ROOT / "compat/posix/src"
    source = (posix / "fd.cc").read_text(encoding="utf-8")
    owners = re.findall(r"(?ms)^class AppContainerNull \{.*?^\};", source)
    if len(owners) != 1:
        raise SystemExit("Cannot identify the exact production NUL owner")
    template = (ROOT / "tests/native/appcontainer_null_owner_test.cc.in").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="gesher-appcontainer-null-") as directory:
        work = pathlib.Path(directory)
        path = work / "appcontainer_null_owner_test.cc"
        path.write_text(template.replace("@PRODUCTION_OWNER@", owners[0]), encoding="utf-8")
        executable = work / "appcontainer_null_owner_test.exe"
        if pathlib.Path(args.cxx).stem.lower() in {"cl", "clang-cl"}:
            compiler = [args.cxx, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                        "/D_WIN32_WINNT=0x0A00", "/I" + str(posix), str(path),
                        "/Fe:" + str(executable), "/link", "advapi32.lib"]
        else:
            compiler = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-D_WIN32_WINNT=0x0A00", "-static", "-I" + str(posix),
                        str(path), "-ladvapi32", "-o", str(executable)]
        subprocess.run(compiler, cwd=work, check=True, timeout=60)
        subprocess.run([str(executable)], cwd=work, check=True, timeout=30)


if __name__ == "__main__":
    main()
