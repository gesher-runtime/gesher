#!/usr/bin/env python3
"""Exercise the exact production GetEnv boundary with a controlled host VM.

The host accepts otherwise unbridged interface IDs and allocates a resource,
so these checks do not depend on ART already rejecting JVMTI. The envelope
models VM lookup and proxy selection; it is not Windows/ART/ARM execution.
--source-ref replaces only the production body, retaining identical assertions
for a historical negative control. The real guest probes run in Windows tracks
40-native-libs and 47-arm64.
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
    parser.add_argument('--source-ref')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()

    path = 'compat/bionic/jni_bridge.cc'
    source = (subprocess.run(['git', 'show', args.source_ref + ':' + path],
                             cwd=ROOT, check=True, capture_output=True, text=True).stdout
              if args.source_ref else (ROOT / path).read_text(encoding='utf-8'))
    start = 'jint SYSV GetEnvSysv(JavaVM* proxy, void** penv, jint version) {'
    end = '\n// JNI_GetCreatedJavaVMs for the .so'
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError('production GetEnv boundaries changed')
    implementation = source[source.index(start):source.index(end, source.index(start))]
    fixture = (ROOT / 'tests/native/jni_getenv_boundary_host.cc.in').read_text(encoding='utf-8')
    if fixture.count('@GET_ENV@') != 1:
        raise ValueError('GetEnv fixture marker changed')
    print('GetEnv production SHA256 ' + hashlib.sha256(implementation.encode()).hexdigest(),
          flush=True)
    print('GetEnv assertion fixture SHA256 ' + hashlib.sha256(fixture.encode()).hexdigest(),
          flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-jni-getenv-') as temporary:
        work = Path(temporary)
        generated, binary = work / 'getenv.cc', work / 'getenv'
        generated.write_text(fixture.replace('@GET_ENV@', implementation), encoding='utf-8')
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            str(generated), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=45).returncode


if __name__ == '__main__':
    raise SystemExit(main())
