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
                if mode == 'raw-race':
                    assert re.search(r'^ACTUAL_RAW_PUBLISHER duplicates=1000 '
                                     r'raw-publications=[1-9][0-9]* errors=0$', output, re.MULTILINE), output
                results.append(dict(mode=mode, assertions=assertions, failures=failures))
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
