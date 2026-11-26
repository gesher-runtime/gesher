#!/usr/bin/env python3
"""Verify immutable wide-conversion evidence without running a new benchmark."""
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import statistics
import tarfile

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / 'docs/performance'
STEM = 'wide-ascii-2026-11-23'
ARCHIVE_SHA256 = '0689ee22c8755627aad1d9455962f426dc3266a2a100ff3debbd52b8112661a0'
REPORT_SHA256 = 'b6e927c1ab83ce07bb3bfff5c027f0e36f3fc4a677aa2716a048455b44a027ef'
CANDIDATE_SHA256 = '77b803efa81e0606ea0f927bd46829a059d7c9828f9afc8699d8668512b9e772'
checks = 0


def check(value, description):
    global checks
    checks += 1
    if not value:
        raise AssertionError(description)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def close(a, b, description):
    check(math.isclose(a, b, rel_tol=1e-13, abs_tol=1e-12), description)


def main():
    report_bytes = (DOC / (STEM + '.json')).read_bytes()
    check(digest(report_bytes) == REPORT_SHA256, 'immutable report including adverse rows')
    report = json.loads(report_bytes)
    archive_path = DOC / report['archive']['path']
    archive_bytes = archive_path.read_bytes()
    check(len(archive_bytes) == 5119878 == report['archive']['bytes'], 'complete archive bytes')
    check(digest(archive_bytes) == ARCHIVE_SHA256 == report['archive']['sha256'], 'complete archive digest')
    files = {}
    with tarfile.open(archive_path, 'r:gz') as archive:
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            check(member.isfile() and not path.is_absolute() and '..' not in path.parts, 'regular bounded archive member')
            check(member.name not in files, 'no duplicate archive member')
            files[member.name] = archive.extractfile(member).read()
    check(len(files) == 1455 == report['archive']['members'], 'all retained members')
    manifest = json.loads(files['manifest.json'])
    check(set(manifest) == set(files) - {'manifest.json'}, 'manifest has no omissions/additions')
    for name, value in manifest.items():
        check(len(files[name]) == value['bytes'] and digest(files[name]) == value['sha256'], 'member identity: ' + name)

    def load(name):
        return json.loads(files[name])

    frozen = load('wide-ascii-source-freeze.json')
    check(frozen == report['source'], 'frozen source phase')
    check(frozen['head'] == '9b3947bc6530a84ab8ed96d6e7b645848e2b7aae', 'measured source commit')
    check(frozen['tree'] == '49e265ecac58572fc7aa89b467951716fcc2de98', 'measured source tree')
    check(frozen['parent'] == '73bf018b8c02228ac1e5ad4ce4656296674b07ed', 'baseline commit')
    for name, value in frozen['files'].items():
        data = files['frozen-source/' + name]
        check(digest(data) == value['sha256'], 'frozen source digest: ' + name)
        check(hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() == value['blob'], 'frozen Git blob: ' + name)
        # The preserved workflows are source-phase build context. New evidence
        # steps and unrelated later CI jobs are not measured conversion bodies.
        if not name.startswith('.github/workflows/'):
            check((ROOT / name).read_bytes() == data, 'current measured source: ' + name)
    check(digest(files['wide-ascii-build-v1/candidate-source.cc']) == CANDIDATE_SHA256 == report['candidate_source_sha256'], 'exact candidate locale')
    check(digest(files['wide-ascii-build-v1/baseline-source.cc']) == report['baseline_source_sha256'], 'exact baseline locale')
    check(files['wide-ascii-build-v1/caller.cc'] == files['frozen-source/tests/native/wide_ascii_bench.cc'], 'same opaque caller')
    build = load('wide-ascii-build-v1/build.json')
    check(build == report['build'], 'complete build receipt')
    check(build['ordinary_separate_translation_units'] and build['lto'] is False, 'ordinary separate translation units')
    check('-O2' in build['flags'] and '-fno-lto' in build['flags'] and not any('sanitize' in flag for flag in build['flags']), 'ordinary timing flags')
    for name, value in build['files'].items():
        data = files['wide-ascii-build-v1/' + name]
        check(len(data) == value['size'] and digest(data) == value['sha256'], 'frozen build member: ' + name)
    symbols = files['wide-ascii-build-v1/caller-symbols.stdout'].decode()
    for name in ['Wcsnrtombs', 'Wcsrtombs', 'Wcstombs']:
        check(any(' U gesher::bionic::' + name + '(' in line for line in symbols.splitlines()), 'opaque caller symbol: ' + name)
    check(all(row['exit'] == 0 for row in build['commands']), 'successful build/resource commands')

    resource_rows = load('wide-ascii-build-v1/resources.json')
    check(resource_rows == report['resources'] and len(resource_rows) == 36, 'all resource pairs')
    resource_by_case = {}
    for case in build['cases']:
        pair = [{k: v for k, v in row.items() if k != 'variant'} for row in resource_rows if row['case'] == case]
        check(len(pair) == 2 and pair[0] == pair[1], 'unchanged complete resource pair: ' + case)
        resource = pair[0]
        check(resource['retained_bytes'] == 0 and resource['allocations'] == resource['frees'], 'complete resource retirement: ' + case)
        count, size = (1, 4097) if case == 'lifecycle4096' else (0, 0)
        check(resource['allocations'] == count and resource['requested_bytes'] == size and resource['peak_live_requested_bytes'] == size, 'scoped requested bytes: ' + case)
        resource_by_case[case] = resource

    for label, blocks, process_count in [('pilot', 1, 72), ('final', 6, 432)]:
        prefix = 'wide-ascii-' + label + '-v1/'
        declaration = load(prefix + 'predeclaration.json')
        receipt = load(prefix + 'receipt.json')
        raw = load(prefix + 'raw.json')
        rows = raw['rows']
        check(declaration['build'] == build and declaration['build_sha256'] == digest(files['wide-ascii-build-v1/build.json']), 'predeclared exact binaries')
        check(receipt == report['campaigns'][label]['receipt'], 'complete campaign receipt')
        check(receipt['processes'] == process_count and receipt['records'] == process_count * 4, 'campaign completeness')
        check(declaration['blocks'] == blocks and len(rows) == process_count and len(declaration['plan']) == process_count, 'complete predeclared plan')
        check(digest(files[prefix + 'raw.json']) == receipt['raw_sha256'], 'immutable raw campaign')
        check(digest(files[prefix + 'predeclaration.json']) == report['campaigns'][label]['predeclaration_sha256'], 'immutable predeclaration')
        for row, expected in zip(rows, declaration['plan'], strict=True):
            check(all(row[k] == v for k, v in expected.items()) and row['exit'] == 0, 'exact planned successful process')
            records = row['records']
            check([r['interval'] for r in records] == [0, 1, 2, 3], 'first and three steady intervals retained')
            check([json.loads(line) for line in files[prefix + row['stdout']].decode().splitlines()] == records, 'raw records match process stdout')
            check(files[prefix + row['stderr']] == b'', 'no ignored process stderr')
            check(row['process_wall_ns'] > 0, 'whole subprocess wall retained')
            for record in records:
                check(record['case'] == row['case'] and record['loops'] > 0 and record['cpu_ns'] > 0 and record['wall_ns'] > 0, 'valid complete interval')
                check(record['checksum'] == (resource_by_case[row['case']]['checksum'] * record['loops']) % (1 << 64), 'all-iteration return/source/errno/state/consumer checksum')
            check(records[0]['loops'] == 1, 'actual first production invocation')
        summary = load(prefix + 'summary.json')['rows']
        check(summary == report['campaigns'][label]['summary'], 'full summary including controls/tails')
        check([x['case'] for x in summary] == build['cases'], 'no selected/dropped case')
        for stated in summary:
            selected = [r for r in rows if r['case'] == stated['case']]
            check(len(selected) == blocks * 4, 'all ABBA processes for case')
            for block in range(blocks):
                group = [r for r in selected if r['block'] == block]
                check([r['variant'] for r in group] == ['baseline', 'candidate', 'candidate', 'baseline'], 'exact ABBA order')
            for metric in ['cpu_ns', 'wall_ns']:
                for first in [False, True]:
                    key = ('first_' if first else 'steady_') + metric
                    values = {}
                    for variant in ['baseline', 'candidate']:
                        values[variant] = []
                        for row in selected:
                            if row['variant'] != variant: continue
                            intervals = row['records'][:1] if first else row['records'][1:]
                            values[variant].append((row['block'], sum(x[metric] for x in intervals) / sum(x['loops'] for x in intervals)))
                    a, b = [statistics.median(v for _, v in values[variant]) for variant in ['baseline', 'candidate']]
                    close(stated[key]['baseline_median'], a, 'raw baseline median')
                    close(stated[key]['candidate_median'], b, 'raw candidate median')
                    close(stated[key]['change_percent'], 100 * (b / a - 1), 'raw relative change')
                    for variant in ['baseline', 'candidate']:
                        array = sorted(v for _, v in values[variant])
                        close(stated[key][variant + '_p95'], array[math.ceil(.95 * len(array)) - 1], 'raw nearest-rank p95')
                    for block, stated_change in enumerate(stated[key]['paired_block_changes_percent']):
                        a, b = [statistics.mean(v for k, v in values[variant] if k == block) for variant in ['baseline', 'candidate']]
                        close(stated_change, 100 * (b / a - 1), 'raw paired block including adverse block')
            for variant in ['baseline', 'candidate']:
                durations = [r['process_wall_ns'] for r in selected if r['variant'] == variant]
                check(stated['process_wall_ns'][variant]['all'] == durations, 'all subprocess wall observations')
                close(stated['process_wall_ns'][variant]['median'], statistics.median(durations), 'subprocess wall median')

    for folder, value in report['qualification'].items():
        receipt = load(folder + '/receipt.json')
        check(receipt == value, 'full qualification receipt')
        check('WIDE_ASCII_CONTRACT_PASS checks=8537842' in receipt['result'] and '1748 conversion/state/boundary/LP64 assertions, 0 failures' in receipt['result'], 'actual/pinned positive corpus')
        check(all(x['exit'] == 0 for x in receipt['commands']), 'positive qualification commands')
        check(digest(files[folder + '/host']) == receipt['binary_sha256'], 'qualified executable identity')
    extra = load('wide-ascii-extra-qualification-v2/receipt.json')
    check(extra == report['negative_controls_and_windows_compilation'], 'compiled control and complete-TU receipt')
    check(len(extra['rows']) == 14, 'four full TUs and ten actual-source controls')
    for row in extra['rows'][:4]:
        check(row['exit'] == 0 and '-Werror' in row['command'], 'complete Windows TU with warnings as errors')
    for row in extra['rows'][4:]:
        check(row['compiled'] and row['exit'] != 0 and 'WIDE_ASCII_FAIL ' in row['actual_failure'], 'compiled actual semantic control')
        commands = load('wide-ascii-extra-qualification-v2/' + row['label'] + '/commands.json')
        check(any(x['stdout'] == 'compile.stdout' and x['exit'] == 0 for x in commands), 'negative actually compiles')
        check(any(x['stdout'] == 'run.stdout' and x['exit'] != 0 for x in commands), 'negative actually executes and fails')
    ci = (ROOT / '.github/workflows/ci.yml').read_text()
    check(ci.count('run: python3 tools/test_wide_ascii_contracts.py --sanitize') == 1, 'mandatory actual-source full CI gate')
    check(ci.count('run: python3 tools/test_wide_ascii_performance_evidence.py') == 1, 'mandatory immutable evidence full CI gate')
    print(f'PASS wide ASCII performance evidence: {checks} source/raw/archive/resource/qualification checks')


if __name__ == '__main__':
    main()
