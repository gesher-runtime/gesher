#!/usr/bin/env python3
"""Run real layered-window presentation/cache code with counted GDI calls.

Windows uses real memory DCs/DIBs and verifies their handles/pixels/lifetimes.
Other hosts declare the GDI boundary. UpdateLayeredWindow itself is an adapter,
so this does not measure the desktop compositor, monitor hardware or frame rate.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compare older presentation code using the same GDI boundary")
    parser.add_argument("--measure-only", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--compile-only", action="store_true",
                        help="Build without running, for a Windows cross-compiled CI artifact")
    parser.add_argument("--output", type=Path, help="Keep the compiled fixture at this path")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    path = "compat/hwui/window/gesher_popup.cc"
    source = ((ROOT / path).read_text() if not args.source_ref else
              subprocess.run(["git", "show", args.source_ref + ":" + path], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout)
    start, end = "void Window::PresentLayered() {", "bool Window::HandlePopupMessage("
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("layered presentation extraction boundary changed")
    presentation = source[source.index(start):source.index(end)]
    # The producer post and dimming methods participate in dirty coalescing.
    # Keep their real state transitions in the fixture, not a mirrored model.
    window_path = "compat/hwui/window/gesher_window.cc"
    window = ((ROOT / window_path).read_text() if not args.source_ref else
              subprocess.run(["git", "show", args.source_ref + ":" + window_path], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout)
    post = window[window.index("void Window::UnlockAndPost("):window.index("int Window::width()")]
    dim = source[source.index("void Window::SetDim("):source.index("void Window::ScreenBounds(")]
    post_declaration = post[:post.index("{")].replace("Window::", "").strip() + ";"
    dim_signature = dim[dim.index("const uint8_t* Window::DimmedPixels("):]
    dim_declaration = dim_signature[:dim_signature.index("{")].replace("Window::", "").strip()
    dim_declaration = dim_declaration.replace("const PixelRect* dirty", "const PixelRect* dirty = nullptr") + ";"
    # The baseline does not use the new helper. Keep the helper's direct lifetime
    # tests identical while comparing old/new production presentation functions.
    header = re.sub(r"^\s*#(?:include|pragma).*\n", "",
                    (ROOT / "compat/hwui/window/layered_bitmap.h").read_text(), flags=re.MULTILINE)
    damage = re.sub(r"^\s*#(?:include|pragma).*\n", "",
                    (ROOT / "compat/hwui/window/surface_damage.h").read_text(), flags=re.MULTILINE)
    header = damage + header
    fixture = (ROOT / "tests/media/layered_bitmap_cache_host.cc.in").read_text()
    fixture = fixture.replace("@POST_DECLARATION@", post_declaration).replace("@DIM_DECLARATION@", dim_declaration)
    fixture = fixture.replace("@POST_ARGUMENT@", "dirty" if "PixelRect" in post_declaration else "")
    fixture = fixture.replace("@CACHE@", header).replace("@PRESENTATION@", "namespace gesher {\n" + post + dim + presentation + "\n}")
    print("layered bitmap source SHA256 " + hashlib.sha256((header + post + dim + presentation).encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-layered-cache-") as temporary:
        work = Path(temporary)
        cpp = work / "layered.cc"
        cpp.write_text(fixture)
        binary = work / ("layered.exe" if os.name == "nt" else "layered")
        if args.output is not None:
            binary = args.output.resolve()
            binary.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra",
                                         "-Werror", "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary)]
        if os.name == "nt" or binary.suffix.lower() == ".exe" or "mingw" in command[0]:
            command += ["-lgdi32", "-luser32"]
            if args.compile_only:
                command += ["-static"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        runtime_env = os.environ.copy()
        compiler = Path(command[0])
        if os.name == "nt" and compiler.is_absolute():
            runtime_env["PATH"] = str(compiler.parent) + os.pathsep + runtime_env.get("PATH", "")
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            print(f"Compiled layered bitmap fixture: {binary}", flush=True)
            return 0
        result = subprocess.run([str(binary), "measure" if args.measure_only else "gate"],
                                check=False, timeout=120, env=runtime_env)
        if result.returncode:
            print(f"Layered bitmap fixture failed with exit code {result.returncode}", flush=True)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
