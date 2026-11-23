#!/usr/bin/env python3
"""Compile complete production raw-DNS bodies with declared socket/platform edges.

The Windows run uses the real private Windows thread pool and real loopback
UDP/TCP peers. Linux uses a thread-pool adapter; neither run substitutes the
production admission, queue, request owner, wire exchange, result or cancel code.
The separately compiled production TU uses Gesher's actual POSIX headers.
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
SOURCE = 'compat/posix/src/network_dns.cc'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref', help='prior production source for measured unbounded control')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'cl' if os.name == 'nt' else 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path, help='retain generated source/binary without running')
    args = parser.parse_args()
    production = (subprocess.check_output(['git', 'show', args.source_ref + ':' + SOURCE],
            cwd=ROOT, text=True) if args.source_ref else (ROOT / SOURCE).read_text())
    # Includes are the only removed source lines. All owner/worker/wire bodies
    # compile unchanged against the explicitly labelled platform adapters.
    body = re.sub(r'^#include[^\n]*\n', '', production, flags=re.MULTILINE)
    template = (ROOT / 'tests/native/dns_admission_host.cc.in').read_text()
    source = template.replace('@PRODUCTION@', body).replace('@BOUNDED@',
            '0' if args.source_ref else '1')
    print(SOURCE + ' SHA256 ' + hashlib.sha256(production.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-dns-admission-') as temporary:
        work = args.output or Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        cpp = work / 'dns_admission.cc'; cpp.write_text(source)
        exe = work / ('dns_admission.exe' if os.name == 'nt' else 'dns_admission')
        cxx = shlex.split(args.cxx)
        if Path(cxx[0]).name.lower() in ('cl', 'cl.exe'):
            command = cxx + ['/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
                    '/D_WIN32_WINNT=0x0a00', '/DWINVER=0x0a00', str(cpp),
                    '/Fe:' + str(exe), '/Fo:' + str(work) + '/', 'ws2_32.lib', 'bcrypt.lib']
        else:
            command = cxx + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-pthread', str(cpp), '-o', str(exe)]
            if 'mingw' in cxx[0]: command += ['-static', '-lws2_32', '-lbcrypt']
            if args.sanitize: command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
        subprocess.run(command, check=True, timeout=120)
        if not args.output:
            subprocess.run([str(exe)], check=True, timeout=75)


if __name__ == '__main__':
    main()
