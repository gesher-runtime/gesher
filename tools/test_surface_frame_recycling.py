#!/usr/bin/env python3
"""Measure complete QueueSurface publication and immutable frame ownership.

The existing geometry fixture supplies only Android dispatch/refcount and clock
adapters. QueueSurface and FrameImages are complete, unchanged production source;
SkData/SkImage adapt the retained-pixel boundary without running Skia or a GPU.
Allocation headers count requested C++ bytes, not allocator overhead or process RSS.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def generate(ref: str | None) -> str:
    def read(path: str) -> str:
        if ref:
            return subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    originals = [read("compat/media/surface/queue_surface." + suffix) for suffix in ("h", "cc")]
    adapted = [re.sub(r"^\s*#(?:include|pragma).*\n", "", source, flags=re.MULTILINE)
               .replace("protected:", "public:") for source in originals]
    constants = set(re.findall(r"\bNATIVE_WINDOW_[A-Z0-9_]+\b", "\n".join(originals)))
    constants.discard("NATIVE_WINDOW_TIMESTAMP_AUTO")
    adapters = (ROOT / "tests/media/surface_geometry_host.cc.in").read_text().split("@PRODUCTION@")[0]
    adapters = adapters.replace("@CONSTANTS@", "enum { " + ", ".join(sorted(constants)) + " };")
    image_source = read("compat/media/surface/surface_jni.cc")
    cache = image_source[image_source.index("class FrameImages {"):
                         image_source.index("// --- SurfaceTexture")]
    fixture = (ROOT / "tests/media/surface_frame_recycling_host.cc.in").read_text()
    fixture = fixture.replace("@ACCOUNTING@", (ROOT / "tests/media/surface_frame_accounting.h").read_text())
    fixture = fixture.replace("@ADAPTERS@", adapters).replace("@PRODUCTION@", "\n".join(adapted))
    fixture = fixture.replace("@CACHE@", cache)
    print("surface frame source SHA256 " + hashlib.sha256(("\n".join(originals) + cache).encode()).hexdigest(),
          flush=True)
    return fixture


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Use an older implementation with the same assertions")
    parser.add_argument("--measure-only", action="store_true", help="Report allocations without new budgets")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gesher-surface-frame-") as temporary:
        work = Path(temporary)
        cpp = work / "frames.cc"
        cpp.write_text(generate(args.source_ref))
        binary = work / ("frames.exe" if os.name == "nt" else "frames")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary), "measure" if args.measure_only else "gate"],
                              check=False, timeout=120).returncode


if __name__ == "__main__":
    raise SystemExit(main())
