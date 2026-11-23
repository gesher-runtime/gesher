#!/usr/bin/env python3
"""Exercise the complete HEVC decoder with real libhevc and independent FFmpeg pixels.

The provider's C objects use ASan, while the product wrapper and observer also
use UBSan. Windows aligned allocation and null Format metadata are explicit host
boundaries; unselected video backends abort instead of simulating a provider.
Generated .hevct files carry real H.265 access units,
presentation timestamps and FFmpeg pixel hashes; the same files run on Windows.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLIPS = [('small', 384, 256, 12), ('cropped', 318, 238, 12),
         ('720p', 1280, 720, 24), ('1080p', 1920, 1080, 12), ('4k', 3840, 2160, 8)]


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, timeout=240, **kwargs)


def pixel_hash(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def fixtures(work: Path):
    manifest = {'ffmpeg': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
                'clips': {}}
    for name, width, height, count in CLIPS:
        encoded = work / (name + '.hevc')
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
             f'testsrc2=size={width}x{height}:rate=30', '-frames:v', count,
             '-c:v', 'libx265', '-preset', 'veryfast', '-crf', '25', '-pix_fmt', 'yuv420p',
             '-x265-params', 'aud=1:repeat-headers=1:keyint=8:min-keyint=8:scenecut=0:'
                             'bframes=2:b-adapt=0:b-pyramid=0:open-gop=0:frame-threads=1:pools=none:wpp=0:log-level=error',
             '-f', 'hevc', encoded])
        packets = json.loads(run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                  '-show_packets', '-of', 'json', encoded],
                                 capture_output=True, text=True).stdout)['packets']
        frames = json.loads(run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                                 '-show_frames', '-of', 'json', encoded],
                                capture_output=True, text=True).stdout)['frames']
        assert len(packets) == len(frames) == count
        assert any(frame['pict_type'] == 'B' for frame in frames)
        pts = {int(frame['pkt_pos']): index * 33333 + 7 for index, frame in enumerate(frames)}
        reference = work / (name + '.reference.i420')
        run(['ffmpeg', '-v', 'error', '-xerror', '-y', '-i', encoded, '-vsync', '0',
             '-pix_fmt', 'yuv420p', '-f', 'rawvideo', reference])
        pixels = reference.read_bytes()
        frame_bytes = width * height * 3 // 2
        assert len(pixels) == count * frame_bytes
        hashes = [pixel_hash(pixels[index * frame_bytes:(index + 1) * frame_bytes])
                  for index in range(count)]
        compressed = encoded.read_bytes()
        fixture = bytearray(struct.pack('<4sIII', b'HEVC', width, height, count))
        fixture += struct.pack('<' + 'Q' * count, *hashes)
        for packet in packets:
            start, size = int(packet['pos']), int(packet['size'])
            fixture += struct.pack('<Iq', size, pts[start]) + compressed[start:start + size]
        destination = work / (name + '.hevct')
        destination.write_bytes(fixture)
        manifest['clips'][name] = {
            'width': width, 'height': height, 'frames': count,
            'fixture_sha256': hashlib.sha256(fixture).hexdigest(),
            'reference_sha256': hashlib.sha256(pixels).hexdigest(),
            'display_picture_types': [frame['pict_type'] for frame in frames],
        }
    (work / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n')


def build_provider(source: Path, destination: Path, sanitize: bool, cc: str) -> Path:
    cmake = re.sub(r'#[^\n]*', '', (ROOT / 'cmake/aosp/media_video_codecs.cmake').read_text())
    block = re.search(r'set\(_hevc_srcs\s+(.*?)\)', cmake, re.S)[1]
    sources = re.findall(r'(?:common|decoder)/[\w/]+\.c', block)
    assert sources
    flags = ['-O3', '-g', '-msse4.2', '-mno-avx', '-DX86', '-DDISABLE_AVX2',
             '-D_LIB', '-DDISABLE_SEI', '-UKEEP_THREADS_ACTIVE',
             '-DDEFAULT_ARCH=D_ARCH_X86_SSE42', '-pthread']
    if sanitize:
        flags += ['-fsanitize=address', '-fno-omit-frame-pointer']
    for include in ('common', 'common/x86', 'decoder', 'decoder/x86'):
        flags += ['-I', str(source / include)]
    objects = [destination / (name.replace('/', '_') + '.o') for name in sources]

    def compile_one(pair):
        name, output = pair
        run(shlex.split(cc) + flags + ['-c', source / name, '-o', output])

    with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 1, 8)) as pool:
        list(pool.map(compile_one, zip(sources, objects)))
    library = destination / 'libhevc-decoder.a'
    run(['ar', 'rcs', library, *objects])
    return library


def negative_controls(source: bytes):
    """Require the copy budget and each plane identity check to be effective."""
    pixels = 'Hash(frame.data.data(), frame.data.size()) == clip.hashes[index]'
    mutations = [
        ('always-copy', 'if (p[0].stride == w &&', 'if (false && p[0].stride == w &&',
         'bytes == 0 && full_bytes == 0'),
        ('ignore-y-ownership', 'p[0].data == buffer_.data() &&', 'true &&', pixels),
        ('ignore-u-ownership', 'p[1].data == buffer_.data() + luma &&', 'true &&', pixels),
        ('ignore-v-ownership', 'p[2].data == buffer_.data() + luma + chroma)', 'true)', pixels),
    ]
    text = source.decode('utf-8')
    for name, before, after, expected in mutations:
        if text.count(before) != 1:
            raise ValueError('HEVC mutation boundary changed: ' + name)
        yield name, text.replace(before, after).encode('utf-8'), expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libhevc-root', type=Path, required=True)
    parser.add_argument('--libhevc-library', type=Path)
    parser.add_argument('--vpx-root', type=Path, required=True)
    parser.add_argument('--dav1d-root', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--measure-only', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--reuse-fixtures', action='store_true')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    if args.negative_controls and (os.name == 'nt' or args.measure_only or args.source_ref):
        parser.error('--negative-controls requires the POSIX current-source gate')
    provider = args.libhevc_root.resolve()
    revision = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'HEAD'], text=True).strip()
    pinned = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'android-16.0.0_r4^{commit}'],
                                     text=True).strip()
    assert revision == pinned
    headers = {}
    for name, path in (('libvpx', args.vpx_root), ('libdav1d', args.dav1d_root)):
        current = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        tag = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'android-16.0.0_r4^{commit}'], text=True).strip()
        assert current == tag
        headers[name] = current
    assert 'tag android-16.0.0_r4' in (ROOT / 'aosp.lock').read_text()
    relative = 'compat/media/video/soft_video.cc'
    source = ((ROOT / relative).read_bytes() if not args.source_ref else subprocess.check_output(
        ['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
    print('libhevc revision ' + revision, flush=True)
    print('soft_video.cc SHA256 ' + hashlib.sha256(source).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-hevc-output-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        if not args.reuse_fixtures:
            fixtures(work)
        manifest = json.loads((work / 'fixtures.json').read_text())
        for name, entry in manifest['clips'].items():
            assert hashlib.sha256((work / (name + '.hevct')).read_bytes()).hexdigest() == entry['fixture_sha256']
        library = args.libhevc_library.resolve() if args.libhevc_library else build_provider(
            provider, work, args.sanitize, args.cc)
        actual = work / 'soft_video.cc'
        actual.write_bytes(source)
        probe = work / 'soft-hevc-output'
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-mismatched-new-delete', '-ffunction-sections', '-fdata-sections',
            '-fno-exceptions', '-DGESHER_MEDIA_STANDALONE',
            '-DGESHER_SOFT_HEVC_DECODE_SOURCE="' + str(actual) + '"', '-I', ROOT,
            '-I', ROOT / 'tests/media/decoder-host/include', '-I', ROOT / 'compat/media/video',
            '-isystem', provider / 'common', '-isystem', provider / 'decoder',
            '-isystem', args.vpx_root.resolve(), '-isystem', args.dav1d_root.resolve() / 'include',
            ROOT / 'tests/media/soft_hevc_output_test.cc',
            ROOT / 'compat/media/video/video_decoder.cc', library,
            '-Wl,--gc-sections', '-pthread', '-lm', '-o', probe]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        run(command)
        result = subprocess.run([str(probe), 'measure' if args.measure_only else 'gate', str(work)],
                                check=False, capture_output=True, text=True, timeout=240)
        print(result.stdout, end='')
        print(result.stderr, end='')
        (work / 'probe.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        for name, _, _, _ in CLIPS:
            reference = work / (name + '.reference.i420')
            actual = work / (name + '.actual.i420')
            assert actual.read_bytes() == reference.read_bytes(), name
        metrics = []
        for line in result.stdout.splitlines():
            if line.startswith('metric,'):
                _, name, *values = line.split(',')
                metrics.append({'name': name, **{key: int(value) for key, value in
                                                (item.split('=') for item in values)}})
        controls = []
        if args.negative_controls:
            for name, variant, expected in negative_controls(source):
                variant_source = work / (name + '.cc')
                variant_source.write_bytes(variant)
                variant_probe = work / ('soft-hevc-' + name)
                variant_command = command.copy()
                define = next(i for i, arg in enumerate(variant_command)
                              if str(arg).startswith('-DGESHER_SOFT_HEVC_DECODE_SOURCE='))
                variant_command[define] = '-DGESHER_SOFT_HEVC_DECODE_SOURCE="' + str(variant_source) + '"'
                variant_command[variant_command.index('-o') + 1] = variant_probe
                run(variant_command)
                failed = subprocess.run([str(variant_probe), 'gate', str(work)], check=False,
                                        capture_output=True, text=True, timeout=240)
                (work / (name + '.log')).write_text(failed.stdout + failed.stderr)
                if (failed.returncode != -signal.SIGABRT or
                        'HEVC output check failed at ' not in failed.stderr or expected not in failed.stderr):
                    raise RuntimeError('negative control did not fail its intended assertion: ' + name +
                                       '\n' + failed.stdout + failed.stderr)
                controls.append({'name': name, 'source_sha256': hashlib.sha256(variant).hexdigest(),
                                 'assertion': expected, 'returncode': failed.returncode})
                print('PASS negative control ' + name + ': ' + expected, flush=True)
        (work / 'result.json').write_text(json.dumps({
            'source_sha256': hashlib.sha256(source).hexdigest(), 'libhevc_revision': revision,
            'header_dependency_revisions': headers, 'measure_only': args.measure_only,
            'metrics': metrics, 'fixtures': manifest, 'negative_controls': controls,
        }, indent=2) + '\n')
        print('All 68 frames byte-identical to independent FFmpeg decoding; retained/lifecycle cases also passed.')


if __name__ == '__main__':
    main()
