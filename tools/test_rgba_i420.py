#!/usr/bin/env python3
"""Actual RGBA converter/callbacks and pinned AVC encoding, with complete-byte oracles.

Only standalone metadata, clocks and host aligned allocation are adapted. The
converter is a separate production translation unit, so a test caller cannot
constant-fold away the baseline's coordinate mapping. Measurement builds have
no mapping observer or sanitizers. Actual Windows/ART remains a full-CI gate.
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
import subprocess
import tempfile

from codec_contract_source import ENGINE, JNI, member, shared

ROOT = Path(__file__).resolve().parents[1]
VIDEO = 'compat/media/video/video_decoder.cc'
ENCODER = 'compat/media/encode/soft_avc_encoder.cc'
RECORDER = 'compat/media/record/recorder.cc'


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, timeout=240, **kwargs)


def read_source(path, ref=None):
    if ref:
        return subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT).decode()
    return (ROOT / path).read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def generate(output, ref=None, instrument=True, mutation=None):
    codec = ENGINE if shared(ref) else JNI
    sources = {p: read_source(p, ref) for p in (VIDEO, ENCODER, codec, RECORDER)}
    video = sources[VIDEO]
    if mutation:
        before, after = mutation
        assert video.count(before) == 1, before
        video = video.replace(before, after)
    callback = member(sources[codec], 'OnSurfaceFrame') if codec == ENGINE else (
        function(sources[codec], 'void OnSurfaceFrame('))
    recorder = function(sources[RECORDER], 'void Recorder::OnFrame(').replace('Recorder::OnFrame', 'OnFrame', 1)
    clock = function(sources[RECORDER], 'int64_t Recorder::NowUs() const').replace('Recorder::NowUs', 'NowUs', 1)
    template = (ROOT / 'tests/media/rgba_i420_callbacks.h.in').read_text()
    (output / 'rgba_i420_callbacks.h').write_text(template.replace('@CODEC_CALLBACK@', callback)
        .replace('@RECORDER_CALLBACK@', recorder).replace('@RECORDER_CLOCK@', clock))
    if instrument:
        marker = '  auto at = [&](int x, int y) {\n'
        assert video.count(marker) == 1
        video = '#include <cstdint>\nextern std::uint64_t coordinate_mappings;\n' + video.replace(
            marker, marker + '    ++::coordinate_mappings;  // standalone observer only\n')
    (output / 'rgba_i420_video_decoder.cc').write_text(video)
    (output / 'rgba_i420_soft_avc_encoder.cc').write_text(sources[ENCODER])
    return {path: hashlib.sha256(source.encode()).hexdigest() for path, source in sources.items()}


def build_provider(provider, output, sanitize, cc):
    cmake = (ROOT / 'cmake/aosp/media_video_codecs.cmake').read_text()
    sources = []
    for name in ('_avc_srcs', '_avcenc_srcs'):
        match = re.search(r'set\(' + name + r'\s+(.*?)\)', cmake, re.S)
        assert match is not None
        sources += match[1].split()
    names = re.search(r'set\(_avc_ithread\s+(.*?)\)', cmake, re.S)[1].split()
    flags = ['-O3', '-g', '-msse4.2', '-mno-avx', '-DX86', '-DDISABLE_AVX2', '-pthread']
    flags += [f'-Dithread_{n}=gesher_avc_ithread_{n}' for n in names]
    if sanitize:
        flags += ['-fsanitize=address', '-fno-omit-frame-pointer']
    for include in ('common', 'common/x86', 'encoder', 'encoder/x86'):
        flags += ['-I', provider / include]
    objects = [output / (p.replace('/', '_') + '.o') for p in sources]

    def compile_one(pair):
        path, obj = pair
        extra = ['-DNDEBUG', '-DN_MB_ENABLE', '-UHP_PL'] if path.startswith('encoder/') else []
        run(shlex.split(cc) + flags + extra + ['-c', provider / path, '-o', obj])

    with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 1, 8)) as pool:
        list(pool.map(compile_one, zip(sources, objects)))
    library = output / 'libavc-rgba-test.a'
    run(['ar', 'rcs', library, *objects])
    return library


def build_probe(provider, library, output, sanitize, instrument, cxx):
    flags = shlex.split(cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-fno-exceptions', '-DGESHER_MEDIA_STANDALONE',
        '-I', ROOT, '-I', output, '-I', ROOT / 'compat/media/video',
        '-I', ROOT / 'compat/media/encode', '-I', ROOT / 'tests/media/rgba-host/include',
        '-isystem', provider / 'common', '-isystem', provider / 'encoder']
    if instrument:
        flags += ['-DGESHER_COUNT_COORDINATES=1']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    objects = []
    for src in (output / 'rgba_i420_video_decoder.cc', output / 'rgba_i420_soft_avc_encoder.cc',
                ROOT / 'tests/media/rgba_i420_test.cc'):
        obj = output / (src.stem + '.o')
        extra = ['-include', ROOT / 'tests/media/rgba_i420_host_alloc.h'] if 'soft_avc' in src.name else []
        run(flags + extra + ['-c', src, '-o', obj])
        objects.append(obj)
    exe = output / 'rgba-i420-test'
    run(flags + objects + [library, '-pthread', '-lm', '-o', exe])
    return exe


def decode_outputs(output):
    results = {}
    for candidate in sorted(output.glob('*.actual.h264')):
        reference = candidate.with_name(candidate.name.replace('.actual.', '.reference.'))
        assert candidate.read_bytes() == reference.read_bytes(), candidate
        decoded = run(['ffmpeg', '-v', 'error', '-xerror', '-i', candidate, '-vsync', '0',
                       '-f', 'framemd5', '-'], capture_output=True, text=True)
        assert not decoded.stderr, decoded.stderr
        frames = [line for line in decoded.stdout.splitlines() if line and not line.startswith('#')]
        count = int(candidate.name.split('.')[0].split('-')[-1])
        assert len(frames) == count, (candidate, len(frames), count)
        results[candidate.name] = {'bitstream_sha256': hashlib.sha256(candidate.read_bytes()).hexdigest(),
                                  'frames': frames}
    assert results, 'no real encoded output'
    (output / 'decoded.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'FFMPEG_RGBA_PASS streams={len(results)} frames={sum(len(x["frames"]) for x in results.values())}')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libavc-root', type=Path)
    parser.add_argument('--libavc-library', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--measure', action='store_true')
    parser.add_argument('--measure-only', action='store_true',
                        help='run the complete reference corpus, disabling only same-size mapping budgets')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    assert not (args.measure and args.sanitize), 'timing must be uninstrumented'
    assert not (args.measure and args.measure_only), 'select timing or full reference corpus'
    with tempfile.TemporaryDirectory(prefix='gesher-rgba-') as temp:
        output = args.output_dir.resolve() if args.output_dir else Path(temp)
        output.mkdir(parents=True, exist_ok=True)
        identities = generate(output, args.source_ref, not args.measure)
        if args.generate_only:
            return
        assert args.libavc_root
        provider = args.libavc_root.resolve()
        revision = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'HEAD'], text=True).strip()
        pin = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'android-16.0.0_r4^{commit}'], text=True).strip()
        assert revision == pin and 'tag android-16.0.0_r4' in (ROOT / 'aosp.lock').read_text()
        run(['git', '-C', provider, 'diff', '--exit-code', '--quiet'])
        library = args.libavc_library.resolve() if args.libavc_library else build_provider(provider, output, args.sanitize, args.cc)
        exe = build_probe(provider, library, output, args.sanitize, not args.measure, args.cxx)
        mode = 'measure' if args.measure else 'reference' if args.measure_only else 'gate'
        result = subprocess.run([str(exe), mode, str(output)],
                                capture_output=True, text=True, timeout=240)
        print(result.stdout, end=''); print(result.stderr, end='')
        (output / 'probe.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        decoded = decode_outputs(output)
        receipt = {'sources': identities, 'provider_revision': revision,
                   'provider_library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                   'mapping_observer': not args.measure, 'sanitize': args.sanitize,
                   'decoded_streams': len(decoded)}
        (output / 'source.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if args.negative_controls:
            assert not args.measure and not args.measure_only
            mutants = [
                ('disabled-fast-path', ('if (src_w == w && src_h == h)', 'if (false)'), 'same-size mapping budget'),
                ('height-guard', ('src_w == w && src_h == h', 'src_w == w'), 'complete output and padding oracle'),
                ('chroma-position', ('const uint32_t c = row[2 * x];', 'const uint32_t c = row[x];'), 'complete output and padding oracle'),
                ('padding-write', ('dst[x] = Clamp8', 'dst[x + 1] = Clamp8'), 'complete output and padding oracle'),
            ]
            for name, mutation, assertion in mutants:
                directory = output / name; directory.mkdir(exist_ok=True)
                generate(directory, args.source_ref, True, mutation)
                mutant = build_probe(provider, library, directory, args.sanitize, True, args.cxx)
                bad = subprocess.run([str(mutant), 'negative'], capture_output=True, text=True, timeout=30)
                (directory / 'probe.log').write_text(bad.stdout + bad.stderr)
                assert bad.returncode != 0 and assertion in bad.stderr, (name, bad.stdout, bad.stderr)
                assert 'Sanitizer' not in bad.stderr and 'runtime error:' not in bad.stderr, (name, bad.stderr)
                print('RGBA_NEGATIVE_PASS ' + name)


if __name__ == '__main__':
    main()
