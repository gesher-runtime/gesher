#!/usr/bin/env python3
"""Ordinary separate-TU I420/QueueSurface lifecycle ABBA with exact output checks.

This uses the declared object/dispatch/clock boundary from test_i420_crop.py.
It does not execute Media Foundation, a codec provider, Android RefBase, a GPU or
Windows. Same-size cases call actual RenderFrame. Scaled controls call the actual
converter and QueueSurface lock/post directly, because RenderFrame requests the
visible size. Input and the independent full-byte oracle precede both clocks.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess

from test_i420_crop import BASELINE, ROOT, digest, generate

CASES = ('owner-empty', 'single-odd', 'odd-17x9', 'odd-65x37', 'odd-720p',
         'odd-1080p', 'odd-4k', 'even-720p', 'zero-720p', 'scaled-up', 'scaled-down')


def now():
    return datetime.now(timezone.utc).isoformat()


def build(directory, cxx, baseline):
    directory.mkdir(parents=True, exist_ok=True)
    commands, inputs = [], {}
    for name, ref in (('baseline', baseline), ('candidate', None)):
        output = directory / name
        output.mkdir(exist_ok=True)
        inputs[name] = generate(output, ref, observe=False)
    # Both callers and complete QueueSurface/RenderFrame have identical inputs.
    for path in ('i420_crop_boundary.h', 'i420_crop_surface.cc',
                 'compat/media/video/video_decoder.h', 'compat/media/video/yuv_rgba.h',
                 'compat/media/player/extractor.h', 'compat/media/player/data_source.h',
                 'compat/media/audio/log.h'):
        if (directory / 'baseline' / path).read_bytes() != (directory / 'candidate' / path).read_bytes():
            raise ValueError('common source differs across variants: ' + path)
    common = directory / 'common'
    common.mkdir(exist_ok=True)
    candidate = directory / 'candidate'
    flags = shlex.split(cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-fno-exceptions', '-ffunction-sections', '-fdata-sections',
        '-DGESHER_MEDIA_STANDALONE', '-DGESHER_I420_HOST', '-I', str(candidate),
        '-I', str(ROOT), '-I', str(candidate / 'compat/media/video'),
        '-I', str(ROOT / 'tests/media/i420-host/include')]
    objects = []
    for source in (candidate / 'i420_crop_surface.cc', ROOT / 'tests/media/i420_crop_bench.cc'):
        obj = common / (source.stem + '.o')
        cmd = flags + ['-c', str(source), '-o', str(obj)]
        subprocess.run(cmd, check=True, timeout=180); commands.append(cmd); objects.append(obj)
    variants = {}
    for name in ('baseline', 'candidate'):
        output = directory / name
        obj = output / 'i420_crop_video.o'
        # The common header identities were checked above. This selected full
        # production .cc has no counter injection or alteration whatsoever.
        source = output / 'i420_crop_video.cc'
        cmd = flags + ['-c', str(source), '-o', str(obj)]
        subprocess.run(cmd, check=True, timeout=180); commands.append(cmd)
        binary = output / 'i420-crop-bench'
        cmd = flags + [str(p) for p in [obj, *objects]] + ['-Wl,--gc-sections', '-pthread', '-o', str(binary)]
        subprocess.run(cmd, check=True, timeout=180); commands.append(cmd)
        result = subprocess.run([str(binary), '--contracts'], capture_output=True, text=True, timeout=180)
        log = output / 'contracts.log'; log.write_text(result.stdout + result.stderr)
        result.check_returncode()
        variants[name] = {'binary': str(binary), 'binary_sha256': digest(binary),
                          'sources': inputs[name], 'production_sha256': digest(source),
                          'contracts_log_sha256': digest(log), 'contracts_stdout': result.stdout}
    info = {'baseline_ref': baseline, 'compiler': subprocess.check_output(shlex.split(cxx) + ['--version'], text=True),
            'commands': commands, 'variants': variants,
            'common_objects': {p.name: digest(p) for p in objects},
            'driver_sha256': digest(ROOT / 'tests/media/i420_crop_bench.cc'),
            'helper_sha256': digest(Path(__file__)),
            'generator_sha256': digest(ROOT / 'tools/test_i420_crop.py')}
    (directory / 'build.json').write_text(json.dumps(info, indent=2) + '\n')
    return info


def summarize(processes):
    result = {}
    for case in CASES:
        result[case] = {}
        for phase in ('first', 'steady'):
            phase_result = {}
            for metric in ('cpu_ns', 'wall_ns'):
                values = {}
                for variant in ('baseline', 'candidate'):
                    values[variant] = [r[metric] / r['lifecycles']
                                      for p in processes if p['case'] == case and p['variant'] == variant
                                      for r in p['records'] if r['phase'] == phase]
                if not all(values.values()):
                    continue
                medians = {k: statistics.median(v) for k, v in values.items()}
                phase_result[metric] = {'medians_per_lifecycle': medians,
                    'change_percent': (medians['candidate'] / medians['baseline'] - 1) * 100,
                    'count': {k: len(v) for k, v in values.items()},
                    'min': {k: min(v) for k, v in values.items()},
                    'max': {k: max(v) for k, v in values.items()}}
            result[case][phase] = phase_result
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--baseline', default=BASELINE)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--blocks', type=int, default=1)
    args = parser.parse_args()
    directory = args.build_dir.resolve()
    info = json.loads((directory / 'build.json').read_text()) if args.skip_build else build(directory, args.cxx, args.baseline)
    if args.build_only:
        print(json.dumps({'build': str(directory / 'build.json'), 'variants': {
            k: v['binary_sha256'] for k, v in info['variants'].items()}})); return 0
    if not args.output or args.blocks < 1:
        parser.error('measurement requires --output and positive --blocks')
    for variant in info['variants'].values():
        if digest(Path(variant['binary'])) != variant['binary_sha256']:
            raise ValueError('binary differs from build receipt')
    start = now(); processes = []
    identities = {}
    for block in range(args.blocks):
        for variant in ('baseline', 'candidate', 'candidate', 'baseline'):
            for case in CASES:
                began = now()
                run = subprocess.run([info['variants'][variant]['binary'], case],
                                     capture_output=True, text=True, timeout=180)
                ended = now()
                run.check_returncode()
                if run.stderr:
                    raise ValueError('unexpected benchmark stderr: ' + run.stderr)
                records = [json.loads(line) for line in run.stdout.splitlines()]
                if len(records) != 4 or [r['phase'] for r in records] != ['first', 'steady', 'steady', 'steady']:
                    raise ValueError('incomplete measurement process')
                for r in records:
                    if r['case'] != case or r['cpu_ns'] <= 0 or r['wall_ns'] <= 0:
                        raise ValueError('invalid measurement record')
                    key = (case, r['phase'], r['sample'])
                    identity = {k: v for k, v in r.items() if k not in ('cpu_ns', 'wall_ns')}
                    if key in identities and identities[key] != identity:
                        raise ValueError('complete workload identity changed')
                    identities[key] = identity
                processes.append({'block': block, 'variant': variant, 'case': case, 'start_utc': began,
                    'end_utc': ended, 'returncode': run.returncode, 'stdout': run.stdout,
                    'stdout_sha256': hashlib.sha256(run.stdout.encode()).hexdigest(), 'records': records})
    report = {'start_utc': start, 'end_utc': now(), 'blocks': args.blocks,
              'order': ['baseline', 'candidate', 'candidate', 'baseline'],
              'platform': platform.platform(), 'build': info, 'processes': processes,
              'summary': summarize(processes),
              'boundary': 'Actual converter plus QueueSurface/RenderFrame, declared host object/dispatch/clock adapter; no Windows/provider/GPU execution. First lifecycle follows input/reference preparation. Full output validation is inside both timing intervals.'}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'start': report['start_utc'], 'end': report['end_utc'], 'processes': len(processes),
                      'records': sum(len(p['records']) for p in processes), 'output': str(args.output)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
