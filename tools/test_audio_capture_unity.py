#!/usr/bin/env python3
"""Actual capture stream and paced WAV source in a separate production TU.

Only engine attachment and the paced source's input clock are supplied by the
fixture. Production interpolation, PCM conversion, buffers, callbacks, lifecycle
and WAV parsing run unchanged. This does not replace a real WASAPI/device test.
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

ROOT = Path(__file__).resolve().parents[1]
CAPTURE = 'compat/media/audio/capture.cc'


def section(source, begin, end):
    assert source.count(begin) == source.count(end) == 1, (begin, end)
    return source[source.index(begin):source.index(end)]


def generate(output, ref=None, instrument=True, mutation=None):
    def read(path):
        if ref:
            return subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT).decode()
        return (ROOT / path).read_text()
    paths = (CAPTURE, 'compat/media/audio/capture.h', 'compat/media/audio/engine.cc',
             'compat/media/audio/engine.h')
    sources = {p: read(p) for p in paths}
    source = sources[CAPTURE]
    if mutation:
        before, after = mutation
        assert source.count(before) == 1, before
        source = source.replace(before, after, 1)
    members = section(source, 'CaptureStream::CaptureStream(', 'std::shared_ptr<CaptureStream> OpenCapture(')
    if instrument:
        anchor = '    auto source_frame = [&](size_t i, float* out) {\n'
        assert members.count(anchor) == 1
        members = members.replace(anchor, anchor + '      ++::capture_channel_frames; // test observer only\n')
    engine_header = sources['compat/media/audio/engine.h']
    header = re.sub(r'^\s*#(?:include|pragma).*\n', '', sources['compat/media/audio/capture.h'], flags=re.M)
    header = header.replace('private:', 'public:')
    common = (ROOT / 'tests/media/capture_unity_common.h.in').read_text()
    common = common.replace('@TYPES@', section(engine_header, 'enum class PcmFormat :', "// Android's stream types"))
    common = common.replace('@STATUSES@', '\n'.join(re.findall(
        r'constexpr int (?:kOk|kInvalidOperation) = [^;]+;', engine_header)))
    common = common.replace('@HEADER@', header)
    (output / 'capture_unity_common.h').write_text(common)
    bytes_per_sample = section(sources['compat/media/audio/engine.cc'], 'size_t BytesPerSample(',
                               '// --- Stream volume groups')
    pcm = source[source.index('void FloatToPcm('):source.rindex('}  // namespace gesher::media')]
    paced = section(source, '// --- Sources paced by the clock', '// The source this machine has')
    production = '''#include "capture_unity_common.h"
namespace gesher::media {
@BYTES@
@MEMBERS@
int CaptureEngine::StartStream(CaptureStream* stream, uint64_t generation) {
  ++wakes;
  return stream->Activate(generation);
}
@PCM@
namespace {
int64_t MonotonicNs() { return ::capture_source_clock_ns; }
@PACED@
}
std::unique_ptr<CaptureSource> OpenFileCaptureFixture(const std::string& path, std::string* error) {
  auto source = std::make_unique<FileSource>();
  if (!source->Load(path, error)) return nullptr;
  return source;
}
}
'''.replace('@BYTES@', bytes_per_sample).replace('@MEMBERS@', members).replace('@PCM@', pcm).replace('@PACED@', paced)
    (output / 'capture_unity_production.cc').write_text(production)
    return {p: hashlib.sha256(v.encode()).hexdigest() for p, v in sources.items()}


def build(output, cxx, sanitize, instrument, windows):
    command = shlex.split(cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-fno-exceptions', '-pthread', '-I', str(output)]
    if instrument:
        command += ['-DGESHER_COUNT_CAPTURE=1']
    if sanitize:
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    if windows:
        command += ['-static']
    objects = []
    for source in (output / 'capture_unity_production.cc', ROOT / 'tests/media/capture_unity_test.cc'):
        obj = output / (source.stem + '.o')
        subprocess.run(command + ['-c', str(source), '-o', str(obj)], check=True, timeout=120)
        objects.append(str(obj))
    executable = output / ('capture-unity.exe' if windows else 'capture-unity')
    subprocess.run(command + objects + ['-o', str(executable)], check=True, timeout=120)
    return executable, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--compare-ref')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--measure', action='store_true')
    parser.add_argument('--measure-only', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--generate-only', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    assert not (args.measure and args.sanitize), 'timings require uninstrumented code'
    assert not (args.negative_controls and (args.measure or args.measure_only or args.source_ref or
                args.generate_only or args.compile_only or args.build_only)), 'mutants require the current complete gate'
    with tempfile.TemporaryDirectory(prefix='gesher-capture-unity-') as temp:
        output = args.output_dir.resolve() if args.output_dir else Path(temp)
        output.mkdir(parents=True, exist_ok=True)

        def variant(directory, ref, gate):
            directory.mkdir(parents=True, exist_ok=True)
            hashes = generate(directory, ref, not args.measure)
            if args.generate_only:
                return None
            windows = os.name == 'nt' or args.compile_only
            exe, command = build(directory, args.cxx, args.sanitize, not args.measure, windows)
            receipt = {'source_ref': ref, 'sources': hashes, 'command': command,
                'compiler_version': subprocess.check_output(shlex.split(args.cxx) + ['--version'], text=True),
                'channel_frame_observer': not args.measure, 'sanitize': args.sanitize,
                'production_sha256': hashlib.sha256((directory / 'capture_unity_production.cc').read_bytes()).hexdigest(),
                'header_sha256': hashlib.sha256((directory / 'capture_unity_common.h').read_bytes()).hexdigest(),
                'fixture_sha256': hashlib.sha256((ROOT / 'tests/media/capture_unity_test.cc').read_bytes()).hexdigest(),
                'template_sha256': hashlib.sha256((ROOT / 'tests/media/capture_unity_common.h.in').read_bytes()).hexdigest(),
                'helper_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'executable_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()}
            (directory / 'source.json').write_text(json.dumps(receipt, indent=2) + '\n')
            if args.compile_only or args.build_only:
                return None
            mode = 'measure' if args.measure else 'gate' if gate else 'reference'
            result = subprocess.run([str(exe), mode, str(directory)], capture_output=True, text=True, timeout=120)
            (directory / 'probe.log').write_text(result.stdout + result.stderr)
            print(result.stdout, end=''); print(result.stderr, end='')
            result.check_returncode()
            return directory / 'transcript.bin'

        candidate = variant(output, args.source_ref, not args.measure_only)
        if args.compare_ref:
            assert not (args.measure or args.generate_only or args.compile_only or args.build_only)
            baseline = variant(output / 'baseline', args.compare_ref, False)
            assert candidate.read_bytes() == baseline.read_bytes(), 'capture complete transcript differs from baseline'
            print('CAPTURE_UNITY_BASELINE_PASS bytes=' + str(candidate.stat().st_size))
        if args.negative_controls:
            samples = 'independent channel rate and fractional-phase fallback samples'
            ieee = 'IEEE float interpolation preserves values and signed zero'
            mutants = [
                ('disabled-fast-path', ('if (src_rate == params_.sample_rate && src_channels == ch &&',
                    'if (false && src_rate == params_.sample_rate && src_channels == ch &&'),
                    'unity capture extracts only final channel history'),
                ('channel-guard', ('src_rate == params_.sample_rate && src_channels == ch &&',
                    'src_rate == params_.sample_rate &&'), samples),
                ('rate-guard', ('src_rate == params_.sample_rate && src_channels == ch &&',
                    'src_channels == ch &&'), samples),
                ('fractional-phase', ('phase_ == 0.0 && !std::signbit(phase_) &&',
                    '!std::signbit(phase_) &&'), ieee),
                ('negative-zero-phase', ('!std::signbit(phase_) && ', ''), ieee),
                ('rounding-guard', (' &&\n        std::fegetround() == FE_TONEAREST', ''), ieee),
                ('prior-history', ('const float* previous = last_.data();', 'const float* previous = src;'),
                    'independent finite capture samples'),
                ('discarded-accounting', ('      n = frames;', '      n = take;'),
                    'capture count and lost frames retain full-block accounting'),
                ('zero-weight-copy', ('previous[c] + (current[c] - previous[c]) * 0.0f', 'previous[c]'), ieee),
            ]
            receipts = []
            for name, mutation, assertion in mutants:
                directory = output / name; directory.mkdir(exist_ok=True)
                generate(directory, None, True, mutation)
                executable, command = build(directory, args.cxx, args.sanitize, True, os.name == 'nt')
                result = subprocess.run([str(executable), 'gate', str(directory)],
                                        capture_output=True, text=True, timeout=120)
                (directory / 'probe.log').write_text(result.stdout + result.stderr)
                assert result.returncode != 0 and ('CAPTURE_UNITY_FAIL: ' + assertion) in result.stderr, (
                    name, result.returncode, result.stdout, result.stderr)
                assert 'Sanitizer' not in result.stderr and 'runtime error:' not in result.stderr, (name, result.stderr)
                receipts.append({'name': name, 'assertion': assertion, 'returncode': result.returncode,
                    'mutation': mutation, 'command': command,
                    'production_sha256': hashlib.sha256((directory / 'capture_unity_production.cc').read_bytes()).hexdigest()})
                print('CAPTURE_UNITY_NEGATIVE_PASS ' + name, flush=True)
            (output / 'negative-controls.json').write_text(json.dumps(receipts, indent=2) + '\n')


if __name__ == '__main__':
    main()
