#!/usr/bin/env python3
"""Verify historical Annex-B/MP4 evidence without executing a timing experiment.

Current production behavior is covered separately by test_annexb_scan.py. This
file deliberately does not label future production edits as the measured source.
"""
from __future__ import annotations
import hashlib
import io
import json
import math
from pathlib import Path, PurePosixPath
import statistics
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CHECKS = 0


def check(condition, label):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(label)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def number(a, b):
    check(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-8), 'recomputed arithmetic')


def p95(values):
    values = sorted(values)
    position = (len(values) - 1) * .95
    low, high = math.floor(position), math.ceil(position)
    return values[low] + (values[high] - values[low]) * (position - low)


def main():
    path = ROOT / 'docs/research/annexb-mp4-2026-11-23.json'
    report = json.loads(path.read_text())
    parts = report['archive']['parts']
    check(isinstance(parts, list) and len(parts) == 2, 'two archive transport parts')
    payloads = []
    for ordinal, part in enumerate(parts, 1):
        relative = PurePosixPath(part['file'])
        check(part['file'] == report['archive']['file'] + f'.part{ordinal}' and
              not relative.is_absolute() and '..' not in relative.parts,
              'ordered safe archive part')
        payload = (path.parent / relative).read_bytes()
        check(len(payload) == part['bytes'], 'archive part length')
        check(sha(payload) == part['sha256'], 'archive part identity')
        payloads.append(payload)
    data = b''.join(payloads)
    check(sha(data) == report['archive']['sha256'], 'archive identity')
    check(len(data) == report['archive']['bytes'], 'archive length')
    blobs = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:xz') as handle:
        for member in handle:
            relative = PurePosixPath(member.name)
            check(member.isfile() and not relative.is_absolute() and '..' not in relative.parts,
                  'safe regular archive member')
            check(member.name not in blobs, 'unique member')
            blobs[member.name] = handle.extractfile(member).read()
    check(set(blobs) == set(report['archive']['members']), 'complete archive manifest')
    for name, identity in report['archive']['members'].items():
        check(len(blobs[name]) == identity['bytes'] and sha(blobs[name]) == identity['sha256'], name)
    summaries = report['analysis']
    cases = ['tiny', 'small', '720p', '1080p', '4k', 'flat', 'avcc-control',
             'config-sample', 'dense-zero', 'escaped-zero', 'all-one', 'dense-prefix']
    repetitions = dict(zip(cases, (32, 8, 2, 2, 1, 16, 2, 2, 2, 2, 2, 2)))
    for label, directory, blocks, scale in (
            ('pilot', 'annexb-pilot-20261123', 1, 1), ('final', 'annexb-final-20261123', 6, 2)):
        prefix = 'evidence/' + directory + '/'
        raw = json.loads(blobs[prefix + 'raw.json'])
        summary = summaries[label]
        check(raw['complete'] and raw['rounds'] == blocks, 'complete predeclared campaign')
        check(raw['order'] == ['baseline', 'candidate', 'candidate', 'baseline'], 'fixed ABBA')
        check(raw['repetitions'] == {name: n * scale for name, n in repetitions.items()}, 'fixed lifecycle counts')
        check(len(raw['records']) == summary['processes'] == 48 * blocks, 'all processes retained')
        check(sha(blobs[prefix + 'raw.json']) == summary['raw_sha256'], 'raw receipt identity')
        check(sha(blobs[prefix + 'summary.json']) == raw['summary_sha256'] == summary['original_summary_sha256'],
              'original summary identity')
        prior_end = raw['started_utc']
        observations = 0
        for index, process in enumerate(raw['records']):
            block, within = divmod(index, 48)
            case_position, slot = divmod(within, 4)
            order = cases[block:] + cases[:block]
            check((process['block'], process['case'], process['slot'], process['variant']) ==
                  (block, order[case_position], slot, raw['order'][slot]), 'actual fixed order')
            check(prior_end <= process['started_utc'] <= process['ended_utc'] <= raw['ended_utc'], 'process time bounds')
            prior_end = process['ended_utc']
            check(process['returncode'] == 0 and process['stderr'] == '', 'no replaced failure')
            tag = f"{block:02d}-{process['case']}-{slot}-{process['variant']}"
            for stream in ('stdout', 'stderr'):
                payload = blobs[prefix + tag + '.' + stream]
                check(payload.decode() == process[stream] and sha(payload) == process[stream + '_sha256'], 'verbatim process output')
            parsed = [json.loads(line) for line in process['stdout'].splitlines()]
            check(parsed == process['observations'] and len(parsed) == 4, 'all first and steady records')
            observations += len(parsed)
            for ordinal, item in enumerate(parsed):
                check(item['phase'] == ('first' if ordinal == 0 else 'steady') and item['sample'] == max(0, ordinal - 1), 'phase ordinal')
                check(item['lifecycles'] == (1 if ordinal == 0 else repetitions[process['case']] * scale), 'complete lifecycle count')
                for field in ('packets_per_file', 'input_bytes_per_file', 'output_bytes_per_file', 'checksum'):
                    check(item[field] == report['outputs'][process['case']][field], 'complete output identity')
            check(sum(item['cpu_ns'] for item in parsed) == process['measured_cpu_ns'] <=
                  process['process_cpu_ns'] + raw['child_process_cpu_tolerance_ns'], 'process CPU interval consistency')
        check(observations == summary['observations'] == 192 * blocks, 'complete observation count')
        check(sum(o['lifecycles'] for p in raw['records'] for o in p['observations']) == summary['complete_mp4_lifecycles'], 'complete file lifecycle count')
        for row in summary['rows']:
            for metric in ('cpu_ns', 'wall_ns'):
                groups = {variant: [o[metric] / o['lifecycles'] for p in raw['records']
                    if p['case'] == row['case'] and p['variant'] == variant for o in p['observations']
                    if o['phase'] == row['phase']] for variant in ('baseline', 'candidate')}
                result = row[metric]
                medians = {v: statistics.median(values) for v, values in groups.items()}
                for variant, values in groups.items():
                    number(medians[variant], result['median'][variant])
                    number(p95(values), result['p95_r7'][variant])
                    check(min(values) == result['minimum'][variant] and max(values) == result['maximum'][variant], 'full tails')
                    check(len(values) == result['observations_per_variant'], 'observation population')
                check(result['independent_processes_per_variant'] == 2 * blocks, 'independent process population')
                number(medians['candidate'] - medians['baseline'], result['delta_ns_per_complete_file'])
                number(100 * (medians['candidate'] / medians['baseline'] - 1), result['percent'])
                for block in range(blocks):
                    pair = {variant: statistics.mean(o[metric] / o['lifecycles'] for p in raw['records']
                        if p['case'] == row['case'] and p['variant'] == variant and p['block'] == block
                        for o in p['observations'] if o['phase'] == row['phase']) for variant in groups}
                    number(100 * (pair['candidate'] / pair['baseline'] - 1), result['paired_block_percent'][block])
    for source, identity in report['measured_inputs'].items():
        check(sha(blobs['inputs/' + source]) == identity, 'frozen measured input ' + source)
    check(report['scope'] == 'historical-host-complete-mp4-component', 'scoped evidence')
    print(json.dumps({'checks': CHECKS, 'members': len(blobs), 'final_processes': 288,
                      'final_observations': 1152, 'pilot_processes': 48, 'pilot_observations': 192,
                      'scope': 'Historical full MP4 component; no Windows/runtime or release-budget acceptance.'}))


if __name__ == '__main__':
    main()
