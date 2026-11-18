#!/usr/bin/env python3
"""Check generated provider metadata against pinned EGL/GLES export contracts.

This is a generator/ownership gate, not GL execution or a backend capability
test. Inputs are the actual headers, maps and implementation lists fetched by
aosp.lock; the Windows build separately compiles the generated real thunks.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "eglGetDisplay": 1, "eglGetProcAddress": 1,
    "glClear": 14, "glGetString": 14, "glMatrixMode": 2,
    "glCreateShader": 12, "glBindVertexArray": 12,
    # Actual GLES2/gl2ext_api.in definitions, absent from the SDK maps.
    "glBeginQueryEXT": 12, "glTexStorage2DEXT": 12,
    # Actual public eglApi.cpp definitions, absent from the SDK map.
    "eglGetNextFrameIdANDROID": 1, "eglGetCompositorTimingANDROID": 1,
    "eglGetCompositorTimingSupportedANDROID": 1,
    "eglGetFrameTimestampsANDROID": 1, "eglGetFrameTimestampSupportedANDROID": 1,
    # Header declarations/proc-address additions alone confer no DSO owner.
    "glBindUniformLocationCHROMIUM": 0, "eglGetPlatformDisplayEXT": 0,
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--include-dir", type=Path, required=True,
                        help="pinned frameworks/native/opengl/include directory")
    args = parser.parse_args()
    include = args.include_dir.resolve()
    generator = ROOT / "compat/bionic/gen_gl_thunks.py"
    spec = importlib.util.spec_from_file_location("gl_generator", generator)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    owners = module.provider_owners(str(include))
    for name, mask in CASES.items():
        if owners.get(name, 0) != mask:
            raise AssertionError(f"{name}: expected owners {mask}, got {owners.get(name, 0)}")
    cmake = (ROOT / "compat/bionic/ndk_graphics.cmake").read_text()
    block = re.search(r"set\(_gl_handwritten\s+(.*?)\)\n", cmake, re.DOTALL).group(1)
    handwritten = re.sub(r"#[^\n]*", "", block).split()
    inputs = [generator, ROOT / "compat/bionic/ndk_graphics.cmake"]
    inputs += [include / name for name in module.HEADERS]
    inputs += [include.parent / "libs" / name for name in (
        "libEGL.map.txt", "libGLESv1_CM.map.txt", "libGLESv2.map.txt", "libGLESv3.map.txt",
        "GLES2/gl2_api.in", "GLES2/gl2ext_api.in", "EGL/eglApi.cpp")]
    print("BOUNDARY actual pinned source ownership metadata and generated rows; no GL backend execution", flush=True)
    for path in inputs:
        print("INPUT_SHA256 " + str(path) + " " + hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-gl-owners-") as temporary:
        output = Path(temporary) / "gl_thunks.inc"
        subprocess.run([sys.executable, str(generator), str(output), str(include), *handwritten],
                       check=True, timeout=30)
        text = output.read_text()
        checks = len(CASES)
        for name, mask in CASES.items():
            if not re.search(r'\{"' + re.escape(name) + r'", kLib\w+, ' + str(mask) + r'\}', text):
                raise AssertionError("generated name ownership differs: " + name)
            checks += 1
            if name not in handwritten:
                if f'reinterpret_cast<void*>(&T_{name}), {mask}}}' not in text:
                    raise AssertionError("generated thunk ownership differs: " + name)
                checks += 1
        print("GENERATED_SHA256 " + hashlib.sha256(output.read_bytes()).hexdigest(), flush=True)
    print(f"GL provider ownership contracts: {checks} checks passed; {len(owners)} ownership names", flush=True)


if __name__ == "__main__":
    main()
