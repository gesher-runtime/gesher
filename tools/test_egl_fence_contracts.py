#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the actual EGL fence object/ownership bodies on declared host boundaries.

ANGLE calls, queued GPU completion and descriptor operations are explicit
adapters. This checks EGL validation, ownership and concurrency; it does not
establish Windows driver ordering or application performance. Use --case async
with --source-ref 8b316aebf0 for the original finish-at-create negative control.
The Windows EGL fixture separately executes the real backend.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'async'), default='all')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    path = 'compat/bionic/egl_native_buffer.cc'
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + path],
              cwd=ROOT, text=True) if args.source_ref else (ROOT / path).read_text())
    begin, end = 'struct Sync {', '// --- extensions '
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError('EGL fence extraction boundary changed')
    bodies = source[source.index(begin):source.index(end)]
    print('EGL fence production-body SHA256 ' + hashlib.sha256(bodies.encode()).hexdigest(), flush=True)
    template = (ROOT / 'tests/native/egl_fence_host.cc.in').read_text()
    if template.count('@EGL_FENCES@') != 1:
        raise ValueError('EGL fence fixture marker changed')
    with tempfile.TemporaryDirectory(prefix='gesher-egl-fence-') as directory:
        cpp, binary = Path(directory) / 'test.cc', Path(directory) / 'test'
        cpp.write_text(template.replace('@EGL_FENCES@', bodies))
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                  '-Werror', '-Wno-unused-function', '-pthread', str(cpp), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary), args.case], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
