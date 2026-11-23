#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production EGL child-surface Win32 display refresh query and swap pacing.

Extracts QueryRefreshIntervalUs and the ChildSurfaceBeforeSwap pacing block directly
from compat/bionic/egl_child_surface.cc and compiles them against declared Win32
MonitorFromWindow / GetMonitorInfoW / EnumDisplaySettingsW boundaries.
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


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f"EGL child refresh extraction boundary changed: {start!r} .. {end!r}")
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    source = (ROOT / "compat/bionic/egl_child_surface.cc").read_text(encoding="utf-8")
    query_fn = section(
        source,
        "constexpr int64_t kDefaultRefreshHz = 60;",
        "// Waits until `t` on a high-resolution waitable timer.",
    )
    pacing_body = section(
        source,
        "  static gesher::perf::Site pace_site(",
        "  // The frame is posted.",
    )

    digest = hashlib.sha256((query_fn + pacing_body).encode()).hexdigest()
    print(f"EGL child refresh source SHA256 {digest}", flush=True)

    template = (ROOT / "tests/native/egl_child_refresh_host.cc.in").read_text(encoding="utf-8")
    code = template.replace("@QUERY_REFRESH@", query_fn).replace("@PACING_BODY@", pacing_body)

    with tempfile.TemporaryDirectory(prefix="gesher-egl-refresh-") as temporary:
        work = Path(temporary)
        cpp = work / "refresh_test.cc"
        binary = work / ("refresh_test.exe" if os.name == "nt" else "refresh_test")
        cpp.write_text(code, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        return subprocess.run([str(binary)], env=env, check=False, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
