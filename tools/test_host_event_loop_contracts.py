#!/usr/bin/env python3
"""Exercise production epoll delivery, vector routing and host select with declared boundaries.

The epoll host replaces registry/Win32 wait handles and uses real Linux eventfds
and poll; it compiles the actual Epoll state, event conversion and wait function.
The vector host compiles actual writev/socket dispatch, scalar write dispatch and
virtual-object write checks, with Linux I/O behind declared CRT/device boundaries.
The select host compiles every production function with a controlled monotonic
clock and poll boundary. Actual public calls also run in Windows POSIX and both
ELF guest fixtures. --source-ref is the same-harness old-source control.
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


def between(text, begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError('ambiguous production boundary: ' + begin)
    return text[text.index(begin):text.index(end, text.index(begin))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--case', choices=['epoll', 'vector', 'select'])
    args = parser.parse_args()

    def read(path):
        if args.source_ref:
            return subprocess.check_output(['git', 'show', args.source_ref + ':' + path], cwd=ROOT).decode()
        return (ROOT / path).read_text()

    epoll = read('compat/posix/src/epoll.cc')
    system = read('compat/posix/src/system.cc')
    fd = read('compat/posix/src/fd.cc')
    select = read('compat/posix/src/select.cc')
    pieces = {
        'epoll': {
            '@EPOLL_STATE@': between(epoll, 'struct Epoll;\n', 'class EpollWaiter {'),
            '@EPOLL_CONVERSION@': between(epoll, '// epoll\'s event bits below', '// Linux permits at most five'),
            '@EPOLL_WAIT@': between(epoll, 'static int EpollWaitImpl(', '\nint epoll_wait('),
        },
        'vector': {
            '@VECTOR_HELPERS@': between(system, '// readv/writev on a socket fd:', '}  // namespace\n\nssize_t readv('),
            '@WRITEV@': between(system, 'ssize_t writev(', '\nssize_t preadv('),
            '@SCALAR_WRITE@': between(fd, 'int write(int fd,', '\nint dup('),
            '@VIRTUAL_WRITE@': between(epoll, 'bool WriteEventObject(', '\nbool ForgetEventObject('),
        },
        'select': {'@SELECT_SOURCE@': re.sub(r'^#include[^\n]*\n', '', select, flags=re.M)},
    }
    # The old source has no vector permission helper. It never calls this
    # declaration; keeping it absent preserves the old behavior control.
    for path, source in [('epoll.cc', epoll), ('system.cc', system), ('fd.cc', fd), ('select.cc', select)]:
        print(path, hashlib.sha256(source.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix='gesher-host-event-loop-') as directory:
        work = Path(directory)
        for name, replacements in pieces.items():
            if args.case and args.case != name:
                continue
            content = (ROOT / ('tests/native/host_' + name + '_contract_test.cc.in')).read_text()
            for marker, value in replacements.items():
                if content.count(marker) != 1:
                    raise ValueError('ambiguous test marker: ' + marker)
                content = content.replace(marker, value)
            source = work / (name + '.cc')
            output = work / name
            source.write_text(content)
            command = shlex.split(args.cxx) + [
                '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                '-pthread', '-I', str(ROOT / 'tests/native'), str(source), '-o', str(output)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
            print(result.stdout, end='', flush=True)
            if result.returncode:
                failed.append(name)
                print(f'FAIL {name}: exit={result.returncode}\n{result.stderr[:12000]}', flush=True)
            else:
                print('PASS ' + name, flush=True)
    return int(bool(failed))


if __name__ == '__main__':
    raise SystemExit(main())
