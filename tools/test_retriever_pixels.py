#!/usr/bin/env python3
"""Compile the complete retriever renderer and its full-image Skia pixel oracle.

The caller supplies an already built CPU Skia library from the pinned AOSP tag.
This is actual Skia raster execution, not ART, JNI or a Windows decoder test.
Sanitizers cover this renderer and fixture; the supplied library keeps its own
build instrumentation. RTTI is disabled to match Skia's default CPU library,
so vptr sanitizer checks are unavailable. The CMake target exercises
the same source against the product's Skia build on Windows.
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
    parser.add_argument('--skia-root', type=Path, required=True)
    parser.add_argument('--skia-library', type=Path, required=True)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--link-flag', action='append', default=[])
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = args.skia_root.resolve()
    library = args.skia_library.resolve()
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    print('Skia revision ' + revision, flush=True)
    print('Skia library SHA256 ' + hashlib.sha256(library.read_bytes()).hexdigest(), flush=True)
    sources = ['compat/media/video/retriever_pixels.cc', 'compat/media/video/retriever_pixels.h',
               'tests/media/retriever_pixels_test.cc']
    for path in sources:
        print(path + ' SHA256 ' + hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-retriever-pixels-') as temporary:
        output = Path(temporary) / 'retriever-pixels'
        command = shlex.split(args.cxx) + [
            '-std=c++17', '-O1' if args.sanitize else '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-fno-rtti', '-DSK_R32_SHIFT=0', '-I', str(ROOT), '-I', str(root),
            str(ROOT / sources[0]), str(ROOT / sources[2]), str(library), '-pthread', '-lz',
            '-o', str(output)] + args.link_flag
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(output)], check=False, timeout=120).returncode


if __name__ == '__main__':
    raise SystemExit(main())
