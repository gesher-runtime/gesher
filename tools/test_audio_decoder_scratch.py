#!/usr/bin/env python3
"""Decode real audio with the production wrapper and its pinned three providers.

Metadata reads and logging are explicit host boundaries. Actual MP3, Tremolo,
libopus and AOSP OpusHeader code run unchanged. Provider objects use ASan; the
production wrapper/observer additionally use UBSan. FFmpeg independently decodes
the complete clips; fixed-point output is compared with a documented tolerance.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLIPS = [('mp3-mono', 1, 44100, 1, 20), ('mp3-stereo', 1, 48000, 2, 20),
         ('mp3-mpeg2', 1, 24000, 2, 20), ('vorbis-mono', 2, 44100, 1, 20),
         ('vorbis-stereo', 2, 48000, 2, 20), ('opus-mono', 3, 48000, 1, 20),
         ('opus-stereo', 3, 48000, 2, 20), ('opus-long', 3, 48000, 2, 60),
         ('opus-surround', 3, 48000, 6, 20)]


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, timeout=240, **kwargs)


def build_provider(roots, work, sanitize, cc, cxx):
    cmake = (ROOT / 'cmake/aosp/media_codecs.cmake').read_text()
    include = ROOT / 'tests/media/audio-host/include'
    common = ['-O2', '-g', '-ffunction-sections', '-fdata-sections']
    if sanitize:
        common += ['-fsanitize=address', '-fno-omit-frame-pointer']
    av, tremolo, opus = roots
    mp3 = av / 'media/module/codecs/mp3dec'
    groups = [
        ('stagefright_mp3dec', '_mp3', mp3, cxx,
         ['-I', mp3 / 'include', '-I', mp3 / 'src', '-include',
          ROOT / 'compat/media/aosp/include/gesher_media_aosp.h']),
        ('vorbisidec', '_t', tremolo / 'Tremolo', cc,
         ['-I', include, '-I', tremolo / 'Tremolo', '-I', ROOT / 'compat/media/aosp/include',
          '-DONLY_C', '-D_GNU_SOURCE', '-funsigned-char']),
        ('opus', '_opus', opus, cc,
         ['-DNULL=0', '-DLOCALE_NOT_USED', '-Drestrict=', '-DOPUS_BUILD', '-DFIXED_POINT',
          '-DUSE_ALLOCA', '-DSIMD_EXTRA_ALLOC_BYTES=16', '-DHAVE_LRINT', '-DHAVE_LRINTF',
          '-DENABLE_HARDENING', '-fno-math-errno'] +
         [arg for path in ('include', 'src', 'silk', 'celt', 'silk/fixed') for arg in ('-I', opus / path)]),
    ]
    commands, objects = [], []
    for name, variable, root, compiler, flags in groups:
        block = re.search(r'add_library\(' + name + r' STATIC\s+(.*?)\)', cmake, re.S)[1]
        sources = re.findall(r'\$\{' + variable + r'\}/([^\s)]+)', block)
        assert sources
        for source in sources:
            output = work / (name + '_' + source.replace('/', '_') + '.o')
            objects.append(output)
            commands.append(shlex.split(compiler) + common + flags + ['-c', root / source, '-o', output])
    foundation = av / 'media/module/foundation'
    output = work / 'OpusHeader.o'
    objects.append(output)
    commands.append(shlex.split(cxx) + common + ['-I', include, '-I',
        foundation / 'include/media/stagefright/foundation', '-c', foundation / 'OpusHeader.cpp', '-o', output])
    def compile_one(command):
        try:
            run(command, capture_output=True, text=True)
        except subprocess.CalledProcessError as error:
            print(error.stdout + error.stderr)
            raise
    with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 1, 8)) as pool:
        list(pool.map(compile_one, commands))
    library = work / 'libaudio-providers.a'
    run(['ar', 'rcs', library, *objects])
    return library


def ogg_packets(path):
    data, cursor, pending, packets, previous = path.read_bytes(), 0, bytearray(), [], 0
    while cursor < len(data):
        assert data[cursor:cursor + 4] == b'OggS' and data[cursor + 4] == 0
        granule = struct.unpack_from('<Q', data, cursor + 6)[0]
        count = data[cursor + 26]
        sizes = data[cursor + 27:cursor + 27 + count]
        at = cursor + 27 + count
        first = True
        for size in sizes:
            pending.extend(data[at:at + size])
            at += size
            if size < 255:
                valid = max(0, granule - previous) if first else -1
                packets.append((bytes(pending), valid))
                pending.clear()
                first = False
        previous = granule
        cursor = at
    assert not pending
    return packets, previous


def opus_frames(packet):
    config, code = packet[0] >> 3, packet[0] & 3
    samples = 120 << (config & 3) if config >= 16 else (
        480 << (config & 1) if config >= 12 else 2880 if config & 3 == 3 else 480 << (config & 3))
    count = 1 if code == 0 else 2 if code < 3 else packet[1] & 63
    assert 0 < samples * count <= 5760
    return samples * count


def fixtures(work):
    manifest = {'ffmpeg': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
                'clips': {}}
    for name, codec, rate, channels, duration in CLIPS:
        samples = bytearray()
        for frame in range(rate * 21 // 10):
            t = frame / rate
            for channel in range(channels):
                value = 0 if 0.6 < t < 0.8 or 1.5 < t < 1.6 else int(
                    6000 * math.sin(2 * math.pi * (397 + 113 * channel) * t) +
                    2500 * math.sin(2 * math.pi * (1201 + 271 * channel) * t) * math.cos(23 * t))
                samples += struct.pack('<h', value)
        raw = work / (name + '.input.s16')
        raw.write_bytes(samples)
        encoded = work / (name + ('.mp3' if codec == 1 else '.ogg'))
        options = (['-c:a', 'libmp3lame', '-b:a', '64k' if channels == 1 or rate == 24000 else '192k',
                    '-write_xing', '0', '-id3v2_version', '0'] if codec == 1 else
                   ['-c:a', 'libvorbis', '-q:a', '4'] if codec == 2 else
                   ['-c:a', 'libopus', '-b:a', '320k' if channels > 2 else '96k',
                    '-vbr', 'off', '-frame_duration', duration])
        run(['ffmpeg', '-v', 'error', '-y', '-f', 's16le', '-ar', rate, '-ac', channels,
             '-i', raw, *options, encoded])
        reference = work / (name + '.reference.s16')
        run(['ffmpeg', '-v', 'error', '-xerror', '-y', '-i', encoded,
             '-c:a', 'pcm_s16le', '-f', 's16le', reference])
        pixels = reference.read_bytes()
        if channels == 6:
            # Ogg/Opus exposes Vorbis channel order; FFmpeg emits WAV order.
            # Compare the same actual channels (FL FC FR BL BR LFE).
            converted = bytearray()
            for frame in struct.iter_unpack('<6h', pixels):
                converted += struct.pack('<6h', *(frame[index] for index in (0, 2, 1, 4, 5, 3)))
            pixels = bytes(converted)
        head0, head1, delay, preroll, elapsed = b'', b'', 0, 0, 0
        packets = []
        if codec == 1:
            data = encoded.read_bytes()
            info = json.loads(run(['ffprobe', '-v', 'error', '-show_packets', '-of', 'json', encoded],
                                  capture_output=True, text=True).stdout)['packets']
            for packet in info:
                start, size = int(packet['pos']), int(packet['size'])
                packets.append((int(float(packet['pts_time']) * 1e6), -1, data[start:start + size]))
            expected = len(pixels) // (channels * 2)
            # Gesher's documented PacketVideo delay removal and silent drain.
            pixels = pixels[529 * channels * 2:] + bytes(529 * channels * 2)
        else:
            data, last_granule = ogg_packets(encoded)
            head0 = data[0][0]
            if codec == 2:
                assert head0.startswith(b'\x01vorbis') and data[2][0].startswith(b'\x05vorbis')
                head1 = data[2][0]
                for i, (packet, valid) in enumerate(data[3:]):
                    packets.append((i * 20000, valid, packet))
                expected = last_granule
            else:
                assert head0.startswith(b'OpusHead')
                skip = struct.unpack_from('<H', head0, 10)[0]
                delay, preroll = skip * 1000000000 // rate, 80000000
                for packet, valid in data[2:]:
                    packets.append((max(0, elapsed - skip) * 1000000 // rate, valid, packet))
                    elapsed += opus_frames(packet)
                expected = elapsed - int(float(delay) * rate / 1e9)
        fixture = bytearray(struct.pack('<4sIIIIIIQQII', b'GAUD', codec, rate, channels,
            len(packets), len(head0), len(head1), delay, preroll, expected, len(pixels)))
        fixture += head0 + head1 + pixels
        for time, valid, packet in packets:
            fixture += struct.pack('<qiI', time, valid, len(packet)) + packet
        (work / (name + '.audt')).write_bytes(fixture)
        manifest['clips'][name] = {'codec': codec, 'rate': rate, 'channels': channels,
            'packets': len(packets), 'expected_frames': expected,
            'reference_frames': len(pixels) // (channels * 2),
            'fixture_sha256': hashlib.sha256(fixture).hexdigest(),
            'reference_sha256': hashlib.sha256(pixels).hexdigest()}
    (work / 'fixtures.json').write_text(json.dumps(manifest, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frameworks-av-root', type=Path, required=True)
    parser.add_argument('--tremolo-root', type=Path, required=True)
    parser.add_argument('--opus-root', type=Path, required=True)
    parser.add_argument('--provider-library', type=Path)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--compare-results', type=Path,
                        help='Require byte-identical PCM to an unchanged-source result directory')
    parser.add_argument('--measure-only', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--reuse-fixtures', action='store_true')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    roots = [path.resolve() for path in (args.frameworks_av_root, args.tremolo_root, args.opus_root)]
    revisions = {}
    for name, path in zip(('frameworks-av', 'tremolo', 'libopus'), roots):
        revision = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        pinned = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'android-16.0.0_r4^{commit}'],
                                         text=True).strip()
        assert revision == pinned
        revisions[name] = revision
    assert 'tag android-16.0.0_r4' in (ROOT / 'aosp.lock').read_text()
    relative = 'compat/media/player/decoder.cc'
    source = ((ROOT / relative).read_bytes() if not args.source_ref else subprocess.check_output(
        ['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
    print('provider revisions ' + json.dumps(revisions), flush=True)
    print('decoder.cc SHA256 ' + hashlib.sha256(source).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-audio-scratch-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        if not args.reuse_fixtures:
            fixtures(work)
        manifest = json.loads((work / 'fixtures.json').read_text())
        for name, entry in manifest['clips'].items():
            assert hashlib.sha256((work / (name + '.audt')).read_bytes()).hexdigest() == entry['fixture_sha256']
        library = args.provider_library.resolve() if args.provider_library else build_provider(
            roots, work, args.sanitize, args.cc, args.cxx)
        actual = work / 'decoder.cc'
        actual.write_bytes(source)
        probe = work / 'audio-decoder-scratch'
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-mismatched-new-delete', '-ffunction-sections', '-fdata-sections',
            '-fno-exceptions', '-DGESHER_MEDIA_STANDALONE',
            '-DGESHER_AUDIO_DECODE_SOURCE="' + str(actual) + '"', '-I', ROOT,
            '-I', ROOT / 'compat/media/player', '-isystem', roots[0] / 'media/module/foundation/include',
            '-isystem', roots[0] / 'media/module/codecs/mp3dec/include',
            '-isystem', roots[0] / 'media/module/codecs/mp3dec/src',
            '-isystem', roots[1], '-isystem', roots[2] / 'include',
            ROOT / 'tests/media/audio_decoder_scratch_test.cc', library,
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
        comparison = None
        if args.compare_results:
            comparison = json.loads((args.compare_results / 'result.json').read_text())
            assert comparison['fixtures'] == manifest
            for name, *_ in CLIPS:
                for suffix in ('.actual.s16', '.after-drain.s16'):
                    assert (work / (name + suffix)).read_bytes() == (
                        args.compare_results / (name + suffix)).read_bytes(), name + suffix
            print('All nine complete PCM outputs and decode-after-drain outputs are byte-identical to the reference source.')
        metrics = []
        for line in result.stdout.splitlines():
            if line.startswith('metric,'):
                _, name, *values = line.split(',')
                metrics.append({'name': name, **{key: int(value) for key, value in
                    (item.split('=') for item in values)}})
        (work / 'result.json').write_text(json.dumps({'source_sha256': hashlib.sha256(source).hexdigest(),
            'providers': revisions, 'measure_only': args.measure_only,
            'comparison_source_sha256': comparison['source_sha256'] if comparison else None,
            'metrics': metrics, 'fixtures': manifest}, indent=2) + '\n')


if __name__ == '__main__':
    main()
