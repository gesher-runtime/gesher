#!/usr/bin/env python3
"""CI-only controller for actual Windows resources and internal failure dependencies.

This suite does not qualify atomic spawn, blocking waits or inherited OFD locks.
Every mode runs in a fresh owned process, including intentional quarantine.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def required_cases(mode):
    if mode == 'raw-race':
        return {
            'held-reservation': 'actual occupied NUL target survives simultaneous raw CRT sentinel allocation',
            'raw-witness-contention': '1000 actual minimum duplicates preserve concurrent raw publisher witnesses and sentinel bytes',
            'raw-cleanup': 'raw publisher race leaves no private fd or native handle growth',
        }
    if mode == 'capacity':
        return {
            'actual-emfile': 'actual CRT allocator reaches real EMFILE',
            'actual-8192-slots': 'all8192 actual low-I/O descriptor slots are occupied',
            'duplicate-emfile-cleanup': 'actual full-table duplicate fails EMFILE with native reservation cleanup',
            'source-retained': 'capacity failure preserves original source fd',
            'capacity-cleanup': 'capacity failure returns native handles and private ownership to baseline',
        }
    if mode == 'retained-nul':
        return {
            'retained-nul-owned': 'every failed-close private NUL remains owned and counted',
            'real-fd-reuse': 'actual CRT publisher reuses retained fd number',
            'foreign-reuse-survives': 'reused foreign FILE_OBJECT survives private cleanup',
            'locks-preserved': 'retained NUL cleanup preserves actual POSIX/OFD source ownership',
        }
    if mode.startswith('quarantine-'):
        cases = {
            'quarantine-owned': 'uncertain source alias is explicitly retained and counted',
            'quarantine-fails-closed': 'public close fails closed before changing original ownership',
            'quarantine-real-cleanup': 'fixture retires its actual identified uncertain alias',
            'quarantine-foreign-reuse': 'quarantine never retries/recloses foreign reused fd',
        }
        live = {
            'quarantine-socket': 'actual retained duplicate SOCKET remains live and explicitly table-owned',
            'quarantine-event': 'actual retained event reference remains live and explicitly table-owned',
            'quarantine-binder': 'actual retained Binder reference completes real broker roundtrip',
        }
        if mode in live:
            cases['quarantine-resource-live'] = live[mode]
        return cases
    cases = {
        'rollback-error': 'explicit flag-failure dependency returns actual failure',
        'rollback-retired': 'all unpublished duplicate ownership retired',
        'rollback-handle-parity': 'actual native handle count returns to warm baseline',
        'rollback-foreign-reuse': 'reused fd reads actual sentinel, never stale virtual redirection',
    }
    if mode == 'regular':
        cases.update({
            'posix-preserved': 'internal rollback preserved source POSIX any-close locks',
            'ofd-preserved': 'internal rollback preserved source OFD lock',
            'actual-final-close': 'actual final user close releases POSIX and OFD ownership',
        })
    elif mode == 'device':
        cases['device-live'] = 'original device dispatch remains live'
    elif mode == 'event':
        cases['event-live'] = 'original event object remains live'
    elif mode == 'socket':
        cases['socket-peer'] = 'original actual SOCKET peer receives after rollback'
        cases['socket-noinherit'] = 'socket plain duplicate preserves existing never-inherit policy'
    elif mode == 'binder':
        cases['binder-roundtrip'] = 'original Binder source completes actual broker roundtrip'
    return cases


def run(worker):
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI-only native fixture'
    kinds = ('regular', 'device', 'event', 'socket', 'binder')
    modes = (*kinds, 'retained-nul', *('quarantine-' + kind for kind in kinds),
             'raw-race', 'capacity')
    results = []
    with tempfile.TemporaryDirectory(prefix='gesher-duplicate-') as directory:
        root = Path(directory)
        source, sentinel = root / 'source', root / 'sentinel'
        source.write_bytes(b'x' * 1024)
        sentinel.write_bytes(b'S')
        for mode in modes:
            process = subprocess.Popen([str(worker), mode, str(source), str(sentinel)],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            try:
                output, unused = process.communicate(timeout=60)
                print(output, end='', flush=True)
                assert process.returncode == 0, (mode, process.returncode)
                summaries = re.findall(r'^ACTUAL_DUPLICATE_UNIT assertions=(\d+) failures=(\d+) '
                                       r'scope=actual-resource-dependency-controls atomic-spawn-open$',
                                       output, re.MULTILINE)
                assert len(summaries) == 1, (mode, 'missing/duplicate actual completion')
                assertions, failures = map(int, summaries[0])
                assert assertions > 0 and failures == 0, (mode, summaries)
                cases = required_cases(mode)
                for identifier, label in cases.items():
                    matches = re.findall(r'^PASS duplicate ownership: ' + re.escape(label) + r'$', output, re.MULTILINE)
                    assert len(matches) == 1, (mode, identifier, 'missing/duplicate required actual case')
                if mode == 'raw-race':
                    assert re.search(r'^ACTUAL_RAW_PUBLISHER duplicates=1000 '
                                     r'raw-publications=[1-9][0-9]* errors=0$', output, re.MULTILINE), output
                results.append(dict(mode=mode, assertions=assertions, failures=failures, cases=list(cases)))
                print('ACTUAL_DUPLICATE_ROW ' + json.dumps(results[-1], sort_keys=True), flush=True)
            finally:
                if process.poll() is None:
                    process.kill()
                    try:
                        tail, unused = process.communicate(timeout=5)
                        print(tail, end='', flush=True)
                    except subprocess.TimeoutExpired as error:
                        raise AssertionError('owned duplicate fixture did not terminate within five seconds') from error
                process.stdout.close()
    assert len(results) == 13
    print(f'ACTUAL_DUPLICATE_ORACLE modes={len(results)} failures=0 semantics=bounded-resource-controls', flush=True)
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--worker', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    arguments = parser.parse_args()
    rows = run(arguments.worker.resolve(strict=True))
    arguments.output.write_text(json.dumps(rows, indent=2) + '\n')
