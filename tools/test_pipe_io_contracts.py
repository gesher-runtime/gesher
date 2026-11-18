#!/usr/bin/env python3
"""Execute production kernel-pipe operations at a declared Linux OS boundary.

The boundary models Windows message-write errors and thread-pool wait APIs
using real Linux pipes, mutexes, blocking callers, signals and cancellation.
It validates production admission, immediate paths and callback retirement,
not NPFS, Windows cancellation delivery or actual thread-pool resource cost.
The native Windows pipe component is required for those contracts.
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
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (ROOT / 'compat/posix/src/pipe_io.cc').read_text()
    first, create, after = ('namespace gesher {', 'int CreatePipeDescriptors(',
                            'namespace {\nint TransferPipeBody(')
    if any(source.count(marker) != 1 for marker in (first, create, after)):
        raise ValueError('production pipe boundaries changed')
    implementation = source[source.index(first):source.index(create)] + source[source.index(after):]
    template = (ROOT / 'tests/native/pipe_io_host_test.cc.in').read_text()
    assert template.count('@PIPE_IMPLEMENTATION@') == 1
    template = template.replace('@PIPE_IMPLEMENTATION@', implementation)
    print('Production pipe I/O SHA256', hashlib.sha256(implementation.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-pipe-io-') as temporary:
        folder = Path(temporary)
        cpp, binary = folder / 'pipe.cc', folder / 'pipe'
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ['-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            '-D_GNU_SOURCE', '-pthread', str(cpp), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=45).returncode


if __name__ == '__main__':
    raise SystemExit(main())
