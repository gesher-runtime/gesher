#!/usr/bin/env python3
"""Exercise production reader GPU handoff with explicit host GPU/JNI boundaries.

The production release, upload and Image.getHardwareBuffer methods are compiled
unchanged. NativeFence, D3D device/query, GPU-thread dispatch and JNI objects are
controlled adapters for failure and interleaving injection. This checks ordering
and error propagation, not real Windows/ANGLE timing or device behavior.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from test_media_surface_contracts import method

ROOT = Path(__file__).resolve().parents[1]
BUFFER = "compat/bionic/hardware_buffer.cc"
GPU = "compat/bionic/gpu_interop.cc"
READER = "compat/devices/camera_frames.cc"


def source(path: str, ref: str | None) -> str:
    if ref:
        return subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT,
                              capture_output=True, text=True, check=True).stdout
    return (ROOT / path).read_text(encoding="utf-8")


def generate(ref: str | None) -> tuple[str, dict]:
    texts = {path: source(path, ref) for path in (BUFFER, GPU, READER)}
    buffer, gpu, reader = (texts[path] for path in (BUFFER, GPU, READER))
    checked = "bool FlushRgbaToGpu(" in buffer
    upload_result = "bool UploadRect(" in buffer
    update_result = "bool UpdateTexture(" in gpu
    replacements = {
        "EMPTY": method(buffer, "bool Empty("),
        "UPLOAD": method(buffer, ("bool" if upload_result else "void") + " UploadRect("),
        "MARK": method(buffer, "void MarkRgbaWritten("),
        "RELEASE": method(buffer, "void NoteRgbaAppReleased("),
        "FLUSH": method(buffer, ("bool" if checked else "void") + " FlushRgbaToGpu("),
        "FINISH": method(gpu, "void Thread::Finish("),
        "UPDATE": method(gpu, ("bool" if update_result else "void") + " UpdateTexture("),
        "GET_BUFFER": method(reader, "jobject Image_getHardwareBuffer("),
    }
    fixture = (ROOT / "tests/media/reader_gpu_sync_host.cc.in").read_text(encoding="utf-8")
    for name, body in replacements.items():
        marker = "@" + name + "@"
        if fixture.count(marker) != 1:
            raise ValueError("fixture marker changed: " + marker)
        fixture = fixture.replace(marker, body)
    hashes = {path: hashlib.sha256(text.encode()).hexdigest() for path, text in texts.items()}
    return f"#define CHECKED_FLUSH {int(checked)}\n" + fixture, hashes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile another revision's actual methods as a negative control")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    fixture, hashes = generate(args.source_ref)
    print("reader GPU handoff production hashes " + json.dumps(hashes), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-reader-gpu-sync-") as temporary:
        work = Path(temporary)
        cpp = work / "reader_gpu_sync.cc"
        cpp.write_text(fixture, encoding="utf-8")
        binary = work / ("reader_gpu_sync.exe" if os.name == "nt" else "reader_gpu_sync")
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                        "-fno-exceptions", "-pthread", str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        runtime_env = os.environ.copy()
        compiler = Path(command[0])
        if os.name == "nt" and compiler.is_absolute():
            runtime_env["PATH"] = str(compiler.parent) + os.pathsep + runtime_env.get("PATH", "")
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                timeout=60, env=runtime_env)
    print(result.stdout, end="", flush=True)
    print(result.stderr, end="", flush=True)
    if args.json:
        args.json.write_text(json.dumps({
            "scope": __doc__.strip(), "source_ref": args.source_ref,
            "source_sha256": hashes, "sanitized": args.sanitize,
            "asan_options": os.environ.get("ASAN_OPTIONS", ""),
            "stdout": result.stdout, "stderr": result.stderr, "returncode": result.returncode,
        }, indent=2) + "\n", encoding="utf-8")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
