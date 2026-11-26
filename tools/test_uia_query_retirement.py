#!/usr/bin/env python3
"""Run the complete production QueryPool with an explicit host thread boundary.

The native Windows lifecycle fixture and ART/Win32 owner-close gate remain the
target acceptance tests. --source-ref compiles the full historical owner so the
same active/queued retirement assertion can demonstrate the prior delay.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if sys.platform == 'win32':
        parser.error('Use the actual tests/windows/web3/query_pool_test.cc on Windows')
    with tempfile.TemporaryDirectory(prefix='gesher-uia-retirement-') as temporary:
        work = Path(temporary)
        for name in ('query_pool.h', 'query_pool.cc'):
            relative = 'compat/accessibility/' + name
            data = subprocess.check_output(['git', 'show', args.source_ref + ':' + relative],
                                           cwd=ROOT) if args.source_ref else (ROOT / relative).read_bytes()
            (work / name).write_bytes(data)
            print(relative + ' SHA256 ' + hashlib.sha256(data).hexdigest(), flush=True)
        executable = work / 'query-retirement'
        command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                   '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-I', str(ROOT / 'tests/accessibility/thread-boundary'), '-I', str(work),
                   str(work / 'query_pool.cc'), str(ROOT / 'tests/accessibility/query_retirement_host.cc'),
                   '-o', str(executable)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    main()
