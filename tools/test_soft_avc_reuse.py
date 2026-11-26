#!/usr/bin/env python3
"""Run the complete AVC wrapper with the pinned, compiled libavc encoder.

The wrapper and observer use ASan/UBSan when requested; the provider uses ASan
only (its existing arithmetic is outside this change). The real-codec output
is independently decoded by FFmpeg. A/B mode accepts an explicit source ref;
only --measure-only disables the large-allocation budget. This is CPU codec
evidence, separate from the same probe's Windows execution and ART/MediaCodec.
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

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, timeout=180, **kwargs)


def build_provider(source: Path, destination: Path, sanitize: bool, cc: str) -> Path:
    cmake = (ROOT / 'cmake/aosp/media_video_codecs.cmake').read_text()
    sources = []
    for variable in ('_avc_srcs', '_avcenc_srcs'):
        match = re.search(r'set\(' + variable + r'\s+(.*?)\)', cmake, re.S)
        assert match is not None, variable
        sources += match[1].split()
    flags = ['-O2', '-g', '-msse4.2', '-mno-avx', '-DX86', '-DDISABLE_AVX2',
             '-DNDEBUG', '-DN_MB_ENABLE', '-UHP_PL', '-pthread']
    if sanitize:
        flags += ['-fsanitize=address', '-fno-omit-frame-pointer']
    for include in ('common', 'common/x86', 'encoder', 'encoder/x86'):
        flags += ['-I', str(source / include)]
    # Use the same renamed threading entry points as the product library.
    names = re.search(r'set\(_avc_ithread\s+(.*?)\)', cmake, re.S)[1].split()
    flags += [f'-Dithread_{name}=gesher_avc_ithread_{name}' for name in names]
    objects = [destination / (name.replace('/', '_') + '.o') for name in sources]

    def compile_one(pair):
        name, output = pair
        run(shlex.split(cc) + flags + ['-c', source / name, '-o', output])

    with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 1, 8)) as pool:
        list(pool.map(compile_one, zip(sources, objects)))
    library = destination / 'libavc-test.a'
    run(['ar', 'rcs', library, *objects])
    return library


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libavc-root', type=Path, required=True)
    parser.add_argument('--libavc-library', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--measure-only', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    provider = args.libavc_root.resolve()
    revision = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'HEAD'], text=True).strip()
    pinned = subprocess.check_output(['git', '-C', str(provider), 'rev-parse', 'android-16.0.0_r4^{commit}'],
                                     text=True).strip()
    assert revision == pinned, (revision, pinned)
    assert 'tag android-16.0.0_r4' in (ROOT / 'aosp.lock').read_text()
    relative = 'compat/media/encode/soft_avc_encoder.cc'
    source = ((ROOT / relative).read_bytes() if not args.source_ref else subprocess.check_output(
        ['git', 'show', args.source_ref + ':' + relative], cwd=ROOT))
    print('libavc revision ' + revision, flush=True)
    print('soft_avc_encoder.cc SHA256 ' + hashlib.sha256(source).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-soft-avc-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        library = args.libavc_library.resolve() if args.libavc_library else build_provider(
            provider, work, args.sanitize, args.cc)
        actual = work / 'soft_avc_encoder.cc'
        actual.write_bytes(source)
        probe = work / 'soft-avc-reuse'
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-mismatched-new-delete', '-fno-exceptions', '-DGESHER_MEDIA_STANDALONE',
            '-DGESHER_SOFT_AVC_SOURCE="' + str(actual) + '"', '-I', ROOT,
            '-I', ROOT / 'compat/media/encode', '-isystem', provider / 'common',
            '-isystem', provider / 'encoder', ROOT / 'tests/media/soft_avc_reuse_test.cc',
            library, '-pthread', '-lm', '-o', probe]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        run(command)
        result = subprocess.run([str(probe), 'measure' if args.measure_only else 'gate', str(work)],
                                check=False, capture_output=True, text=True, timeout=180)
        print(result.stdout, end='')
        print(result.stderr, end='')
        (work / 'probe.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        manifests = {}
        for name, frames in [('plain', 24), ('crop', 24), ('delayed', 24), ('overflow', 12),
                             ('fatal', 5), ('720p', 24), ('1080p', 12)]:
            encoded = work / (name + '.h264')
            decoded = run(['ffmpeg', '-v', 'error', '-xerror', '-i', encoded, '-vsync', '0',
                           '-f', 'framemd5', '-'], capture_output=True, text=True)
            assert not decoded.stderr, decoded.stderr
            lines = [line for line in decoded.stdout.splitlines() if line and not line.startswith('#')]
            assert len(lines) == frames, (name, len(lines), frames)
            manifests[name] = {
                'bitstream_sha256': hashlib.sha256(encoded.read_bytes()).hexdigest(),
                'packets_sha256': hashlib.sha256((work / (name + '.packets')).read_bytes()).hexdigest(),
                'frames': lines,
            }
        (work / 'decoded.json').write_text(json.dumps(manifests, indent=2) + '\n')
        print('FFmpeg independently decoded 125 actual H.264 frames from seven scenarios.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
