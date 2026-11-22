#!/usr/bin/env python3
# Copyright 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Exercise exact native-fence receive/registration bodies on declared host boundaries.

The Linux CRT/Win32 adapter checks descriptor flags and ownership bookkeeping.
It does not validate Windows event names/access rights or execute the broker.
The actual Windows EGL fixture separately exercises real SCM_RIGHTS transfers.
--source-ref selects immutable production bodies for the baseline control.
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
    parser.add_argument('--source-ref')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    paths = ['compat/posix/src/native_fence.cc', 'compat/posix/src/native_fence_internal.h',
             'compat/posix/src/fd.cc', 'compat/posix/src/unix_rights.cc']
    sources = {path: (subprocess.check_output(['git', 'show', args.source_ref + ':' + path],
                    cwd=ROOT).decode() if args.source_ref else (ROOT / path).read_text())
               for path in paths}

    def section(path, first, last):
        value = sources[path]
        if value.count(first) != 1:
            raise ValueError('native-fence extraction boundary changed: ' + first)
        begin = value.index(first)
        end = value.index(last, begin)
        return value[begin:end]

    fence = paths[0]
    pieces = {
        'reserve': section(paths[2], 'int ReserveFd() {', '\nint RegisterSocket('),
        'class': section(fence, 'class NativeFence {', '\nnamespace {'),
        'registry': section(fence, 'constexpr size_t kDescriptorCapacity', '\nbool MaybeFence('),
        'membership': section(fence, 'bool MaybeFence(', '\n// A wait registration'),
        'register': section(fence, 'int Register(', '\n// Only our immutable'),
        'lookup_retire': section(fence, 'std::shared_ptr<NativeFence> LookupNativeFence(', '\nvoid DupNativeFence('),
        'adopt': section(fence, 'int AdoptNativeFence(', '\nshort PollNativeFence('),
        'reply': section(paths[3], 'struct Reply {', '\nstd::string Device()'),
        'receive': section(paths[3], 'bool ReadDelivered(Reply& reply,', '\n// A view of one'),
    }
    declaration = [line for line in sources[paths[1]].splitlines()
                   if line.startswith('int AdoptNativeFence(')]
    if len(declaration) != 1 or not declaration[0].endswith(';'):
        raise ValueError('native-fence adoption declaration changed')
    pieces['declaration'] = declaration[0]
    print('NATIVE_FENCE_RECEIVE_PRODUCTION ' + ' '.join(
        name + '=' + hashlib.sha256(value.encode()).hexdigest()
        for name, value in pieces.items()), flush=True)
    template = (ROOT / 'tests/native/native_fence_receive_host.cc.in').read_text()
    bodies = '\n'.join(pieces[key] for key in (
        'reserve', 'class', 'registry', 'membership', 'register', 'lookup_retire',
        'declaration', 'adopt', 'reply', 'receive'))
    if template.count('@PRODUCTION@') != 1:
        raise ValueError('native-fence fixture marker changed')
    with tempfile.TemporaryDirectory(prefix='gesher-fence-receive-') as directory:
        source, binary = Path(directory) / 'test.cc', Path(directory) / 'test'
        source.write_text(template.replace('@PRODUCTION@', bodies))
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra',
                  '-Werror', '-pthread', str(source), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())

