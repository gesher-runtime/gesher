#!/usr/bin/env python3
"""Exercise complete QueueSurface/Win32Surface source against declared host boundaries.

Android native-window dispatch/refcounts and the Win32 Window are small in-memory
adapters here, not actual platform implementations. Both complete production
surfaces retain their state, mutexes, buffers, geometry and damage handling. The
same shared assertion header is also compiled against real surfaces on Windows.
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
    parser.add_argument('--source-ref')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / path).read_text()
    paths = ['compat/media/surface/queue_surface.h', 'compat/media/surface/queue_surface.cc',
             'compat/hwui/window/win32_surface.h', 'compat/hwui/window/win32_surface.cc']
    original = [read(path) for path in paths]
    adapted = [re.sub(r'^\s*#(?:include|pragma).*\n', '', text, flags=re.MULTILINE)
               .replace('protected:', 'public:') for text in original]
    constants = set(re.findall(r'\bNATIVE_WINDOW_[A-Z0-9_]+\b', '\n'.join(original)))
    constants.discard('NATIVE_WINDOW_TIMESTAMP_AUTO')
    enum = 'enum { ' + ', '.join(sorted(constants)) + ' };\n'
    source = (ROOT / 'tests/media/surface_geometry_host.cc.in').read_text()
    source = source.replace('@CONSTANTS@', enum).replace('@PRODUCTION@', '\n'.join(adapted))
    source = source.replace('@ASSERTIONS@', (ROOT / 'tests/media/surface_geometry_assertions.h').read_text())
    print('surface geometry source SHA256 ' + hashlib.sha256('\n'.join(original).encode()).hexdigest(),
          flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-surface-geometry-') as temporary:
        work = Path(temporary)
        generated = work / 'surface.cc'
        generated.write_text(source)
        binary = work / 'surface'
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                         '-pthread', str(generated), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
