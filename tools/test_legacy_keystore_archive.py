#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Test the production keystore migration against actual Win32 files and AppContainer ACLs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FRAMEWORK = ROOT / "compat/framework"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="cl")
    args = parser.parse_args()
    # The real archive path contains two SHA-256 names. Keep the test prefix short.
    with tempfile.TemporaryDirectory(prefix="gka-") as directory:
        work = Path(directory)
        executable = work / "legacy_keystore_archive_test.exe"
        sources = [str(ROOT / "tests/native/legacy_keystore_archive_test.cc"),
                   str(FRAMEWORK / "legacy_keystore_archive.cc")]
        if Path(args.cxx).stem.lower() in {"cl", "clang-cl"}:
            command = [args.cxx, "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX", "/utf-8",
                       "/D_WIN32_WINNT=0x0A00", "/I" + str(FRAMEWORK), *sources,
                       "/Fe:" + str(executable), "/link", "advapi32.lib", "bcrypt.lib", "userenv.lib"]
        else:
            command = [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-municode",
                       "-D_WIN32_WINNT=0x0A00", "-static", "-I" + str(FRAMEWORK), *sources,
                       "-ladvapi32", "-lbcrypt", "-luserenv", "-o", str(executable)]
        subprocess.run(command, cwd=work, check=True, timeout=90)
        subprocess.run([str(executable), str(work / "sandbox")], cwd=work, check=True, timeout=60)
        # This second fixture calls the actual sealed-path parser. It makes no
        # claim to authenticate a peer or execute the elevated service RPC.
        parser_executable = work / "protected_legacy_archive_wire_test.exe"
        parser_source = str(ROOT / "tests/native/protected_legacy_archive_wire_test.cc")
        parser_command = [part.replace(str(executable), str(parser_executable))
                          .replace(sources[0], parser_source)
                          for part in command if part != sources[1]]
        subprocess.run(parser_command, cwd=work, check=True, timeout=90)
        subprocess.run([str(parser_executable)], cwd=work, check=True, timeout=30)



if __name__ == "__main__":
    main()
