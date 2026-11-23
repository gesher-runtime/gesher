#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production dirty-rect conversion and normal/layered presentation contracts.

Compiles complete Win32Surface, LayeredBitmap, Window::PresentLayered, Window::PresentPixels
and PostWindowDirty production code against declared Win32/GDI boundaries and an independent
full-frame pixel oracle. Supports --source-ref and --measure-only for deterministic ABBA
comparisons against pre-GFX-04 baselines (e.g. 5aa6b390).
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


def section(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError(f"extraction boundary changed: {start!r} .. {end!r}")
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def generate(ref: str | None) -> str:
    def read(path: str) -> str:
        if ref:
            return subprocess.run(
                ["git", "show", f"{ref}:{path}"],
                cwd=ROOT, check=True, capture_output=True, text=True,
            ).stdout
        return (ROOT / path).read_text(encoding="utf-8")

    surface_h = read("compat/hwui/window/win32_surface.h")
    surface_cc = read("compat/hwui/window/win32_surface.cc")
    layered_h = read("compat/hwui/window/layered_bitmap.h")
    popup_cc = read("compat/hwui/window/gesher_popup.cc")

    strip = lambda text: re.sub(r"^\s*#(?:include|pragma).*\n", "", text, flags=re.MULTILINE)
    adapted_surface_h = strip(surface_h).replace("protected:", "public:")
    adapted_surface_cc = strip(surface_cc)
    adapted_layered_h = strip(layered_h)

    present_pixels = section(popup_cc, "void Window::PresentPixels(", "void Window::PresentLayered(")
    present_layered = section(popup_cc, "void Window::PresentLayered() {", "bool Window::HandlePopupMessage(")

    post_dirty = ""
    has_dirty = "0"
    if "void PostWindowDirty(" in popup_cc:
        start = popup_cc.index("void PostWindowDirty(")
        end = popup_cc.rindex("}  // namespace gesher")
        post_dirty = popup_cc[start:end]
        has_dirty = "1"

    constants = set(re.findall(r"\bNATIVE_WINDOW_[A-Z0-9_]+\b", surface_h + "\n" + surface_cc))
    constants.discard("NATIVE_WINDOW_TIMESTAMP_AUTO")
    enum_decl = "enum { " + ", ".join(sorted(constants)) + " };\n"

    digest = hashlib.sha256(
        (surface_h + surface_cc + layered_h + present_pixels + present_layered + post_dirty).encode()
    ).hexdigest()
    print(f"surface dirty-present source SHA256 {digest}", flush=True)

    template = (ROOT / "tests/media/surface_dirty_present_host.cc.in").read_text(encoding="utf-8")
    return (
        template.replace("@HAS_DIRTY@", has_dirty)
        .replace("@CONSTANTS@", enum_decl)
        .replace("@LAYERED_HEADER@", adapted_layered_h)
        .replace("@SURFACE_HEADER@", adapted_surface_h)
        .replace("@SURFACE_CC@", adapted_surface_cc)
        .replace("@POPUP_METHODS@", "namespace gesher {\n" + present_pixels + present_layered + post_dirty + "\n}\n")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile older production source against the measurement harness")
    parser.add_argument("--measure-only", action="store_true", help="Run 1080p full vs dirty-rect benchmark")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--output", type=Path, help="Keep compiled binary at this path")
    parser.add_argument("--compile-only", action="store_true", help="Compile without executing")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")

    source = generate(args.source_ref)
    with tempfile.TemporaryDirectory(prefix="gesher-surface-dirty-") as temporary:
        work = Path(temporary)
        cpp = work / "dirty_present.cc"
        cpp.write_text(source, encoding="utf-8")
        binary = work / ("dirty_present.exe" if os.name == "nt" else "dirty_present")
        if args.output is not None:
            binary = args.output.resolve()
            binary.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            return 0
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        mode = "measure" if args.measure_only else "gate"
        return subprocess.run([str(binary), mode], env=env, check=False, timeout=120).returncode


if __name__ == "__main__":
    raise SystemExit(main())
