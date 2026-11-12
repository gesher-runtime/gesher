#!/usr/bin/env python3
"""Run unchanged production rename logic with explicit Win32 metadata boundaries.

The separate filesystem_metadata_contract_test.inc fixture verifies real Windows
host and guest paths. This adapter proves errors precede destructive deletion.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile old production source as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    relative = "compat/posix/src/fsops.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + relative], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / relative).read_text())
    digest = hashlib.sha256(source.encode()).hexdigest()
    print(relative + " SHA256 " + digest, flush=True)
    start, end = "constexpr unsigned kRenameNoReplace", "// Owner's write permission"
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Production rename boundaries changed")
    production = source[source.index(start):source.index(end)]
    template = (ROOT / "tests/native/directory_rename_host_test.cc.in").read_text()
    if template.count("@PRODUCTION_RENAME@") != 1:
        raise ValueError("Missing production rename marker")
    with tempfile.TemporaryDirectory(prefix="gesher-directory-rename-") as temporary:
        work = Path(temporary)
        generated, binary = work / "contracts.cc", work / "contracts"
        generated.write_text(template.replace("@PRODUCTION_RENAME@", production))
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                          "-Wno-unused-function", str(generated), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
