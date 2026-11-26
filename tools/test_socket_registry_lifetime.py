#!/usr/bin/env python3
"""Exercise production socket-table ownership and final-stdio-flush lifetime.

The host envelope substitutes only SocketFd's native payload. It does not
qualify Winsock I/O, Windows CRT locking, or native shutdown scheduling.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="Run the same fixture against exact historical fd.cc bytes")
    args = parser.parse_args()
    path = "compat/posix/src/fd.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT).decode()
              if args.source_ref else (ROOT / path).read_text())
    first, after = "namespace gesher {\nnamespace {\n\n", "// Stateless logical devices"
    if source.count(first) != 1 or source.count(after) != 1:
        raise ValueError("ambiguous socket registry boundary")
    registry = source[source.index(first) + len(first):source.index(after)]
    marker = "bool LookupSocket("
    if source.count(marker) != 1:
        raise ValueError("ambiguous socket lookup boundary")
    start = source.index(marker)
    lookup = source[start:source.index("\n}\n", start) + 3]
    template = (ROOT / "tests/native/socket_registry_lifetime_host.cc.in").read_text()
    for marker, body in (("@REGISTRY@", registry), ("@LOOKUP@", lookup)):
        if template.count(marker) != 1:
            raise ValueError("ambiguous fixture boundary: " + marker)
        template = template.replace(marker, body)
        print(marker.strip("@") + " SHA256 " + hashlib.sha256(body.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-socket-registry-") as temporary:
        folder = Path(temporary)
        generated, executable = folder / "test.cc", folder / "test"
        generated.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command + [str(generated), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=15)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
