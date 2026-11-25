#!/usr/bin/env python3
"""Authenticate the complete frozen URI evidence and its current source boundary."""
from __future__ import annotations

import hashlib
import itertools
import json
import math
from pathlib import Path
import statistics
import tarfile

ROOT = Path(__file__).resolve().parents[1]
METRICS = ('caller_cpu_ns', 'elapsed_ns', 'caller_allocated_bytes', 'process_cpu_ns')
CHECKS = 0


def require(value: bool, message: str) -> None:
    global CHECKS
    if not value:
        raise AssertionError(message)
    CHECKS += 1


def sha(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def close(actual: float, expected: float, label: str) -> None:
    require(math.isclose(actual, expected, rel_tol=1e-10, abs_tol=1e-8), label)


def percentile(values: list[float], percent: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100
    lower = math.floor(position)
    upper = math.ceil(position)
    return ordered[lower] + (position - lower) * (ordered[upper] - ordered[lower])


def main() -> None:
    report = json.loads((ROOT / 'docs/research/uri-grant-retirement-2026-11-23.json').read_text())
    archive = ROOT / report['archive']['path']
    require(archive.stat().st_size == report['archive']['bytes'], 'Archive byte length changed')
    require(sha(archive.read_bytes()) == report['archive']['sha256'], 'Archive digest changed')
    members: dict[str, bytes] = {}
    with tarfile.open(archive, 'r:gz') as bundle:
        for item in bundle.getmembers():
            require(item.isfile(), 'Unexpected non-file evidence member')
            require(not item.name.startswith('/') and '..' not in Path(item.name).parts,
                    'Unsafe evidence path')
            require(item.name not in members, 'Duplicate evidence member')
            stream = bundle.extractfile(item)
            require(stream is not None, 'Evidence member unavailable')
            members[item.name] = stream.read()
    require(len(members) == report['archive']['members'], 'Evidence census changed')
    inventory = json.loads(members['inventory.json'])
    require(set(inventory) == set(members) - {'inventory.json'}, 'Incomplete evidence inventory')
    for name, expected in inventory.items():
        require(len(members[name]) == expected['bytes'], 'Evidence bytes: ' + name)
        require(sha(members[name]) == expected['sha256'], 'Evidence SHA256: ' + name)

    def read(name: str):
        return json.loads(members[name])

    for path, expected in report['integrated_sources'].items():
        require(sha((ROOT / path).read_bytes()) == expected, 'Current source identity changed: ' + path)
    require(sha((ROOT / report['markdown']['path']).read_bytes()) == report['markdown']['sha256'],
            'Narrative report identity changed')
    declaration = read('uri-grants-final-combined/declaration.json')
    plan = read('uri-grants-final-predeclaration.json')
    records = read('uri-grants-final-combined/receipts.json')
    completion = read('uri-grants-final-combined/completion.json')
    analysis = read('uri-grants-final-combined/analysis.json')
    require(len(records) == 312 and completion['processes'] == 312
            and completion['records'] == 1248 and completion['all_complete'], 'Incomplete fixed campaign')
    require(report['statistics'] == analysis, 'Published statistics differ from raw evidence analysis')
    require(plan['processes'] == 312 and plan['records'] == 1248 and plan['rounds'] == 6,
            'Changed predeclared campaign')
    require(list(plan['cases']) == declaration['cases'], 'Case order changed')
    require(declaration['order'] == ['baseline', 'candidate', 'candidate', 'baseline'], 'ABBA order changed')
    require(plan['declared_utc'] < completion['started_utc'], 'Plan was not declared before timing')
    for block in range(1, 7):
        dec = read(f'uri-grants-final-block{block}/declaration.json')
        end = read(f'uri-grants-final-block{block}/completion.json')
        require(dec['compiled'] == declaration['compiled'], 'Class/source identity changed between blocks')
        require(dec['script_sha256'] == declaration['script_sha256'], 'Driver changed between blocks')
        require(end == completion['intervals'][block - 1], 'Block interval mismatch')
        require(end['all_complete'] and end['processes'] == 52 and end['records'] == 208,
                'Incomplete block')
    for variant in ('baseline', 'candidate'):
        compiled = read(f'uri-grants-{variant}-final/compiled.json')
        require(compiled == declaration['compiled'][variant], 'Frozen compiled receipt changed')
        require(not compiled['count_work'] and not compiled['mutation'], 'Instrumented timing classes')
        for name, digest in compiled['classes'].items():
            require(sha(members[f'uri-grants-{variant}-final/classes/{name}']) == digest,
                    'Frozen class changed: ' + name)
        prefix = f'uri-grants-{variant}-final/classes/'
        require({name.removeprefix(prefix) for name in members if name.startswith(prefix)}
                == set(compiled['classes']), 'Unexpected frozen class')
        require(sha(members[f'uri-grants-{variant}-final/liburi_retirement_host.so'])
                == compiled['native_library_sha256'], 'Native identity adapter changed')
        for path, digest in compiled['original_production'].items():
            require(sha(members[f'uri-grants-{variant}-final/production/{Path(path).name}']) == digest,
                    'Frozen production changed: ' + path)
        key = f'uri-grants-{variant}-build/gesher-app-classes.jar'
        require(read('build-artifact-identities.json')[key]['sha256'] == compiled['app_classes_sha256'],
                'Complete app dependency mismatch')
    candidate = declaration['compiled']['candidate']['production_and_fixtures']
    baseline = declaration['compiled']['baseline']['production_and_fixtures']
    require(set(candidate) == set(baseline), 'Asymmetric compiled source inventory')
    require({path for path in candidate if candidate[path] != baseline[path]}
            == {'app/java/com/gesher/server/PackageManagerService.java',
                'app/java/com/gesher/server/UriGrants.java'}, 'Unexpected measured source delta')
    for path, digest in candidate.items():
        require(sha(members['frozen-candidate-inputs/' + path]) == digest,
                'Frozen complete source/fixture mismatch: ' + path)

    for index, run in enumerate(records):
        block, local = divmod(index, 52)
        case = declaration['cases'][local // 4]
        position = local % 4
        require(run['round'] == block and run['index'] == local
                and run['abba_position'] == position and run['case'] == case
                and run['variant'] == declaration['order'][position], 'Fixed schedule changed')
        require(run['exit'] == 0 and run['complete'], 'Failure or incomplete run in timing')
        require(not any('PrintCompilation' in v or 'javaagent' in v for v in run['command']),
                'Diagnostic observer in ordinary timing')
        stdout = members[run['log']]
        require(sha(stdout) == run['log_sha256'], 'Raw process log changed')
        actual = [json.loads(line.split(' ', 1)[1]) for line in stdout.decode().splitlines()
                  if line.startswith('URI_RETIREMENT_SAMPLE ')]
        require(actual == run['records'] and len(actual) == 4, 'Raw observation/receipt mismatch')
        require('complete_results=true real_owner_retirement=true reference_inside_timing=false'
                in stdout.decode(), 'Missing complete output/retirement marker')
        for sample, observation in enumerate(actual):
            require(observation['case'] == case and observation['phase'] == ('first' if sample == 0 else 'steady')
                    and observation['sample'] == (0 if sample == 0 else sample - 1), 'Changed phase')
            operations = 1 if sample == 0 else plan['cases'][case]['operations_per_sample']
            require(observation['operations'] == operations, 'Changed operation count')
            expected = (int(case.removeprefix('owned_cycle')) if case.startswith('owned_cycle')
                        else 0 if case in ('missing128', 'empty_control') else 1)
            require(observation['checksum'] == expected * operations, 'Incorrect complete operation output')
            require(all(observation[metric] >= 0 for metric in METRICS), 'Unavailable metric treated as data')

    # Three correlated intervals are aggregated within each JVM before any
    # independent-JVM or six-block statistic is calculated.
    for row in analysis['rows']:
        case = row['case']
        for variant in ('baseline', 'candidate'):
            runs = [run for run in records if run['case'] == case and run['variant'] == variant]
            require(len(runs) == 12, 'Wrong independent JVM count')
            for phase in ('first', 'steady'):
                costs = []
                for run in runs:
                    samples = [sample for sample in run['records'] if sample['phase'] == phase]
                    count = sum(sample['operations'] for sample in samples)
                    costs.append({metric: sum(sample[metric] for sample in samples) / count for metric in METRICS})
                expected = row['variants'][variant][phase]
                require(costs == expected['jvm_costs'] and expected['independent_jvms'] == 12,
                        'Independent-JVM costs changed')
                for metric in METRICS:
                    values = [cost[metric] for cost in costs]
                    close(expected['medians'][metric], statistics.median(values), 'Median ' + case)
                    close(expected['descriptive_p95'][metric], percentile(values, 95), 'P95 ' + case)
        for phase in ('first', 'steady'):
            for metric in METRICS:
                b = row['variants']['baseline'][phase]['medians'][metric]
                c = row['variants']['candidate'][phase]['medians'][metric]
                if b == 0:
                    require(row['change_pct'][phase][metric] is None, 'Zero denominator percentage')
                else:
                    close(row['change_pct'][phase][metric], 100 * (c / b - 1), 'Change ' + case)
        for block in range(6):
            for variant in ('baseline', 'candidate'):
                runs = [run for run in records if run['case'] == case
                        and run['variant'] == variant and run['round'] == block]
                require(len(runs) == 2, 'Missing paired JVM')
                for phase in ('first', 'steady'):
                    for metric in METRICS:
                        values = []
                        for run in runs:
                            samples = [sample for sample in run['records'] if sample['phase'] == phase]
                            values.append(sum(sample[metric] for sample in samples)
                                          / sum(sample['operations'] for sample in samples))
                        close(row['paired_blocks'][block][variant][phase][metric], statistics.mean(values),
                              'Paired block mean ' + case)
        for metric in METRICS[:3]:
            b = [block['baseline']['steady'][metric] for block in row['paired_blocks']]
            c = [block['candidate']['steady'][metric] for block in row['paired_blocks']]
            expected = row['paired_summary'][metric]
            close(expected['pooled_change_pct'], 100 * (statistics.mean(c) / statistics.mean(b) - 1),
                  'Pooled change ' + case)
            effects = [100 * (sum(c[i] for i in indices) / sum(b[i] for i in indices) - 1)
                       for indices in itertools.product(range(6), repeat=6)]
            require(expected['replicates'] == len(effects) == 46656 and expected['independent_blocks'] == 6,
                    'Bootstrap independence changed')
            close(expected['exact_paired_bootstrap_95_pct'][0], percentile(effects, 2.5), 'Bootstrap lower ' + case)
            close(expected['exact_paired_bootstrap_95_pct'][1], percentile(effects, 97.5), 'Bootstrap upper ' + case)

    for control in read('uri-grants-mutants/receipts.json'):
        name = 'uri-grants-mutants/' + Path(control['log']).name
        require(control['returncode'] != 0 and sha(members[name]) == control['log_sha256'], 'Mutation identity')
        require(control['expected_service_assertion'] in members[name].decode(), 'Mutation missed designated assertion')
    for validation in report['validation']:
        content = members[validation['log']]
        require(sha(content) == validation['sha256'], 'Validation log identity')
        for marker in validation['required_markers']:
            require(marker in content.decode(), 'Missing validation marker: ' + marker)
    require(report['release_budget_accepted'] is False and report['windows_art_performance_measured'] is False,
            'Unsupported performance acceptance')
    print(f'URI retirement evidence passed: {CHECKS} checks; all 312 JVMs/1248 records, '
          'first/steady/paired/tails, complete archive/classes and current-source identity; '
          'release budget and Windows/ART acceptance remain open.')


if __name__ == '__main__':
    main()
