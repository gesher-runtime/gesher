#!/usr/bin/env python3
"""Verify every archived Scandir sample, source identity and reported statistic."""
from collections import Counter
import hashlib
import io
import itertools
import json
import math
from pathlib import Path, PurePosixPath
import random
import re
import statistics
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'docs/research/scandir-stable-sort-2026-11-23.json'
checks = 0

def require(value, label):
    global checks
    checks += 1
    if not value:
        raise AssertionError(label)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def close(actual, expected, label):
    require(math.isclose(actual, expected, rel_tol=1e-11, abs_tol=1e-8), label)

def percentile(values, p):
    values = sorted(values)
    point = (len(values) - 1) * p
    lo = int(point)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (point - lo)

def main():
    report = json.loads(REPORT.read_text())
    blob = (ROOT / report['archive']['path']).read_bytes()
    require(digest(blob) == report['archive']['sha256'], 'archive digest')
    require(len(blob) == report['archive']['bytes'], 'archive size')
    members = {}
    with tarfile.open(fileobj=io.BytesIO(blob), mode='r:gz') as archive:
        for entry in archive:
            path = PurePosixPath(entry.name)
            require(entry.isfile() and not path.is_absolute() and '..' not in path.parts and entry.name not in members, 'regular confined archive member')
            members[entry.name] = archive.extractfile(entry).read()
    require(len(members) == report['archive']['members'], 'archive member count')
    manifest = json.loads(members['manifest.json'])
    require(set(members) == set(manifest['members']) | {'manifest.json'}, 'complete archive manifest')
    for path, expected in manifest['members'].items():
        require(digest(members[path]) == expected['sha256'], 'member digest: ' + path)
        require(len(members[path]) == expected['bytes'], 'member size: ' + path)
    from scandir_consolidation import historical_sources
    source_bytes = historical_sources(ROOT, report, members, 'aaab03e09a33a46b605039c94a6bef87f666308ee501c44c84bc398c19044332')
    for path, expected in report['sources'].items():
        require(digest(source_bytes[path]) == expected, 'current source: ' + path)
        require(digest(members['repo/' + path]) == expected, 'archived source: ' + path)
    current = members['repo/compat/bionic/posix_bionic.cc']
    require(digest(current) == report['candidate_production_sha256'], 'candidate production')
    require(digest(members['inputs/baseline-posix.cc']) == report['baseline_production_sha256'], 'baseline production')
    declaration = json.loads(members['workspace/native-scandir-final-predeclaration.json'])
    execution = json.loads(members['workspace/native-scandir-final-execution.json'])
    require(declaration['written_utc'] < execution['start_utc'] and not execution['terminal_failure'], 'predeclared complete campaign')
    require(digest(members['workspace/native-scandir-final-predeclaration.json']) == execution['predeclaration_sha256'], 'execution declaration identity')
    require(declaration['expected_processes'] == 360 and declaration['expected_observations'] == 1440, 'fixed full final plan')
    for campaign in report['campaigns']:
        summary = json.loads(members[campaign['summary']])
        require(summary == report[campaign['name']], 'report includes complete summary')
        records = []
        for path in campaign['raw']:
            raw = json.loads(members[path])
            require('end_utc' in raw, 'complete block')
            require(raw['variants'] == summary['variants'], 'frozen variants throughout campaign')
            for process in raw['processes']:
                log = members[str(PurePosixPath(path).parent / process['log'])]
                require(digest(log) == process['log_sha256'], 'exact raw process log')
                rows = [json.loads(line) for line in log.decode().splitlines() if line.startswith('{')]
                require(rows == process['rows'], 'every stdout observation retained')
                require(process['returncode'] == 0 and b'SCANDIR_BENCH_COMPLETE owned_directory_removed=true' in log, 'complete owned operation and retirement')
                require([row['sample'] for row in rows] == [0, 1, 2, 3], 'first and all three steady samples')
                for row in rows:
                    require(row['complete_output'] and row['case'] == process['case'] and row['calls'] > 0 and row['cpu_ns'] > 0 and row['wall_ns'] > 0, 'valid complete sample')
                records.append(process)
        require(len(records) == summary['processes'] and sum(len(p['rows']) for p in records) == summary['observations'], 'no missing process or sample')
        expected = Counter(itertools.product(summary['rounds'], summary['cases'], range(4)))
        require(Counter((p['round'], p['case'], p['position']) for p in records) == expected, 'every fixed ABBA position exactly once')
        require(all(p['variant'] == ('baseline', 'candidate', 'candidate', 'baseline')[p['position']] for p in records), 'unchanged ABBA order')
        for variant, data in summary['variants'].items():
            folder = 'workspace/' + Path(data['binary']).parent.name + '/'
            receipt = json.loads(members[folder + 'source.json'])
            require(receipt == data['receipt'], 'exact compiled receipt')
            require(digest(members[folder + 'scandir_bench']) == receipt['binary_sha256'], 'actual ordinary binary')
            require(digest(members[folder + 'scandir_bench.cc']) == receipt['generated_sha256'], 'actual compiled complete caller')
            source = members['inputs/baseline-posix.cc'] if variant == 'baseline' else current
            section = source[source.index(b'struct BionicDirent {'):source.index(b'// --- the *at() calls and other path calls')]
            require(digest(section) == receipt['section_sha256'] and digest(source) == receipt['production_sha256'], 'actual compiled directory source')
            generated = members[folder + 'scandir_bench.cc']
            require(generated.count(section) == 1, 'unique complete production insertion')
            require(digest(generated.replace(section, b'@DIRECTORY_IMPLEMENTATION@')) == receipt['template_sha256'], 'exact original common caller')
        for case, table in summary['table'].items():
            for phase, metric in itertools.product(('first', 'steady'), ('cpu_ns', 'wall_ns')):
                def value(p):
                    rows = p['rows'][:1] if phase == 'first' else p['rows'][1:]
                    return sum(r[metric] for r in rows) / sum(r['calls'] for r in rows)
                cell = table[phase + '_' + metric]
                samples = {v: [value(p) for p in records if p['case'] == case and p['variant'] == v] for v in ('baseline', 'candidate')}
                for variant, values in samples.items():
                    recorded = cell['variants'][variant]
                    require(recorded['values'] == values and recorded['n'] == len(values), 'all process costs retained')
                    for stat, actual in [('median', statistics.median(values)), ('mean', statistics.mean(values)), ('p95', percentile(values, .95)), ('min', min(values)), ('max', max(values))]:
                        close(recorded[stat], actual, 'absolute ' + stat)
                for stat in ('median', 'mean', 'p95'):
                    close(cell['delta_percent'][stat], 100 * (cell['variants']['candidate'][stat] / cell['variants']['baseline'][stat] - 1), 'relative ' + stat)
                paired = []
                for block, saved in zip(summary['rounds'], cell['paired_blocks']):
                    means = {v: statistics.mean(value(p) for p in records if p['case'] == case and p['round'] == block and p['variant'] == v) for v in samples}
                    require(saved['block'] == block, 'block identity')
                    for v in samples: close(saved[v], means[v], 'paired absolute cost')
                    relative = 100 * (means['candidate'] / means['baseline'] - 1)
                    close(saved['delta_percent'], relative, 'paired change')
                    paired.append(relative)
                close(cell['paired_mean_delta_percent'], statistics.mean(paired), 'paired mean')
                rng = random.Random(178)
                boot = [statistics.mean(rng.choices(paired, k=len(paired))) for _ in range(10000)]
                for saved, actual in zip(cell['paired_bootstrap95_percent'], [percentile(boot, .025), percentile(boot, .975)]):
                    close(saved, actual, 'descriptive paired bootstrap')
    for variant, saved in report['resources'].items():
        log = members['workspace/native-scandir-final-' + ('baseline' if variant == 'baseline' else 'contracts') + '.log'].decode()
        observed = [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)} for line in log.splitlines() if line.startswith('SCANDIR_CASE ')]
        require(observed == saved and len(observed) == 62, 'complete actual work/storage rows')
    for control in json.loads(members['workspace/native-scandir-final-controls/negative-controls.json']):
        path = 'workspace/native-scandir-final-controls/negative-controls/' + control['name'] + '/driver.log'
        require(control['returncode'] != 0 and digest(members[path]) == control['log_sha256'], 'specific compiled negative identity')
        require(('FAIL SCANDIR_SORT ' + control['assertion']).encode() in members[path], 'specific negative assertion')
    require(len(json.loads(members['workspace/native-scandir-final-controls/negative-controls.json'])) == 9, 'all nine controls retained')
    print(f'SCANDIR_REPORT_PASS checks={checks} archive_members={len(members)} final_processes=360 pilot_processes=60 final_observations=1440 pilot_observations=240')

if __name__ == '__main__':
    main()
