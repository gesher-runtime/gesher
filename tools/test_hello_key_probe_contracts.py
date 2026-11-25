#!/usr/bin/env python3
"""Test the manual Hello probe's real Run/main against scripted native boundaries.

The fixture verifies exit statuses and operation/window lifetimes. It does not
exercise real Windows Hello, crypto, native prompt dismissal, or ART. CI also
compiles the complete probe against the real Windows SDK separately.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "tests/biometrics/hello_key_probe.cc"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--source-ref", help="run the same fixture against an earlier probe")
    parser.add_argument("--thread-sanitizer", action="store_true")
    args = parser.parse_args()
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + SOURCE],
                                     cwd=ROOT, text=True) if args.source_ref
              else (ROOT / SOURCE).read_text())
    print(SOURCE, "SHA256", hashlib.sha256(source.encode()).hexdigest(), flush=True)
    pieces = {
        "// PROBE_STATUS_NAME": source[source.index("static const char* StatusName("):
                                         source.index("static HWND MakeWindow(")],
        "// PROBE_RUN": source[source.index("static int Run("):
                                 source.index("static bool Sha256(")],
        "// PROBE_MAIN": source[source.index("int main("):].replace(
            "int main(", "static int ProbeMain(", 1),
    }
    fixture = (ROOT / "tests/biometrics/HelloKeyProbeContracts.cc.in").read_text()
    for marker, code in pieces.items():
        if fixture.count(marker) != 1:
            raise AssertionError("Probe insertion marker changed: " + marker)
        fixture = fixture.replace(marker, code)
    with tempfile.TemporaryDirectory(prefix="hello-key-probe-contracts-") as temporary:
        path = Path(temporary)
        (path / "probe.cc").write_text(fixture)
        sanitizer = "thread" if args.thread_sanitizer else "address,undefined"
        executable = path / "probe"
        subprocess.run([args.cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-pthread", "-fsanitize=" + sanitizer, "-fno-omit-frame-pointer",
                        str(path / "probe.cc"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
