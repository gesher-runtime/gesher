#!/usr/bin/env python3
"""Exercise production monitor pacing with a declared Win32 display boundary.

Windows compilers also compile/link the real implementation with the production
NDK graphics flags, including NOGDI, and a consumer that includes windows.h first.
The recording fixture separately checks mode/monitor behavior; it does not
establish physical-display refresh or compositor timing.
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


def check_windows_build(cxx, work):
    macros = subprocess.run(cxx + ['-dM', '-E', '-x', 'c++', '-'], input='', text=True,
                            stdout=subprocess.PIPE, check=True, timeout=30).stdout
    if not re.search(r'^#define _WIN32\b', macros, flags=re.MULTILINE):
        return
    # These are the flags of gesher_ndk_graphics, not a permissive standalone
    # header check. Keep the API implementation in its own translation unit.
    flags = ['-DGESHER', '-DNOGDI', '-DNOMINMAX', '-DUSE_MINGW', '-DWIN32_LEAN_AND_MEAN',
             '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-std=gnu++20',
             '-mno-ms-bitfields', '-fno-exceptions', '-Wall', '-Wextra', '-Werror']
    subprocess.run(cxx + flags + ['-I', str(ROOT),
                   str(ROOT / 'tests/native/display_refresh_compile_test.cc'),
                   str(ROOT / 'compat/bionic/display_refresh.cc'), '-luser32',
                   '-o', str(work / 'display_refresh_compile_test.exe')],
                   check=True, timeout=120)
    print('display refresh: production Win32 SDK compile/link passed with NOGDI', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error('--compile-only requires --output')
    header = (ROOT / 'compat/bionic/display_refresh.h').read_text()
    source = (ROOT / 'compat/bionic/display_refresh.cc').read_text()
    declarations = re.sub(r'^\s*#(?:include|pragma).*\n', '', header, flags=re.MULTILINE)
    body = source[source.index('namespace gesher::bionic::gl {'):]
    fixture = (ROOT / 'tests/media/display_refresh_host.cc.in').read_text().replace(
            '@PRODUCTION@', declarations + '\n' + body)
    print('display refresh header SHA256 ' + hashlib.sha256(header.encode()).hexdigest(), flush=True)
    print('display refresh implementation SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-display-refresh-') as temporary:
        work = Path(temporary)
        cxx = shlex.split(args.cxx)
        check_windows_build(cxx, work)
        cpp = work / 'display_refresh.cc'
        cpp.write_text(fixture)
        binary = args.output.resolve() if args.output else work / ('display.exe' if os.name == 'nt' else 'display')
        binary.parent.mkdir(parents=True, exist_ok=True)
        command = cxx + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
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
