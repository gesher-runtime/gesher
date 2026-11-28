#!/usr/bin/env python3
"""Frozen separate-TU UTF conversion experiment; retain all ABBA observations.

Preparation compiles ordinary O2 binaries and distinct allocation-instrumented
callers. Timing never uses the instrumented callers or a sanitizer. Linux
component rates do not establish Windows, whole-application or startup rates.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time
from datetime import datetime, timezone

from test_wide_ascii_contracts import production

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'compat/bionic/locale_bionic.cc'
CASES = ['empty', 'ascii1', 'ascii7', 'ascii32', 'ascii256', 'ascii4096',
         'ascii65536', 'measure4096', 'restart4096', 'legacy4096',
         'chunks4096', 'lifecycle4096', 'mixed4096', 'nonascii4096',
         'invalid4096', 'output0', 'output8', 'source4096']
PREFIX = '''#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "abi_types.h"
namespace gesher::bionic {
using wchar32 = uint32_t;
using wint32 = uint32_t;
constexpr wint32 kWEOF = 0xffffffffu;
constexpr int kEILSEQ = 84, kEINVAL = 22;
constexpr size_t kInvalid = size_t(-1), kIncomplete = size_t(-2);
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def prepare(args):
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command, label):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=120)
        (out / (label + '.stdout')).write_text(result.stdout)
        (out / (label + '.stderr')).write_text(result.stderr)
        commands.append({'command': command, 'exit': result.returncode, 'label': label})
        save(out / 'commands.json', commands)
        if result.returncode:
            raise RuntimeError(f'{label}: {result.returncode}\n{result.stderr[-5000:]}')
        return result

    baseline = subprocess.check_output(['git', 'show', args.baseline_ref + ':' + SOURCE], cwd=ROOT, text=True)
    candidate = (ROOT / SOURCE).read_text()
    (out / 'baseline-source.cc').write_text(baseline)
    (out / 'candidate-source.cc').write_text(candidate)
    run([args.compiler, '--version'], 'compiler-version')
    flags = ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-fno-lto',
             '-I', str(ROOT / 'compat/bionic'), '-I', str(out)]
    # The public type definition is copied exactly into both separately compiled
    # TUs. It is not an aliased byte buffer or a rewritten conversion body.
    exact = production(candidate)
    declaration = exact[:exact.index('\nbool Initial')]
    header = PREFIX + declaration + '''
size_t BIONIC_SYSV Wcsnrtombs(char*, const wchar32**, size_t, size_t, BionicMbstate*);
size_t BIONIC_SYSV Wcsrtombs(char*, const wchar32**, size_t, BionicMbstate*);
size_t BIONIC_SYSV Wcstombs(char*, const wchar32*, size_t);
}
'''
    (out / 'wide_ascii_api.h').write_text(header)
    (out / 'caller.cc').write_bytes((ROOT / 'tests/native/wide_ascii_bench.cc').read_bytes())
    run([args.compiler, *flags, '-c', out / 'caller.cc', '-o', out / 'caller.o'], 'caller-compile')
    run([args.compiler, *flags, '-DWIDE_RESOURCE_COUNTS=1', '-c', out / 'caller.cc',
         '-o', out / 'resource-caller.o'], 'resource-caller-compile')
    resources = []
    for name, source in [('baseline', baseline), ('candidate', candidate)]:
        block = production(source)
        assert block[:block.index('\nbool Initial')] == declaration
        (out / (name + '-actual.cc')).write_text(PREFIX + block + '\n}\n')
        run([args.compiler, *flags, '-fstack-usage', '-c', out / (name + '-actual.cc'),
             '-o', out / (name + '.o')], name + '-compile')
        for resource in [False, True]:
            executable = name + ('-resources' if resource else '')
            caller = 'resource-caller.o' if resource else 'caller.o'
            run([args.compiler, '-fno-lto', out / caller, out / (name + '.o'), '-o', out / executable], executable + '-link')
            if resource:
                for case in CASES:
                    row = json.loads(run([out / executable, case], name + '-resource-' + case).stdout)
                    resources.append({'variant': name, **row})
    save(out / 'resources.json', resources)
    for case in CASES:
        pair = [{k: v for k, v in row.items() if k != 'variant'} for row in resources if row['case'] == case]
        assert len(pair) == 2 and pair[0] == pair[1], case
        assert pair[0]['retained_bytes'] == 0
    symbols = run(['nm', '-C', out / 'caller.o'], 'caller-symbols').stdout
    for function in ['Wcsnrtombs', 'Wcsrtombs', 'Wcstombs']:
        assert any(' U gesher::bionic::' + function + '(' in line for line in symbols.splitlines())
    for name in ['baseline', 'candidate']:
        symbols = run(['nm', '-C', out / (name + '.o')], name + '-symbols').stdout
        assert not any(' U ' in line and any(token in line for token in ['operator new', 'malloc', 'calloc', 'realloc', 'free']) for line in symbols.splitlines())
    files = {p.name: {'size': p.stat().st_size, 'sha256': sha(p)} for p in out.iterdir() if p.is_file()}
    receipt = {'prepared_utc': utc(), 'baseline_ref': args.baseline_ref,
               'candidate_parent': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
               'source_path': SOURCE, 'cases': CASES, 'flags': flags,
               'ordinary_separate_translation_units': True, 'lto': False,
               'files': files, 'commands': commands,
               'boundary': 'Preloaded UTF-32 input; complete API operations, or query/allocate/convert/full-byte-consume/free lifecycle. Input/reference/buffer preparation is outside intervals. First interval is first production call after preparation. Separate process wall includes setup; it is not an app startup measure. Scoped C++ requested/live allocation counts are separate and are not RSS/whole-process heap.'}
    save(out / 'build.json', receipt)
    print('WIDE_BENCH_PREPARED', len(CASES), len(resources))


def execute(args):
    build = args.build_dir.resolve()
    receipt = json.loads((build / 'build.json').read_text())
    for filename, value in receipt['files'].items():
        assert sha(build / filename) == value['sha256'], filename
    assert args.blocks in [1, 6], 'one pilot or the fixed six-block final'
    out = args.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    plan = []
    for block in range(args.blocks):
        cases = CASES[block:] + CASES[:block]
        for case in cases:
            for order, variant in enumerate(['baseline', 'candidate', 'candidate', 'baseline']):
                plan.append({'index': len(plan), 'block': block, 'case': case, 'order': order, 'variant': variant})
    declaration = {'declared_utc': utc(), 'label': args.label, 'blocks': args.blocks,
                   'build_sha256': sha(build / 'build.json'), 'build': receipt,
                   'plan': plan, 'expected_processes': len(plan), 'expected_records': len(plan) * 4,
                   'analysis': 'No deletions or tuning. Each process has first call plus three steady intervals. A process steady rate is sum(duration)/sum(loops); report per-case medians and each paired ABBA block. First-call and full subprocess wall are retained separately. All controls remain.'}
    save(out / 'predeclaration.json', declaration)
    start = utc()
    rows = []
    for item in plan:
        executable = build / item['variant']
        assert sha(executable) == receipt['files'][item['variant']]['sha256']
        before = time.monotonic_ns()
        result = subprocess.run([str(executable), item['case']], text=True, capture_output=True, timeout=120)
        elapsed = time.monotonic_ns() - before
        label = f"{item['index']:04d}-{item['variant']}-{item['case']}"
        (out / (label + '.stdout')).write_text(result.stdout)
        (out / (label + '.stderr')).write_text(result.stderr)
        row = {**item, 'exit': result.returncode, 'process_wall_ns': elapsed,
               'stdout': label + '.stdout', 'stderr': label + '.stderr',
               'records': [json.loads(line) for line in result.stdout.splitlines()] if result.returncode == 0 else []}
        rows.append(row)
        save(out / 'raw.json', {'started_utc': start, 'observed_utc': utc(), 'rows': rows})
        assert result.returncode == 0 and len(row['records']) == 4, label
        assert [x['interval'] for x in row['records']] == [0, 1, 2, 3]
        assert all(x['case'] == item['case'] for x in row['records'])
    save(out / 'receipt.json', {'started_utc': start, 'finished_utc': utc(), 'processes': len(rows),
                              'records': sum(len(x['records']) for x in rows), 'raw_sha256': sha(out / 'raw.json')})
    summarize(out)


def summarize(out):
    declaration = json.loads((out / 'predeclaration.json').read_text())
    raw = json.loads((out / 'raw.json').read_text())['rows']
    assert len(raw) == declaration['expected_processes']
    rows = []

    def value(row, metric, first=False):
        records = row['records'][:1] if first else row['records'][1:]
        return sum(x[metric] for x in records) / sum(x['loops'] for x in records)

    for case in CASES:
        pair = {v: [x for x in raw if x['case'] == case and x['variant'] == v] for v in ['baseline', 'candidate']}
        summary = {'case': case}
        for metric in ['cpu_ns', 'wall_ns']:
            for first in [False, True]:
                key = ('first_' if first else 'steady_') + metric
                a = [value(x, metric, first) for x in pair['baseline']]
                b = [value(x, metric, first) for x in pair['candidate']]
                aa, bb = statistics.median(a), statistics.median(b)
                blocks = []
                for block in range(declaration['blocks']):
                    av = statistics.mean(value(x, metric, first) for x in pair['baseline'] if x['block'] == block)
                    bv = statistics.mean(value(x, metric, first) for x in pair['candidate'] if x['block'] == block)
                    blocks.append(100 * (bv / av - 1))
                summary[key] = {'baseline_median': aa, 'candidate_median': bb,
                                'change_percent': 100 * (bb / aa - 1),
                                'baseline_p95': sorted(a)[math.ceil(.95 * len(a)) - 1],
                                'candidate_p95': sorted(b)[math.ceil(.95 * len(b)) - 1],
                                'paired_block_changes_percent': blocks}
        summary['process_wall_ns'] = {v: {'median': statistics.median(x['process_wall_ns'] for x in pair[v]),
                                         'all': [x['process_wall_ns'] for x in pair[v]]} for v in pair}
        rows.append(summary)
    save(out / 'summary.json', {'source': 'raw.json', 'rows': rows})
    for row in rows:
        print(row['case'], round(row['steady_cpu_ns']['change_percent'], 3), row['steady_cpu_ns']['paired_block_changes_percent'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('--baseline-ref', required=True)
    p.add_argument('--compiler', required=True)
    p.add_argument('--out-dir', required=True, type=Path)
    p = sub.add_parser('run')
    p.add_argument('--build-dir', required=True, type=Path)
    p.add_argument('--out-dir', required=True, type=Path)
    p.add_argument('--blocks', required=True, type=int)
    p.add_argument('--label', required=True)
    args = parser.parse_args()
    if args.command == 'prepare': prepare(args)
    else: execute(args)


if __name__ == '__main__':
    main()
