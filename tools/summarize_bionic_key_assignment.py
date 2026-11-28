#!/usr/bin/env python3
"""Verify preserved key-assignment receipts and summarize independent processes."""
import argparse
import datetime
import hashlib
import json
import random
import statistics
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc(ns):
    return datetime.datetime.fromtimestamp(ns / 1e9, datetime.timezone.utc).isoformat()


def change(before, after):
    return 100 * (after / before - 1)


def summarize(paths):
    campaigns, all_processes, counted = [], [], []
    builds = None
    worker_receipts = 0
    checks = 0
    for block, path in enumerate(paths):
        data = json.loads(path.read_text())
        if builds is None:
            builds = data['builds']
        assert data['builds'] == builds
        assert data['method']['order'] == 'ABBA'
        assert data['method']['warmup_samples'] == [0, 1]
        cases = data['method']['cases']
        assert len(cases) == 12 and len(set(cases)) == 12
        assert len(data['processes']) == 4 * len(cases) * data['method']['rounds']
        assert len(data['counted_processes']) == 2
        for instrumented, name in [(False, 'processes'), (True, 'counted_processes')]:
            for index, process in enumerate(data[name]):
                assert process['sequence'] == index and process['returncode'] == 0
                suffix = 'counted' if instrumented else 'timed'
                log = path.with_name(f'{path.stem}-{suffix}-{index:02d}-{process["variant"]}.log')
                assert digest(log) == process['log_sha256']
                rows = [json.loads(line) for line in log.read_text().splitlines() if line.startswith('{')]
                assert rows == process['observations']
                if instrumented:
                    assert [row['case'] for row in rows] == cases
                else:
                    expected_case = cases[(index // 4) % len(cases)]
                    assert process['case'] == expected_case
                    assert process['variant'] == ['baseline', 'candidate', 'candidate', 'baseline'][index % 4]
                    assert process['round'] == index // (4 * len(cases))
                    expected_samples = 1 if expected_case == 'cold-single' else data['method']['samples_per_case']
                    assert [row['sample'] for row in rows] == list(range(expected_samples))
                for row in rows:
                    assert row['instrumented'] == instrumented
                    assert row['cpu_consistent'] and row['cpu_ns'] >= row['main_cpu_ns'] + row['worker_cpu_ns']
                    assert sum(row['worker_cpu_samples_ns']) == row['worker_cpu_ns']
                    assert row['global_array_bytes'] == 3072 and row['thread_array_bytes'] == 2048
                    if instrumented:
                        expected = 2 * row['created'] + row['exits']
                        if process['variant'] == 'baseline':
                            expected += row['sets']
                        assert row['registry_locks'] == expected
                    else:
                        assert row['registry_locks'] == 0 and row['case'] == expected_case
                    worker_receipts += len(row['worker_cpu_samples_ns'])
                    checks += 8
                process = dict(process, campaign=block)
                (counted if instrumented else all_processes).append(process)
                checks += 7
        campaigns.append({
            'path': str(path), 'sha256': digest(path), 'method': data['method'],
            'started_utc': utc(data['processes'][0]['started_unix_ns']),
            'ended_utc': utc(data['processes'][-1]['ended_unix_ns']),
            'processes': len(data['processes']),
            'observations': sum(len(p['observations']) for p in data['processes']),
            'counted_observations': sum(len(p['observations']) for p in data['counted_processes']),
        })
    results = {}
    for case in cases:
        result = {'first': {}, 'steady': {}}
        for stage in ['first', 'steady']:
            for metric in ['cpu_ns', 'wall_ns']:
                values, by_pair = {}, {}
                for variant in ['baseline', 'candidate']:
                    values[variant] = []
                    for p in all_processes:
                        if p['case'] != case or p['variant'] != variant:
                            continue
                        rows = p['observations']
                        selected = rows[:1] if stage == 'first' or case == 'cold-single' else rows[2:]
                        assert selected
                        value = statistics.median(row[metric] for row in selected)
                        values[variant].append(value)
                        pair = (p['campaign'], p['round'])
                        by_pair.setdefault(pair, {}).setdefault(variant, []).append(value)
                before, after = [statistics.median(values[v]) for v in ['baseline', 'candidate']]
                pairs = []
                for pair, observations in sorted(by_pair.items()):
                    assert len(observations['baseline']) == len(observations['candidate']) == 2
                    b, a = [statistics.median(observations[v]) for v in ['baseline', 'candidate']]
                    pairs.append({'campaign': pair[0], 'round': pair[1],
                                  'baseline_ns': b, 'candidate_ns': a, 'change_percent': change(b, a)})
                rng = random.Random(20261123)
                pair_deltas = [p['change_percent'] for p in pairs]
                samples = sorted(statistics.median(rng.choices(pair_deltas, k=len(pair_deltas)))
                                 for _ in range(4096))
                result[stage][metric] = {
                    'process_medians_ns': values, 'baseline_ns': before, 'candidate_ns': after,
                    'change_percent': change(before, after), 'absolute_change_ns': after - before,
                    'paired_rounds': pairs, 'paired_median_percent': statistics.median(pair_deltas),
                    'paired_bootstrap_median_95_percent': [samples[102], samples[3993]],
                    'bootstrap_caveat': 'Descriptive resampling of a small fixed set of ABBA rounds, not application or scheduler generalization.',
                }
        rows = {v: [row for p in counted if p['variant'] == v
                    for row in p['observations'] if row['case'] == case]
                for v in ['baseline', 'candidate']}
        fields = ['verified', 'sets', 'reads', 'exits', 'created', 'global_array_bytes', 'thread_array_bytes']
        reference = {field: rows['baseline'][0][field] for field in fields}
        for variant in rows:
            assert all({field: row[field] for field in fields} == reference for row in rows[variant])
            assert len({row['registry_locks'] for row in rows[variant]}) == 1
        result['counted_work'] = dict(reference,
            baseline_registry_locks=rows['baseline'][0]['registry_locks'],
            candidate_registry_locks=rows['candidate'][0]['registry_locks'])
        results[case] = result
    return {'schema': 1, 'method': 'Median of within-process steady medians, with samples 0/1 retained as warmups; first sample separately retained. Cold-single runs only once per fresh process.',
            'campaigns': campaigns, 'builds': builds, 'results': results,
            'timed_processes': len(all_processes),
            'timed_observations': sum(len(p['observations']) for p in all_processes),
            'counted_observations': sum(len(p['observations']) for p in counted),
            'worker_cpu_receipts': worker_receipts, 'verification_assertions': checks}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = summarize(args.raw)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ['timed_processes', 'timed_observations', 'counted_observations',
                                          'worker_cpu_receipts', 'verification_assertions']}))
    for case, row in result['results'].items():
        cpu, wall = row['steady']['cpu_ns'], row['steady']['wall_ns']
        first = row['first']['cpu_ns']
        print(f'{case}: CPU {cpu["change_percent"]:+.3f}%; wall {wall["change_percent"]:+.3f}%; first CPU {first["change_percent"]:+.3f}%')
