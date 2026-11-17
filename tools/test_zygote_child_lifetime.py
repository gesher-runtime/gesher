#!/usr/bin/env python3
"""Run exact production child-handle identity/termination with recording Win32 boundaries."""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "compat/framework/services_zygote.cc").read_text()
    start, end = "int64_t ChildStartTime(int pid)", "bool Kill(int pid)"
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("Production child lifetime boundaries changed")
    production = source[source.index(start):source.index(end)]
    print("services_zygote.cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    template = (ROOT / "tests/native/zygote_child_lifetime_host_test.cc.in").read_text()
    if template.count("@PRODUCTION_CHILD_LIFETIME@") != 1:
        raise ValueError("Missing production child lifetime marker")
    with tempfile.TemporaryDirectory(prefix="gesher-child-lifetime-") as directory:
        work = Path(directory); generated = work / "contracts.cc"; binary = work / "contracts"
        generated.write_text(template.replace("@PRODUCTION_CHILD_LIFETIME@", production))
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(generated), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
