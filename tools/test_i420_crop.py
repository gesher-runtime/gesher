#!/usr/bin/env python3
"""Actual full I420 converter plus QueueSurface/RenderFrame and exact pixel oracles.

The converter is a separate production translation unit. The complete surface
and renderer use the existing declared Android object/dispatch and clock boundary;
no decoder, Skia or hardware behavior is simulated as success. The same assertion
file also runs against real hostgraphics objects on Windows.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_media_surface_contracts import method

ROOT = Path(__file__).resolve().parents[1]
VIDEO = 'compat/media/video/video_decoder.cc'
DAMAGE = 'compat/hwui/window/surface_damage.h'
INPUTS = (VIDEO, 'compat/media/video/video_decoder.h', 'compat/media/video/yuv_rgba.h',
          'compat/media/player/extractor.h', 'compat/media/player/data_source.h',
          'compat/media/audio/log.h',
          'compat/media/video/render.cc', 'compat/media/surface/queue_surface.h',
          'compat/media/surface/queue_surface.cc', 'tests/media/surface_geometry_host.cc.in',
          'tests/media/i420_crop_test.cc', 'tests/media/i420-host/include/media/NdkMediaFormat.h')
BASELINE = 'e505ad227523aaca566d05b8af6e466495c819cc'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path, ref=None):
    if ref:
        return subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT).decode()
    return (ROOT / path).read_text()


def portable(source):
    return re.sub(r'^\s*#(?:include|pragma).*\n', '', source, flags=re.MULTILINE).replace(
        'protected:', 'public:')


def generate(output, ref=None, observe=True, mutation=None):
    # Assertions/adapters stay current; runtime inputs use the requested ref.
    sources = {p: read(p, ref if not p.startswith('tests/') else None) for p in INPUTS}
    # Keep the actual selected headers next to their selected translation unit;
    # a baseline build must not silently pick a changed working-tree header.
    for path in (p for p in INPUTS if p.startswith('compat/') and p.endswith('.h')):
        destination = output / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(sources[path])
    constants = set(re.findall(r'\bNATIVE_WINDOW_[A-Z0-9_]+\b',
                              sources['compat/media/surface/queue_surface.h'] +
                              sources['compat/media/surface/queue_surface.cc']))
    constants.discard('NATIVE_WINDOW_TIMESTAMP_AUTO')
    boundary = sources['tests/media/surface_geometry_host.cc.in'].split('@PRODUCTION@')[0]
    boundary = boundary.replace('@CONSTANTS@', 'enum { ' + ', '.join(sorted(constants)) + ' };')
    # The current window adapter uses the production rectangle on both refs.
    sources[DAMAGE] = read(DAMAGE)
    if boundary.count('@DAMAGE_TYPES@') != 1:
        raise ValueError('surface damage adapter boundary changed')
    boundary = boundary.replace('@DAMAGE_TYPES@', portable(sources[DAMAGE]))
    boundary = boundary.replace('int64_t systemTime(int)', 'inline int64_t systemTime(int)')
    boundary = '#pragma once\n#include "compat/media/video/video_decoder.h"\n' + boundary
    boundary += '\nnamespace android { constexpr int OK = 0, BAD_VALUE = -EINVAL, NO_INIT = -ENODEV; }\n'
    boundary += portable(sources['compat/media/surface/queue_surface.h'])
    boundary += '''
inline int ANativeWindow_setBuffersGeometry(ANativeWindow* w, int width, int height, int format) {
  return w->perform(w, NATIVE_WINDOW_SET_BUFFERS_GEOMETRY, width, height, format);
}
inline int ANativeWindow_lock(ANativeWindow* w, ANativeWindow_Buffer* b, ARect* dirty) {
  return static_cast<android::Surface*>(w)->lock(b, dirty);
}
inline int ANativeWindow_unlockAndPost(ANativeWindow* w) {
  return static_cast<android::Surface*>(w)->unlockAndPost();
}
'''
    (output / 'i420_crop_boundary.h').write_text(boundary)
    surface = '#include "i420_crop_boundary.h"\n' + portable(sources['compat/media/surface/queue_surface.cc'])
    surface += portable(sources['compat/media/video/render.cc'])
    (output / 'i420_crop_surface.cc').write_text(surface)
    video = sources[VIDEO]
    if mutation:
        before, after = mutation
        if video.count(before) != 1:
            raise ValueError('mutation boundary changed: ' + before)
        video = video.replace(before, after)
    if observe:
        body = method(video, 'void I420ToRgba(')
        observed, rows = re.subn(r'^([ \t]*)yuv::RowToRgba\(',
                                 r'\1++::i420_row_calls;\n\1yuv::RowToRgba(', body, flags=re.MULTILINE)
        if rows != (2 if 'DisjointFrameOutput' in body else 1):
            raise ValueError('row observation boundary changed')
        observed, first = re.subn(r'^([ \t]*)d\[0\] = yuv::PixelToRgba',
            r'\1++::i420_first_pixels;\n\1d[0] = yuv::PixelToRgba', observed, flags=re.MULTILINE)
        if first not in (0, 1):
            raise ValueError('first pixel boundary changed')
        loop = '    for (int x = 0; x < dst_w; x++) {\n'
        if observed.count(loop) != 1:
            raise ValueError('scalar observation boundary changed')
        observed = observed.replace(loop, loop + '      ++::i420_scalar_pixels;\n')
        video = video.replace(body, observed)
        video = ('#include <cstdint>\nthread_local uint64_t i420_row_calls = 0, '
                 'i420_first_pixels = 0, i420_scalar_pixels = 0;\n'
                 'extern "C" void i420_crop_reset_counts() {\n'
                 ' i420_row_calls = i420_first_pixels = i420_scalar_pixels = 0;\n}\n'
                 'extern "C" void i420_crop_read_counts(uint64_t* rows, uint64_t* first, uint64_t* scalar) {\n'
                 ' *rows = i420_row_calls; *first = i420_first_pixels; *scalar = i420_scalar_pixels;\n}\n') + video
    (output / 'i420_crop_video.cc').write_text(video)
    return {p: hashlib.sha256(s.encode()).hexdigest() for p, s in sources.items()}


def build(output, args, ref=None, observe=True, mutation=None):
    output.mkdir(parents=True, exist_ok=True)
    sources = generate(output, ref, observe, mutation)
    command = shlex.split(args.cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-fno-exceptions', '-ffunction-sections', '-fdata-sections',
        '-DGESHER_MEDIA_STANDALONE', '-DGESHER_I420_HOST', '-I', str(output), '-I', str(ROOT),
        '-I', str(output / 'compat/media/video'), '-I', str(ROOT / 'tests/media/i420-host/include')]
    if observe:
        command += ['-DGESHER_I420_OBSERVE']
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    commands, objects = [], []
    for source in [output / 'i420_crop_video.cc', output / 'i420_crop_surface.cc',
                   ROOT / 'tests/media/i420_crop_test.cc']:
        obj = output / (source.stem + '.o')
        cmd = command + ['-c', str(source), '-o', str(obj)]
        subprocess.run(cmd, check=True, timeout=180); commands.append(cmd); objects.append(obj)
    binary = output / ('i420-crop.exe' if os.name == 'nt' else 'i420-crop')
    cmd = command + [str(p) for p in objects] + ['-Wl,--gc-sections', '-pthread', '-o', str(binary)]
    subprocess.run(cmd, check=True, timeout=180); commands.append(cmd)
    receipt = {'source_ref': ref, 'sources': sources, 'observe': observe,
               'sanitize': args.sanitize, 'commands': commands, 'binary_sha256': digest(binary),
               'generated': {p.name: digest(p) for p in output.glob('*.cc')},
               'boundary_sha256': digest(output / 'i420_crop_boundary.h')}
    (output / 'build.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return binary, receipt


def execute(binary, arguments, output, expected_failure=None):
    result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=180)
    text = result.stdout + result.stderr
    output.write_text(text)
    if expected_failure:
        if result.returncode == 0 or expected_failure not in text:
            raise AssertionError((binary, result.returncode, text))
        if 'Sanitizer' in text or 'runtime error:' in text:
            raise AssertionError('mutation must fail its assertion, not memory safety: ' + text)
    else:
        print(text, end='', flush=True)
        result.check_returncode()
    return {'returncode': result.returncode, 'log_sha256': digest(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--compare-ref', default=BASELINE)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-i420-crop-') as temporary:
        output = args.output_dir.resolve() if args.output_dir else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        current, receipt = build(output / 'candidate', args, args.source_ref)
        receipt['run'] = execute(current, [], output / 'candidate.log')
        baseline, old = build(output / 'baseline', args, args.compare_ref)
        old['run'] = execute(baseline, ['--reference'], output / 'baseline.log')
        old['negative'] = execute(baseline, ['--negative'], output / 'baseline-negative.log',
                                  'same-size odd crop uses aligned row conversion')
        report = {'candidate': receipt, 'baseline': old, 'negative_controls': {}}
        if args.negative_controls:
            controls = [
                ('disabled-row-path', ('same && DisjointFrameOutput(f, dst, dst_stride, dst_w, dst_h)',
                                       'same && (DisjointFrameOutput(f, dst, dst_stride, dst_w, dst_h) && false)'),
                                      [], 'same-size odd crop uses aligned row conversion'),
                ('wrong-chroma-phase', ('yr + 1, ur + 1, vr + 1',
                                        'yr + 1, ur, vr + 1'), [], 'complete cropped pixels'),
                ('wrong-leading-luma', ('yuv::PixelToRgba(yr[0], ur[0], vr[0])',
                                        'yuv::PixelToRgba(yr[1], ur[0], vr[0])'), [], 'complete cropped pixels'),
                ('missing-tail', ('d + 1, dst_w - 1);',
                                   'd + 1, dst_w - 2);'), [], 'complete cropped pixels'),
                ('height-guard', ('dst_w == vw && dst_h == vh', 'dst_w == vw'),
                                 ['height'], 'complete cropped pixels'),
                ('overlap-guard', ('same && DisjointFrameOutput(f, dst, dst_stride, dst_w, dst_h)',
                    'same && (DisjointFrameOutput(f, dst, dst_stride, dst_w, dst_h) || true)'),
                    ['alias'], 'aliased odd-crop source does not enter restrict row helper'),
            ]
            for name, mutation, extra, assertion in controls:
                binary, info = build(output / name, args, args.source_ref, mutation=mutation)
                info['run'] = execute(binary, ['--negative', *extra], output / (name + '.log'), assertion)
                report['negative_controls'][name] = info
                print('I420_CROP_NEGATIVE_PASS ' + name, flush=True)
        (output / 'source.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
