#!/usr/bin/env python3
"""Exercise production epoll delivery, vector routing and host select with declared boundaries.

The epoll host replaces registry/Win32 wait handles and uses real Linux eventfds
and poll; it compiles the actual Epoll state, event conversion and wait function.
The vector host compiles actual writev/socket dispatch, scalar write dispatch and
virtual-object write checks, with Linux I/O behind declared CRT/device boundaries.
The select host compiles every production function with a controlled monotonic
clock and poll boundary. Actual public calls also run in Windows POSIX and both
ELF guest fixtures. --source-ref is the same-harness old-source control.

The scratch host compiles the actual outer poll, EpollWaiter and nested epoll
sampling/delivery with Linux eventfds/poll behind the Win32/Winsock seams. It
also pins actual Linux pipe descriptors behind the OFD snapshot boundary and
checks ownership, recursion, deadlines and allocation budgets without claiming
Windows scheduling, whole-application memory or native WSAPoll performance.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def between(text, begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError('ambiguous production boundary: ' + begin)
    return text[text.index(begin):text.index(end, text.index(begin))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--source-ref')
    source.add_argument('--fd-source', type=Path,
                        help='use an exact descriptor source for focused negative controls')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--case', choices=['epoll', 'vector', 'select', 'scratch'])
    parser.add_argument('--measure', action='store_true',
                        help='report scratch allocation counts without requiring the new allocation budget')
    args = parser.parse_args()

    def read(path):
        if args.source_ref:
            return subprocess.check_output(['git', 'show', args.source_ref + ':' + path], cwd=ROOT).decode()
        return (ROOT / path).read_text()

    epoll = read('compat/posix/src/epoll.cc')
    system = read('compat/posix/src/system.cc')
    fd = args.fd_source.read_text() if args.fd_source else read('compat/posix/src/fd.cc')
    select = read('compat/posix/src/select.cc')
    scratch_header = (read('compat/posix/src/poll_scratch.h')
                      if 'PollScratchLease' in fd or 'PollScratchLease' in epoll else '')
    socket_header = read('compat/posix/src/socket_internal.h')
    eventfd_namespace = 'namespace gesher {\n\n'
    # Include the producer wake channels and watches declared before EventFd.
    eventfd_state = between(epoll, eventfd_namespace, '\nnamespace {\n\nstruct Epoll;')
    pieces = {
        'epoll': {
            '@VIRTUAL_OWNER@': '#define HAS_VIRTUAL_OWNER 1' if 'struct ObjectOwner {' in epoll else '',
            '@EPOLL_STATE@': between(epoll, 'struct Epoll;\n', 'class EpollWaiter {'),
            '@EPOLL_CONVERSION@': between(epoll, '// epoll\'s event bits below', '// Linux permits at most five'),
            '@EPOLL_WAIT@': between(epoll, 'static int EpollWaitImpl(', '\nint epoll_wait('),
        },
        'vector': {
            '@VECTOR_HELPERS@': between(system, '// readv/writev on a socket fd:', '}  // namespace\n\nssize_t readv('),
            '@WRITEV@': between(system, 'ssize_t writev(', '\nssize_t preadv('),
            '@PWRITEV@': between(system, 'ssize_t pwritev(', '\nssize_t process_vm_readv('),
            '@SCALAR_WRITE@': (
                between(fd, 'void ReleaseCanceledSnapshot(', '// The caller owns an exact native object')
                if 'void ReleaseCanceledSnapshot(' in fd else '') + between(fd, 'int write(int fd,', '\nint dup('),
            '@VIRTUAL_WRITE@': between(epoll, 'bool WriteEventObject(', '\nbool ForgetEventObject('),
        },
        'select': {'@SELECT_SOURCE@': re.sub(r'^#include[^\n]*\n', '', select, flags=re.M)},
        'scratch': {
            '@VIRTUAL_OWNER@': '#define HAS_VIRTUAL_OWNER 1' if 'struct ObjectOwner {' in epoll else '',
            '@EVENTFD_STATE@': eventfd_state.removeprefix(eventfd_namespace),
            '@EPOLL_STATE@': between(epoll, 'struct Epoll;\n', '// fd -> object,'),
            '@EPOLL_CONVERSION@': between(epoll, '// epoll\'s event bits below', '// Linux permits at most five'),
            '@POLL_EVENTFD@': between(epoll, 'short PollEventFd(',
                '\nint SampleVirtualEntries(EpollPollScratch& scratch, bool prepare_wait) {'
                if 'struct ObjectOwner {' in epoll else '\nbool IsNonBlocking('),
            '@POLL_EPOLL@': between(epoll,
                'bool PollEpollOwned(' if 'struct ObjectOwner {' in epoll else 'bool PollEpoll(',
                '\nstd::shared_ptr<EventFd> LookupEventFd('),
            '@POLL_FDS@': between(fd, '// --- poll ', '\nextern "C" int poll('),
            '@EPOLL_WAIT@': between(epoll, 'static int EpollWaitImpl(', '\nint epoll_wait('),
            '@HAS_SCRATCH@': '#define HAS_POLL_SCRATCH 1' if 'PollScratchLease<FdPollScratch>' in fd else '',
        },
    }
    # The old source has no vector permission helper. It never calls this
    # declaration; keeping it absent preserves the old behavior control.
    for path, source in [('epoll.cc', epoll), ('system.cc', system), ('fd.cc', fd), ('select.cc', select)]:
        print(path, hashlib.sha256(source.encode()).hexdigest(), flush=True)
    if scratch_header:
        print('poll_scratch.h', hashlib.sha256(scratch_header.encode()).hexdigest(), flush=True)
    failed = []
    with tempfile.TemporaryDirectory(prefix='gesher-host-event-loop-') as directory:
        work = Path(directory)
        # Quoted includes resolve beside the generated source. An older source
        # with no scratch header sees an empty one, not the current helper.
        (work / 'poll_scratch.h').write_text(scratch_header)
        (work / 'socket_internal.h').write_text(socket_header)
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
                '-pthread', '-I', str(ROOT / 'tests/native'), '-I', str(ROOT / 'compat/posix/src'),
                str(source), '-o', str(output)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
            subprocess.run(command, check=True, timeout=120)
            run = [str(output)] + (['measure'] if args.measure and name == 'scratch' else [])
            result = subprocess.run(run, capture_output=True, text=True, timeout=60)
            print(result.stdout, end='', flush=True)
            if result.returncode:
                failed.append(name)
                print(f'FAIL {name}: exit={result.returncode}\n{result.stderr[:12000]}', flush=True)
            else:
                print('PASS ' + name, flush=True)
    if args.case in (None, 'epoll'):
        # Complete eventfd producers and epoll state complement the declared
        # fairness adapter above, under the same maintained CI gate.
        command = [sys.executable, str(ROOT / 'tools/test_epoll_eventfd_edges.py'), '--cxx', args.cxx]
        if args.source_ref:
            command += ['--source-ref', args.source_ref]
        if args.sanitize:
            command += ['--sanitize']
        if subprocess.run(command, timeout=180).returncode:
            failed.append('eventfd-edge')
        # Whole production registry/ctl/owners are exercised independently of
        # the deliberately numeric descriptor boundaries in the older corpora.
        command = [sys.executable, str(ROOT / 'tools/test_epoll_virtual_owners.py'), '--cxx', args.cxx]
        if args.source_ref:
            command += ['--source-ref', args.source_ref]
        if args.sanitize:
            command += ['--sanitize']
        if subprocess.run(command, timeout=180).returncode:
            failed.append('virtual-owner')
    return int(bool(failed))


if __name__ == '__main__':
    raise SystemExit(main())
