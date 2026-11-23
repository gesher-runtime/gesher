#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile and execute the ordinary Linux reference with owned process deadlines."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'tests/exec/realtime_signals_contract.c'
    args.out.mkdir(parents=True, exist_ok=True)
    binary = (args.out / 'linux-realtime-contract').resolve()
    command = [args.cc, '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-pthread', str(source), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=60)
    cases = ['fifo', 'ignore', 'quota', 'priority', 'timeout', 'thread', 'handler',
             'cross', 'stop', 'status']
    results = []
    for case in cases:
        started = time.monotonic()
        process = subprocess.Popen([str(binary), case, str(binary)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        timedout = False
        try:
            stdout, stderr = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            timedout = True
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=5)
        passed = (not timedout and process.returncode == 0 and
                  'realtime contract: 0 failures' in stdout and
                  not any(line.startswith('FAIL') for line in stdout.splitlines()))
        result = dict(case=case, exit=process.returncode, timeout=timedout,
                      elapsed_seconds=time.monotonic() - started, stdout=stdout,
                      stderr=stderr, passed=passed)
        results.append(result)
        print(('PASS' if passed else 'FAIL') + ' Linux reference: ' + case, flush=True)
        if not passed:
            print(stdout + stderr, flush=True)
    report = dict(role='ordinary-linux-reference', source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  compiler_command=command, results=results)
    (args.out / 'linux-results.json').write_text(json.dumps(report, indent=2))
    storage = (args.out / 'isolated-pending-storage').resolve()
    storage_command = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-fno-exceptions', '-iquote', str(root / 'compat/posix/include'),
                       str(root / 'tests/native/pending_signals_storage_test.cc'),
                       '-Wl,--wrap=malloc', '-Wl,--wrap=free', '-o', str(storage)]
    subprocess.run(storage_command, check=True, timeout=60)
    storage_result = subprocess.run([str(storage)], capture_output=True, text=True, timeout=10)
    (args.out / 'isolated-storage.json').write_text(json.dumps(dict(
        role='isolated-production-container-with-allocation-faults',
        command=storage_command, exit=storage_result.returncode,
        stdout=storage_result.stdout, stderr=storage_result.stderr), indent=2))
    print(storage_result.stdout, end='', flush=True)
    if storage_result.returncode != 0:
        return 1
    return int(any(not result['passed'] for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
