#!/usr/bin/env python3
"""Exercise production monitor pacing with a declared Win32 display boundary.

Cross compilation checks the real Windows API declarations. These deterministic
mode/monitor tests do not establish physical-display refresh or compositor timing.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error('--compile-only requires --output')
    source = (ROOT / 'compat/bionic/display_refresh.h').read_text()
    body = re.sub(r'^\s*#(?:include|pragma).*\n', '', source, flags=re.MULTILINE)
    fixture = (ROOT / 'tests/media/display_refresh_host.cc.in').read_text().replace('@PRODUCTION@', body)
    print('display refresh source SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-display-refresh-') as temporary:
        work = Path(temporary)
        cpp = work / 'display_refresh.cc'
        cpp.write_text(fixture)
        binary = args.output.resolve() if args.output else work / ('display.exe' if os.name == 'nt' else 'display')
        binary.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                                         str(cpp), '-o', str(binary)]
        if args.compile_only:
            command += ['-static']
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        if args.compile_only:
            print('Compiled display refresh fixture: ' + str(binary), flush=True)
            return 0
        return subprocess.run([str(binary)], check=False, timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
