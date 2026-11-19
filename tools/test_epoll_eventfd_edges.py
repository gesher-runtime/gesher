#!/usr/bin/env python3
"""Exercise complete production eventfd/epoll over declared host boundaries.

Win32 events, PollFds, CRT reservations and unrelated socket/timer/inotify calls
are host adapters; producer notifications, ctl, registry, delivery and nesting
are the actual production source. The same public API corpus also runs on native
Linux. Actual Windows validation is epoll_probe --eventfd-edge; this host test
never claims Win32 execution. --source-ref runs an unchanged baseline control.
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
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--keep-dir', type=Path)
    args = parser.parse_args()
    source_path = 'compat/posix/src/epoll.cc'
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + source_path], cwd=ROOT).decode()
              if args.source_ref else (ROOT / source_path).read_text())
    print('production epoll.cc sha256=' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    source = re.sub(r'^#include "(?:event_internal|internal|socket_internal|record_lock_descriptors)\.h"\n', '', source, flags=re.M)
    source = source.replace('#include <windows.h>\n', '')
    template = (ROOT / 'tests/native/epoll_eventfd_edges_host.cc.in').read_text()
    if template.count('@EPOLL_SOURCE@') != 1:
        raise ValueError('ambiguous production-source marker')
    host = template.replace('@EPOLL_SOURCE@', source)
    reference = '''#include <stdio.h>
static int failed, checked;
static void check(const char* label, int passed) {
  ++checked; if (!passed) { ++failed; printf("FAIL %s\\n", label); }
}
#include "epoll_eventfd_edge_test.inc"
int main() {
  test_epoll_eventfd_edges();
  printf("eventfd edge Linux reference: %d checks, %d failures\\n", checked, failed);
  return failed != 0;
}
'''
    failure = False
    with tempfile.TemporaryDirectory(prefix='gesher-eventfd-edge-') as temporary:
        directory = args.keep_dir or Path(temporary)
        directory.mkdir(parents=True, exist_ok=True)
        for role, content in [('reference', reference), ('production-host', host)]:
            path = directory / (role + '.cc')
            path.write_text(content)
            output = directory / role
            command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                      '-Wno-unused-function', '-pthread', '-I', str(ROOT / 'tests/native'), str(path), '-o', str(output)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all']
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
            (directory / (role + '.txt')).write_text(result.stdout + result.stderr)
            print(result.stdout, end='', flush=True)
            if result.returncode:
                failure = True
                print(f'FAIL {role}: exit={result.returncode}\n{result.stderr[:8000]}', flush=True)
    return int(failure)


if __name__ == '__main__':
    raise SystemExit(main())
