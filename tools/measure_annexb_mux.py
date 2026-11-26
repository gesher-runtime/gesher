#!/usr/bin/env python3
"""Fixed ABBA of previously qualified, ordinary complete-MP4 binaries.

Every child checks every finalized file byte against the independently decoded
baseline output. Input preparation, full-file readback and hashing are outside
the reported open/factory/write/finalize/close/destruction intervals. No fsync,
Windows runtime, device codec, end-to-end app or durable-storage claim is made.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import resource
import statistics
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def child_cpu():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return round((usage.ru_utime + usage.ru_stime) * 1_000_000_000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--fixtures', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--rounds', type=int, choices=range(1, 13), default=6)
    parser.add_argument('--repeat-scale', type=int, choices=(1, 2, 4), default=1,
                        help='Predeclare a fixed multiplier for every steady lifecycle count')
    args = parser.parse_args()
    build, fixtures, out = args.build.resolve(), args.fixtures.resolve(), args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = json.loads((build / 'receipt.json').read_text())
    manifest = json.loads((fixtures / 'manifest.json').read_text())
    assert len(receipt['variants']) == 2 and len(receipt['mux']) == 24
    for variant in ('baseline', 'candidate'):
        hashes = {row['binary_sha256'] for row in receipt['mux'] if row['variant'] == variant}
        assert hashes == {sha(build / f'{variant}-mux')}
    assert not any('sanitize=' in word for command in receipt['commands'] for word in command)
    for name, identity in manifest['inputs'].items():
        assert sha(fixtures / name) == identity['sha256']
    repetitions = {'tiny': 32, 'small': 8, '720p': 2, '1080p': 2, '4k': 1, 'flat': 16,
                   'avcc-control': 2, 'config-sample': 2, 'dense-zero': 2, 'escaped-zero': 2,
                   'all-one': 2, 'dense-prefix': 2}
    repetitions = {name: count * args.repeat_scale for name, count in repetitions.items()}
    assert {case['name'] for case in manifest['cases']} == set(repetitions)
    oracle_rows = {row['case']: row for row in receipt['mux'] if row['variant'] == 'baseline'}
    for name, row in oracle_rows.items():
        assert sha(build / f'baseline-{name}.mp4') == row['output_sha256']
    records = []
    metadata = {'started_utc': now(), 'rounds': args.rounds, 'order': ['baseline', 'candidate', 'candidate', 'baseline'],
                'samples_per_process': 3, 'repetitions': repetitions, 'build_receipt_sha256': sha(build / 'receipt.json'),
                'fixture_manifest_sha256': sha(fixtures / 'manifest.json'), 'build': str(build),
                'fixtures': str(fixtures), 'driver_sha256': sha(__file__),
                'binary_sha256': {variant: sha(build / f'{variant}-mux') for variant in ('baseline', 'candidate')},
                'child_process_cpu_tolerance_ns': 100000, 'records': records}

    def save():
        (out / 'raw.json').write_text(json.dumps(metadata, indent=2) + '\n')

    save()
    for block in range(args.rounds):
        cases = manifest['cases'][block:] + manifest['cases'][:block]
        for case in cases:
            name = case['name']
            for slot, variant in enumerate(metadata['order']):
                tag = f'{block:02d}-{name}-{slot}-{variant}'
                command = [str(build / f'{variant}-mux'), str(fixtures / case['packets']),
                           str(fixtures / case['config']), str(out / f'{tag}.mp4'),
                           str(build / f'baseline-{name}.mp4'), str(repetitions[name]), '3',
                           case['configuration'], 'verify']
                before = child_cpu()
                started = now()
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                ended = now()
                process_cpu = child_cpu() - before
                (out / f'{tag}.stdout').write_text(result.stdout)
                (out / f'{tag}.stderr').write_text(result.stderr)
                record = {'block': block, 'case': name, 'slot': slot, 'variant': variant, 'command': command,
                          'started_utc': started, 'ended_utc': ended, 'returncode': result.returncode,
                          'process_cpu_ns': process_cpu, 'stdout': result.stdout, 'stderr': result.stderr,
                          'stdout_sha256': sha(out / f'{tag}.stdout'), 'stderr_sha256': sha(out / f'{tag}.stderr')}
                records.append(record)
                save()  # Preserve an unsuccessful process too; never retry or filter it.
                assert result.returncode == 0 and result.stderr == '', (tag, result.stderr)
                observations = [json.loads(line) for line in result.stdout.splitlines()]
                assert len(observations) == 4
                for sample, observation in enumerate(observations):
                    assert observation['phase'] == ('first' if sample == 0 else 'steady')
                    assert observation['sample'] == (0 if sample == 0 else sample - 1)
                    assert observation['lifecycles'] == (1 if sample == 0 else repetitions[name])
                    assert observation['cpu_ns'] > 0 and observation['wall_ns'] > 0
                    for key in ('packets_per_file', 'input_bytes_per_file', 'output_bytes_per_file', 'checksum'):
                        assert observation[key] == oracle_rows[name]['observation'][key], (tag, key)
                measured = sum(row['cpu_ns'] for row in observations)
                assert measured <= process_cpu + metadata['child_process_cpu_tolerance_ns'], (tag, measured, process_cpu)
                record['observations'] = observations
                record['measured_cpu_ns'] = measured
                assert not (out / f'{tag}.mp4').exists(), 'child failed to retire output file'
                save()
        print(f'Annex-B complete MP4 ABBA block {block + 1}/{args.rounds}: all 48 processes passed', flush=True)
    metadata['ended_utc'] = now()
    metadata['complete'] = True
    summary = []
    for case in manifest['cases']:
        name = case['name']
        for phase in ('first', 'steady'):
            row = {'case': name, 'phase': phase, 'real_encoded_stream': case['real_encoded_stream']}
            for metric in ('cpu_ns', 'wall_ns'):
                values = {variant: [item[metric] / item['lifecycles'] for record in records
                    if record['variant'] == variant and record['case'] == name
                    for item in record['observations'] if item['phase'] == phase]
                    for variant in ('baseline', 'candidate')}
                medians = {variant: statistics.median(data) for variant, data in values.items()}
                paired = []
                for block in range(args.rounds):
                    means = {variant: statistics.mean(item[metric] / item['lifecycles'] for record in records
                        if record['block'] == block and record['case'] == name and record['variant'] == variant
                        for item in record['observations'] if item['phase'] == phase)
                        for variant in ('baseline', 'candidate')}
                    paired.append(100 * (means['candidate'] / means['baseline'] - 1))
                row[metric] = {'median': medians, 'percent': 100 * (medians['candidate'] / medians['baseline'] - 1),
                               'paired_block_percent': paired,
                               'range': {variant: [min(data), max(data)] for variant, data in values.items()},
                               'sample_counts': {variant: len(data) for variant, data in values.items()}}
            summary.append(row)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    metadata['summary_sha256'] = sha(out / 'summary.json')
    save()
    print(json.dumps({'started_utc': metadata['started_utc'], 'ended_utc': metadata['ended_utc'],
                      'processes': len(records), 'observations': sum(len(row['observations']) for row in records)}), flush=True)


if __name__ == '__main__':
    main()
