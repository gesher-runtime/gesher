#!/usr/bin/env python3
"""Ordinary complete-Track measurements, retaining all first/control results.

The actual production and common driver are separate objects with normal
allocators and no coefficient counters. Each fresh process measures its first
complete Track lifecycle after caller PCM/output-buffer preparation, then
repeated complete lifecycles. Construction, all writes/mixes/output hashes,
final state checks, Release and destruction are inside the clocks. The explicit
Engine attachment/callback/clock transport is not WASAPI or an ART application.
Build/smoke separately before reserving a quiet ABBA measurement interval.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import os
import shlex
import platform

from test_audio_resampler_weights import ROOT, BASELINE, production
CASES = [
    ('mono_44100_stream', ['1', '44100', '48000', '14400', '1', '1']),
    ('stereo_44100_stream', ['2', '44100', '48000', '14400', '1', '1']),
    ('stereo_96000_stream', ['2', '96000', '48000', '14400', '1', '1']),
    ('stereo_half_speed_stream', ['2', '48000', '48000', '14400', '.5', '1']),
    ('stereo_fast_static', ['2', '48000', '48000', '14400', '1.5', '0']),
    ('stereo_unity_stream', ['2', '48000', '48000', '14400', '1', '1']),
    ('eight_44100_stream', ['8', '44100', '48000', '14400', '1', '1']),
    ('stereo_short_stream', ['2', '44100', '48000', '64', '1', '1']),
    ('mono_one_static', ['1', '44100', '48000', '1', '1', '0']),
    ('stereo_192000_stream', ['2', '192000', '48000', '14400', '1', '1']),
    ('stereo_wide_fallback', ['2', '288000', '48000', '14400', '1', '1']),
    ('no_read_static', ['2', '48000', '48000', '0', '1', '0']),
]


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def build(args):
    work = args.output_dir
    media = ROOT / 'tests/media'
    compiler = shlex.split(args.cxx)
    driver = media / 'audio_resampler_weights_bench.cc'
    header = media / 'audio_resampler_weights_bench.h'
    adapter = media / 'audio_resampler_weights_adapter.cc.in'
    common_object = work / 'driver.o'
    flags = ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-fno-exceptions', '-pthread', '-I' + str(media)]
    receipt = {'baseline': args.compare_ref, 'host': platform.platform(),
               'compiler': subprocess.check_output(compiler + ['--version'], text=True),
               'scope': 'Complete actual Track with explicit Engine/callback/clock transport; ordinary CV/allocators, no work counters',
               'commands': [], 'inputs': {}}

    def execute(command):
        result = subprocess.run(command, text=True, capture_output=True, timeout=120)
        receipt['commands'].append({'command': command, 'returncode': result.returncode,
                                    'stdout': result.stdout, 'stderr': result.stderr})
        (work / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if result.returncode: raise RuntimeError(result.stderr)

    execute(compiler + flags + ['-c', str(driver), '-o', str(common_object)])
    for label, ref in [('baseline', args.compare_ref), ('candidate', None)]:
        cpp = work / (label + '.cc')
        cpp.write_text(production(ref, False) + adapter.read_text())
        obj = work / (label + '.o')
        binary = work / label
        execute(compiler + flags + ['-c', str(cpp), '-o', str(obj)])
        execute(compiler + flags + [str(common_object), str(obj), '-o', str(binary)])
        for path in [cpp, obj, common_object, binary, driver, header, adapter]:
            receipt['inputs'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ['compat/media/audio/engine.cc', 'compat/media/audio/engine.h',
                 'tests/media/audio_state_host.cc.in', 'tools/test_audio_state_contracts.py',
                 'tools/test_audio_resampler_weights.py', 'tools/benchmark_audio_resampler_weights.py']:
        path = ROOT / name
        receipt['inputs'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    (work / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--build-only', action='store_true')
    p.add_argument('--run-only', action='store_true')
    p.add_argument('--compare-ref', default=BASELINE)
    p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    p.add_argument('--blocks', type=int, default=1)
    p.add_argument('--cycles', type=int, default=8)
    p.add_argument('--smoke', action='store_true')
    args = p.parse_args()
    if args.blocks < 1 or args.cycles < 1: p.error('positive blocks and cycles required')
    if args.build_only and args.run_only: p.error('choose either build-only or run-only')
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if not args.run_only: build(args)
    if args.build_only: return
    if args.output is None: p.error('measurement output is required')
    if args.output.exists(): p.error('refusing to overwrite an earlier measurement')
    BIN = args.output_dir
    receipt = json.loads((BIN / 'build.json').read_text())
    for path, digest in receipt['inputs'].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest, path
    payload = {'start_utc': timestamp(), 'scope': 'actual Track construction/input/complete mix/end/Release/destruction plus all-output hash; input and output buffer preparation excluded',
               'mode': 'smoke-under-uncontrolled-load' if args.smoke else 'ordinary-ABBA',
               'build': receipt, 'processes': []}
    identities = {}
    for block in range(args.blocks):
        for name, case in CASES[block % len(CASES):] + CASES[:block % len(CASES)]:
            order = ['baseline', 'candidate'] if args.smoke else ['baseline', 'candidate', 'candidate', 'baseline']
            for slot, variant in enumerate(order):
                command = [str(BIN / variant), *case, str(args.cycles), '128']
                started = timestamp()
                result = subprocess.run(command, text=True, capture_output=True, timeout=60)
                record = {'block': block, 'slot': slot, 'case': name, 'variant': variant,
                          'start_utc': started, 'end_utc': timestamp(), 'command': command,
                          'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
                payload['processes'].append(record)
                args.output.write_text(json.dumps(payload, indent=2) + '\n')
                assert result.returncode == 0 and not result.stderr, record
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                assert [x['phase'] for x in rows] == ['first', 'steady'], record
                record['rows'] = rows
                for row in rows:
                    assert row['process_ns'] >= row['cpu_ns'], row
                    key = (name, row['phase'])
                    identity = {k: v for k, v in row.items() if k not in ['cpu_ns', 'process_ns', 'wall_ns']}
                    if key in identities:
                        assert identities[key] == identity, (key, identity, identities[key])
                    else:
                        identities[key] = identity
                print(block, name, slot, variant, 'PASS', flush=True)
    payload['end_utc'] = timestamp()
    payload['binary_hashes'] = {label: hashlib.sha256((BIN / label).read_bytes()).hexdigest()
                                for label in ['baseline', 'candidate']}
    summaries = []
    for name, _ in CASES:
        for phase in ['first', 'steady']:
            row = {'case': name, 'phase': phase}
            for metric in ['cpu_ns', 'process_ns', 'wall_ns']:
                values = {variant: [r[metric] / r['cycles'] for proc in payload['processes']
                                    if proc['variant'] == variant and proc['case'] == name
                                    for r in proc['rows'] if r['phase'] == phase]
                          for variant in ['baseline', 'candidate']}
                medians = {k: statistics.median(v) for k, v in values.items()}
                row[metric] = {'samples_per_cycle': values, 'median': medians,
                               'change_percent': (medians['candidate'] / medians['baseline'] - 1) * 100}
            summaries.append(row)
    payload['summaries'] = summaries
    args.output.write_text(json.dumps(payload, indent=2) + '\n')
    for row in summaries:
        print(row['case'], row['phase'], 'CPU', round(row['cpu_ns']['change_percent'], 3),
              'wall', round(row['wall_ns']['change_percent'], 3), flush=True)


if __name__ == '__main__':
    main()
