#!/usr/bin/env python3
"""Run whole capture engine/stream with a controlled WASAPI boundary on Windows."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index(name)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def adapters():
    opensl = (ROOT / 'compat/media/ndk/opensles.cc').read_text()
    recorder = (ROOT / 'compat/media/record/recorder.cc').read_text()
    record = (ROOT / 'compat/media/jni/audio_record_jni.cc').read_text()
    template = (ROOT / 'tests/media/capture_failure_adapters_host.cc.in').read_text()
    for marker, body in {
        'OPENSL_LAYOUT': opensl[opensl.index('struct DataLocatorHeader'):opensl.index('// --- Objects')],
        'RECORD_LOOP': method(opensl, 'void RecordLoop('),
        'REALIZE': method(opensl, 'SLresult SYSV ObjRealize('),
        'SET_STATE': method(opensl, 'SLresult SYSV RecSetState('),
        'CREATE_RECORDER': method(opensl, 'SLresult SYSV EngCreateAudioRecorder('),
        'RECORDER_START': method(recorder, 'bool Recorder::Start('),
        'RECORDER_AUDIO_LOOP': method(recorder, 'void Recorder::AudioLoop('),
        'READ_INTO': method(record, 'ssize_t ReadInto('),
    }.items():
        template = template.replace('@' + marker + '@', body)
    return template


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--fixture-output', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if args.compile_only and (args.output is None or args.fixture_output is None):
        parser.error('--compile-only requires --output and --fixture-output')
    with tempfile.TemporaryDirectory(prefix='gesher-capture-failure-') as temporary:
        work = Path(temporary)
        source = work / 'capture.cc'
        source.write_text((ROOT / 'tests/media/capture_failure_native.cc.in').read_text()
                          .replace('@ROOT@', ROOT.as_posix()).replace('@ADAPTER_IMPLEMENTATIONS@', adapters()))
        for fixture in (False, True):
            output = args.fixture_output if fixture else args.output
            binary = output.resolve() if output else work / ('fixture.exe' if fixture else 'production.exe')
            binary.parent.mkdir(parents=True, exist_ok=True)
            compiler = shlex.split(args.cxx)
            subprocess.run(compiler + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                           *(['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if args.sanitize else ['-static']),
                           '-pthread', '-DGESHER_MEDIA_STANDALONE', '-D_WIN32_WINNT=0x0A00',
                           *(['-DGESHER_CAPTURE_TEST_SOURCES'] if fixture else []),
                           '-I' + str(ROOT / 'compat/posix/include'), str(source), '-lkernel32', '-lmincore', '-o', str(binary)], check=True, timeout=90)
            env = os.environ.copy()
            env.pop('GESHER_AUDIO_SOURCE', None)
            if args.sanitize and os.name == 'nt':
                for name in ('libclang_rt.asan_dynamic-x86_64.dll', 'libc++.dll', 'libunwind.dll'):
                    runtime = Path(compiler[0]).parent / name
                    if runtime.is_file():
                        shutil.copyfile(runtime, binary.parent / name)
                env['PATH'] = str(Path(compiler[0]).parent) + os.pathsep + env.get('PATH', '')
            if args.compile_only:
                continue
            for case in (('fixture-env', 'fixture-bad-file') if fixture else ('missing', 'start-fails', 'disconnect', 'stop-race', 'release-race', 'reentrant', 'fixture-env',
                         'last-owner', 'stale-peer', 'opensl-descriptors', 'opensl-missing', 'opensl-permission', 'opensl-start', 'opensl-disconnect', 'recorder-start', 'recorder-disconnect', 'jni-read')):
                subprocess.run([str(binary), case], env=env, cwd=work, check=True, timeout=15)
        wasapi_source = (ROOT / 'compat/media/audio/capture_wasapi.cc').read_text().replace(' private:', ' public:')
        wasapi = work / 'wasapi.cc'
        wasapi.write_text((ROOT / 'tests/media/wasapi_capture_host.cc.in').read_text().replace('@WASAPI@', wasapi_source))
        wasapi_binary = args.output.resolve().parent / 'capture_wasapi_contract_test.exe' if args.output else work / 'wasapi.exe'
        subprocess.run(shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                       '-static', '-pthread', '-DGESHER_MEDIA_STANDALONE', '-D_WIN32_WINNT=0x0A00',
                       '-I' + str(ROOT / 'compat/media/audio'), '-I' + str(ROOT / 'compat/posix/include'),
                       str(wasapi), '-lole32', '-luuid', '-lpropsys', '-o', str(wasapi_binary)], check=True, timeout=90)
        if not args.compile_only:
            subprocess.run([str(wasapi_binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
