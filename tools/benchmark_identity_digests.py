#!/usr/bin/env python3
"""ABBA comparisons of complete production identity calls and actual shortcut file lifecycles.

Compile both trees first with test_identity_digests.py --compile-only --work-dir.
Each case runs in its own fresh JVM. All complete results and retirement markers
must validate before any process receipt is accepted; failed receipts are retained.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

CASES = {
    'key-tiny': (20000, 5000), 'key-unicode': (20000, 5000), 'key-large': (10000, 2000),
    'signed-one': (20000, 5000), 'signed-four': (20000, 5000), 'signed-empty': (20000, 5000),
    'unsigned-tiny': (2000, 500), 'unsigned-splits': (1000, 100), 'unsigned-large': (128, 16),
    'appops-new-owner': (5000, 512), 'appops-cached': (250000, 100000),
    'appops-unsigned': (250000, 100000), 'shortcut-publication': (64, 32),
}


def pattern(length: int, seed: int) -> bytes:
    return bytes((i * 73 + seed * 29) & 255 for i in range(length))


def expected(name: str) -> str:
    if name.startswith('key-'):
        value = '' if name == 'key-tiny' else 'משימה/📞"\0id' if name == 'key-unicode' else 'x' * 8192
        return 'launch fixture.identity --shortcut ' + hashlib.sha256(('fixture.identity\0' + value).encode()).hexdigest()
    if name.startswith('signed-'):
        count = 0 if name == 'signed-empty' else 4 if name == 'signed-four' else 1
        framed = b''.join(struct.pack('>I', 1024) + pattern(1024, i + 1) for i in range(count))
        return 'cert:' + hashlib.sha256(framed).hexdigest()
    if name.startswith('unsigned-'):
        size = 257 if name == 'unsigned-tiny' else 1048576 if name == 'unsigned-large' else 65537
        files = [('base.apk', pattern(size, 9))]
        if name == 'unsigned-splits': files += [('split_a.apk', pattern(31, 2)), ('split_z.apk', pattern(8193, 7))]
        framed = b''.join(struct.pack('>I', len(n.encode())) + n.encode() + struct.pack('>Q', len(b)) + b for n, b in files)
        return 'apk:' + hashlib.sha256(framed).hexdigest()
    if name == 'appops-unsigned': return '<null>'
    if name.startswith('appops-'):
        return ':'.join(sorted([hashlib.sha256(pattern(1024, 1)).hexdigest(), hashlib.sha256(pattern(257, 2)).hexdigest()]))
    if name == 'shortcut-publication': return 'cert:identity-publication-fixture'
    raise ValueError(name)


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dir', type=Path, required=True)
    parser.add_argument('--candidate-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=1)
    parser.add_argument('--samples', type=int, default=3)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    args = parser.parse_args()
    if args.output.exists(): parser.error('Use a new output directory; previous observations are never overwritten')
    args.output.mkdir(parents=True)
    variants = {name: json.loads((path / 'compiled.json').read_text()) for name, path in
                [('baseline', args.baseline_dir), ('candidate', args.candidate_dir)]}
    dataset = {'started_utc': now(), 'variants': variants, 'cases': args.cases,
               'rounds': args.rounds, 'samples': args.samples, 'processes': []}
    def save():
        (args.output / 'results.json').write_text(json.dumps(dataset, indent=2, ensure_ascii=False) + '\n')
    save()
    for block in range(args.rounds):
        for position, variant in enumerate(('baseline', 'candidate', 'candidate', 'baseline')):
            for case in args.cases:
                stem = f'{block}-{position}-{variant}-{case}'
                work = args.output / stem; work.mkdir()
                spec = variants[variant]; warmup, iterations = CASES[case]
                environment = os.environ.copy(); environment.update(spec['environment'])
                environment['ANDROID_DATA'] = str(work / 'data')
                environment.pop('GESHER_BINDER_UID', None)
                command = spec['command'] + [case, str(work / 'inputs'), expected(case),
                                             str(warmup), str(iterations), str(args.samples)]
                record = {'round': block, 'position': position, 'variant': variant, 'case': case,
                          'started_utc': now(), 'command': command, 'log': stem + '/run.log'}
                log = work / 'run.log'
                with log.open('w') as output:
                    result = subprocess.run(command, env=environment, stdout=output,
                                            stderr=subprocess.STDOUT, timeout=90)
                text = log.read_text(); record['finished_utc'] = now(); record['exit_code'] = result.returncode
                record['log_sha256'] = hashlib.sha256(log.read_bytes()).hexdigest()
                record['observations'] = [json.loads(line.removeprefix('IDENTITY_DIGEST_SAMPLE '))
                                           for line in text.splitlines() if line.startswith('IDENTITY_DIGEST_SAMPLE ')]
                dataset['processes'].append(record); save()
                assert result.returncode == 0 and 'IDENTITY_DIGEST_COMPLETE outputs_validated=true owned_inputs_retired=true' in text, str(log)
                assert len(record['observations']) == 1 + args.samples, str(log)
                assert all(row['case'] == case for row in record['observations']), str(log)
                print(stem + ' passed', flush=True)
    dataset['finished_utc'] = now(); save()
    print(json.dumps({'started_utc': dataset['started_utc'], 'finished_utc': dataset['finished_utc'],
                      'processes': len(dataset['processes']), 'observations': sum(len(p['observations']) for p in dataset['processes'])}))


if __name__ == '__main__': main()
