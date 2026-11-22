#!/usr/bin/env python3
"""Run the exact POSIX owner/table and Bionic putenv body in a Linux envelope.

Only Win32 module/init/CRT copying boundaries are replaced. Allocation failure,
borrowed bytes, table growth and the CRT-read lock ordering execute production
bodies. Real UCRT imports, ELF relocations, font validation and child spawning
remain mandatory native Windows CI gates. The fixtures expose only owned keys.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--shim-source-ref', help='Use the exact historical Putenv body with the current unchanged fixture')
    args = parser.parse_args()
    source = (ROOT / 'compat/posix/src/environment.cc').read_text()
    internal = ROOT / 'compat/posix/src/environment_internal.h'
    source = source.replace('#include "environment_internal.h"', '#include "' + str(internal) + '"')
    shim = subprocess.check_output(['git', 'show', args.shim_source_ref + ':compat/bionic/shim.cc'], cwd=ROOT).decode() if args.shim_source_ref else (ROOT / 'compat/bionic/shim.cc').read_text()
    start = shim.index('int BIONIC_SYSV Putenv(char* assignment) {')
    if '\n}' not in shim[start:shim.index('\n\n', start)]:
        body = shim[start:shim.index('\n', start)]
    else:
        body = shim[start:shim.index('\n}', start) + 2]
    print('environment owner SHA256', hashlib.sha256((ROOT / 'compat/posix/src/environment.cc').read_bytes()).hexdigest(), flush=True)
    print('environment table SHA256', hashlib.sha256(internal.read_bytes()).hexdigest(), flush=True)
    print('guest Putenv SHA256', hashlib.sha256(body.encode()).hexdigest(), flush=True)
    template = (ROOT / 'tests/native/environment_owner_host.cc.in').read_text().replace('@ENVIRONMENT_SOURCE@', source).replace('@GUEST_PUTENV@', body).replace('@ENVIRONMENT_ENCODING_HEADER@', str(ROOT / 'compat/posix/include/gesher/environment_utf16.h'))
    with tempfile.TemporaryDirectory(prefix='gesher-environment-') as temporary:
        folder = Path(temporary)
        (folder / 'gesher').mkdir()
        (folder / 'gesher/environment.h').write_bytes((ROOT / 'compat/posix/include/gesher/environment.h').read_bytes())
        (folder / 'windows.h').write_text('// Win32 boundary declared by the host envelope.\n')
        (folder / 'owner.cc').write_text(template)
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-builtin', '-pthread', '-I', str(folder), str(folder / 'owner.cc'), '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=free', '-o', str(folder / 'test')]
        if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            # Owned environment strings deliberately outlive atexit. Disable
            # only leak detection; retain all address/undefined-behavior checks.
            environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
            environment['UBSAN_OPTIONS'] = environment.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        for allocation in range(7):
            subprocess.run([str(folder / 'test'), '--initial-failure', str(allocation)], check=True, timeout=15, env=environment)
        subprocess.run([str(folder / 'test')], check=True, timeout=30, env=environment)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
