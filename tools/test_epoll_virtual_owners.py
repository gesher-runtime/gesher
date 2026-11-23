#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Test the actual epoll virtual-file registry against native Linux contracts.

The full production epoll.cc runs over the existing, explicit Win32/CRT/PollFds
host boundaries. The public fd/alias corpus also runs without adapters on Linux
and in epoll_probe on Windows. Linux/Windows run real timerfd and inotify too;
the host corpus does not claim those platform backends. --source-ref compiles
the unchanged production baseline with the same corpus and close-work counter.
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


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('ambiguous host boundary: ' + old[:80])
    return text.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--keep-dir', type=Path)
    parser.add_argument('--measure', action='store_true', help='run close/common-path work measurements only in the host')
    args = parser.parse_args()

    def read(path):
        if args.source_ref:
            return subprocess.check_output(['git', 'show', args.source_ref + ':' + path], cwd=ROOT).decode()
        return (ROOT / path).read_text()

    source = read('compat/posix/src/epoll.cc')
    print('production epoll.cc sha256=' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    source = re.sub(r'^#include "(?:event_internal|internal|socket_internal|record_lock_descriptors|native_fence_internal)\.h"\n', '', source, flags=re.M)
    source = source.replace('#include <windows.h>\n', '')
    template = (ROOT / 'tests/native/epoll_eventfd_edges_host.cc.in').read_text()
    template = replace_once(template, 'int main() {', 'static int existing_edge_main() {')
    template = replace_once(template, 'static std::atomic<unsigned> host_poll_calls{0};',
                            'static std::atomic<unsigned> host_poll_calls{0}, host_blocked_polls{0};\n'
                            'static std::atomic<bool> owner_pause_poll_return{false}, owner_poll_return_reached{false}, '
                            'owner_allow_poll_return{false}, owner_poll_return_resumed{false};')
    # The real fd.cc captures shared EventFd references for the complete call,
    # including its blocking wait. Preserve that crucial ownership seam here;
    # a numeric virtual target must not accidentally pass this epoll lifetime test.
    template = replace_once(template,
        'static int PollFds(pollfd* fds, nfds_t count, int timeout, HANDLE interrupt, bool* interrupted) {',
        'struct OwnerHostPollScratch {\n'
        '  std::vector<std::shared_ptr<EventFd>> retained;\n'
        '  void Reset() { ClearPollScratch(retained); }\n'
        '};\n'
        'static int PollFds(pollfd* fds, nfds_t count, int timeout, HANDLE interrupt, bool* interrupted) {\n'
        '  PollScratchLease<OwnerHostPollScratch> retained_lease;\n'
        '  auto& retained = retained_lease.get().retained;\n'
        '  retained.resize(count);\n'
        '  for (nfds_t i = 0; i < count; ++i) if (fds[i].fd >= 0) retained[i] = LookupEventFd(fds[i].fd);')
    template = replace_once(template,
        '      if (!HostValid(item.fd)) item.revents = POLLNVAL;\n'
        '      else if (auto source = LookupEventFd(item.fd)) item.revents = PollEventFd(*source, item.events, nullptr);',
        '      if (retained[i]) item.revents = PollEventFd(*retained[i], item.events, nullptr);\n'
        '      else if (!HostValid(item.fd)) item.revents = POLLNVAL;')
    template = replace_once(template, '    std::unique_lock<std::mutex> lock(host_events_mutex);\n    if (timeout < 0)',
                            '    std::unique_lock<std::mutex> lock(host_events_mutex);\n    ++host_blocked_polls;\n    if (timeout < 0)')
    template = replace_once(template, '    if (interrupt && before != host_epoch) {',
        '    --host_blocked_polls;\n'
        '    if (owner_pause_poll_return.exchange(false)) {\n'
        '      lock.unlock();\n'
        '      owner_poll_return_reached = true;\n'
        '      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);\n'
        '      while (!owner_allow_poll_return && std::chrono::steady_clock::now() < end) std::this_thread::yield();\n'
        '      owner_poll_return_resumed = owner_allow_poll_return.load();\n'
        '      lock.lock();\n'
        '    }\n'
        '    if (interrupt && before != host_epoch) {')
    template = replace_once(template, 'static int HostDup(int fd) { return ::dup(fd); }',
                            'static int HostDup(int fd) { return ::dup(fd); }\n'
                            'static int HostDup2(int from, int to) { return ::dup2(from, to); }\n'
                            'static int HostFcntl(int fd, int cmd, int arg) { return ::fcntl(fd, cmd, arg); }')
    template = replace_once(template, 'struct EventFd; struct Inotify; struct TimerFd; class NativeFence;',
                            'struct EventFd; class NativeFence;\n'
                            '// Isolated payload lifetime boundaries, not timer/directory emulation.\n'
                            'static int owner_inotify_destroyed, owner_timer_destroyed;\n'
                            'struct Inotify { std::shared_ptr<EventFd> ready; ~Inotify() { ++owner_inotify_destroyed; } };\n'
                            'struct TimerFd { std::shared_ptr<EventFd> ready; ~TimerFd() { ++owner_timer_destroyed; } };')
    allocations = r'''// Counts real C++ allocations only inside the measured close interval.
#include <cstdlib>
#include <new>
#include <functional>
static bool owner_count_allocations;
static size_t owner_allocations, owner_allocation_bytes;
[[gnu::noinline]] void* operator new(size_t size) {
  void* result = std::malloc(size == 0 ? 1 : size);
  if (!result) throw std::bad_alloc();
  if (owner_count_allocations) { ++owner_allocations; owner_allocation_bytes += size; }
  return result;
}
[[gnu::noinline]] void* operator new[](size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, size_t) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory, size_t) noexcept { std::free(memory); }
'''
    extension = '\n' + (ROOT / 'tests/native/epoll_virtual_owners_host.inc').read_text()
    host = allocations + replace_once(template, '@EPOLL_SOURCE@', source) + extension
    reference = r'''#include <stdio.h>
static int checked, failed;
static void check(const char* label, int passed) {
  ++checked; if (!passed) { ++failed; printf("FAIL %s\n", label); }
}
#define OWNER_READONLY_REAL
#include "epoll_virtual_owner_test.inc"
int main() {
  test_epoll_virtual_owners(); test_epoll_readonly_owners();
  printf("virtual epoll owner Linux reference: %d checks, %d failures\n", checked, failed);
  return failed != 0;
}
'''
    variants = [('reference', reference), ('production-host', host)]
    if not args.source_ref and not args.measure:
        mutant = replace_once(source, '      item.fd = -1;', '      item.fd = key.fd;')
        print('negative control pin-virtual-wait body sha256=' + hashlib.sha256(mutant.encode()).hexdigest(), flush=True)
        variants.append(('pin-virtual-wait-negative',
                         allocations + replace_once(template, '@EPOLL_SOURCE@', mutant) + extension))
    failure = False
    with tempfile.TemporaryDirectory(prefix='gesher-epoll-owner-') as temporary:
        directory = args.keep_dir or Path(temporary)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'poll_scratch.h').write_text(read('compat/posix/src/poll_scratch.h'))
        for role, content in variants:
            path = directory / (role + '.cc')
            path.write_text(content)
            output = directory / role
            command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                      '-Wno-unused-function', '-pthread', '-I', str(ROOT / 'tests/native'), str(path), '-o', str(output)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
            subprocess.run(command, check=True, timeout=120)
            negative = role == 'pin-virtual-wait-negative'
            run = [str(output)] + (['--wait-lifetime-only'] if negative else
                                  ['--measure'] if args.measure and role == 'production-host' else [])
            result = subprocess.run(run, capture_output=True, text=True, timeout=30)
            (directory / (role + '.txt')).write_text(result.stdout + result.stderr)
            if negative:
                print(''.join('EXPECTED_NEGATIVE ' + line + '\n' for line in result.stdout.splitlines()), end='', flush=True)
                observed = (result.returncode == 1 and not result.stderr and
                            [line for line in result.stdout.splitlines() if line.startswith('FAIL ')] ==
                            ['FAIL owner snapshot/wait never pins final file'])
                print('PASS retained-poll negative control' if observed else 'FAIL retained-poll negative control', flush=True)
                failure |= not observed
            else:
                print(result.stdout, end='', flush=True)
                if result.returncode:
                    failure = True
                    print(f'FAIL {role}: exit={result.returncode}\n{result.stderr[:8000]}', flush=True)
    return int(failure)


if __name__ == '__main__':
    raise SystemExit(main())
