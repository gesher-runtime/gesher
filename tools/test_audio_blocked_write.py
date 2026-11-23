#!/usr/bin/env python3
"""Exercise actual Track blocking-write ownership through its real mixer.

Uses the maintained audio-state actual-source generator: complete production
Track methods, conversion, ring and resampler, with explicit Engine/clock/CV
host boundaries. Real waiters cover catch-up, FIFO registration and cancellation;
the CV hook only schedules precise pause/stop/restart/spurious interleavings.
Neither Windows endpoints nor ART/JNI execution are inferred from this fixture.
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

from test_audio_state_contracts import generate, section

ROOT = Path(__file__).resolve().parents[1]
BASELINE = 'bcfb5044029d486e17138dc417f3ced81524cf3f'
INPUTS = ['compat/media/audio/engine.cc', 'compat/media/audio/engine.h',
          'compat/media/audio/sink.h', 'compat/media/audio/sink_file.cc',
          'compat/media/probe/media_probe.cc',
          'tests/media/audio_blocked_write_host.cc.in', 'tests/media/audio_state_host.cc.in',
          'tools/test_audio_blocked_write.py', 'tools/test_audio_state_contracts.py']


def source(ref: str | None) -> str:
    def read(path: str) -> str:
        return subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT, text=True) \
            if ref else (ROOT / path).read_text()
    production = generate('audio', ref)
    boundary = 'namespace {\nusing namespace gesher::media;'
    if production.count(boundary) != 1:
        raise ValueError('actual Track generator boundary changed')
    production = production[:production.index(boundary)]
    definition = '#define HAS_PENDING_WRITE ' + str(int('struct Track::PendingWrite {' in production)) + '\n'
    fixture = (ROOT / 'tests/media/audio_blocked_write_host.cc.in').read_text()
    engine = read('compat/media/audio/engine.cc')
    wait_refill = ''
    if 'void Sink::WaitForRefill(' in engine:
        # Keep the actual new default method for its vtable. Explicit Render
        # calls never enter the OS wait; any accidental use must fail closed.
        wait_refill = ('[[noreturn]] unsigned long WaitForSingleObject(void*, unsigned long) '
                       '{ std::abort(); }\n' +
                       section(engine, 'void Sink::WaitForRefill(', '\n\nclass Engine :'))
    replacements = {
        '@PRODUCTION@': production,
        '@SINK_HEADER@': read('compat/media/audio/sink.h').replace('#pragma once', ''),
        '@WAIT_REFILL@': wait_refill,
        '@CAPTURE_SOURCE@': section(read('compat/media/audio/sink_file.cc'),
                                   'constexpr uint32_t kRate', 'std::string Narrow('),
        '@ANALYZE@': section(read('compat/media/probe/media_probe.cc'), 'int Analyze(', 'int Wasapi('),
    }
    for marker, text in replacements.items():
        if fixture.count(marker) != 1:
            raise ValueError('blocked-write fixture boundary changed: ' + marker)
        fixture = fixture.replace(marker, text)
    return definition + fixture


def altered(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError('production mutation boundary changed: ' + before)
    return text.replace(before, after)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', default='all')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    if args.negative_controls and (args.source_ref or args.case != 'all'):
        parser.error('negative controls require the complete current-source suite')
    temporary = tempfile.TemporaryDirectory(prefix='gesher-audio-blocked-') if not args.output_dir else None
    output = args.output_dir.resolve() if args.output_dir else Path(temporary.name)
    output.mkdir(parents=True, exist_ok=True)
    receipts = []
    snapshot = {p: (ROOT / p).read_bytes() for p in INPUTS}

    def unchanged() -> None:
        if any((ROOT / path).read_bytes() != data for path, data in snapshot.items()):
            raise RuntimeError('source or fixture changed during validation')

    def execute(label: str, body: str, case: str, ref: str | None) -> subprocess.CompletedProcess:
        unchanged()
        cpp = output / (label + '.cc')
        cpp.write_text(body)
        binary = output / label
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                          '-pthread', str(cpp), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        compiled = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        (output / (label + '-compile.log')).write_text(compiled.stdout)
        if compiled.returncode:
            raise RuntimeError('strict compile failed: ' + label + '\n' + compiled.stdout)
        run = subprocess.run([str(binary), case, str(output / (label + '.wav'))], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=120)
        (output / (label + '-contracts.log')).write_text(run.stdout)
        unchanged()
        inputs = {}
        for path in INPUTS:
            data = subprocess.check_output(['git', 'show', ref + ':' + path], cwd=ROOT) \
                if ref and path.startswith('compat/') else snapshot[path]
            inputs[path] = hashlib.sha256(data).hexdigest()
        receipts.append({'label': label, 'source_ref': ref, 'case': case, 'command': command,
                         'generated_sha256': hashlib.sha256(cpp.read_bytes()).hexdigest(),
                         'executable_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                         'returncode': run.returncode, 'inputs': inputs})
        (output / 'source.json').write_text(json.dumps(receipts, indent=2) + '\n')
        return run

    current = source(args.source_ref)
    run = execute('actual-blocked-write', current, args.case, args.source_ref)
    print(run.stdout, end='', flush=True)
    if run.returncode or not args.negative_controls:
        return run.returncode
    current_hz = re.findall(r'sound \d+: start [\d.]+ s, duration [\d.]+ s, ([\d.]+) Hz', run.stdout)
    if len(current_hz) != 1 or abs(float(current_hz[0]) - 750) > 750 * 0.02:
        raise RuntimeError('unchanged production Analyze and the existing 2% gate reject current capture')
    controls = [
        ('unchanged-baseline', source(BASELINE), 'gap', BASELINE,
         'prepared blocking input remains continuous across a catch-up mix'),
        ('unchanged-capture', source(BASELINE), 'capture', BASELINE,
         'actual capture writes every prepared tone frame without an inserted interruption'),
        ('no-refill', altered(current,
            'if (read_ == written_ && state_ == State::kPlaying && !released_) {',
            'if (false && read_ == written_ && state_ == State::kPlaying && !released_) {'),
         'gap', None, 'prepared blocking input remains continuous across a catch-up mix'),
        ('stale-epoch', altered(current,
            'while (pending_write_head_ != nullptr &&\n             pending_write_head_->interrupt != write_interrupt_) {',
            'while (false && pending_write_head_ != nullptr &&\n             pending_write_head_->interrupt != write_interrupt_) {'),
         'epoch', None, "a restarted stream cannot borrow an interrupted writer's remaining input"),
        ('lost-progress', altered(current, 'done += pending.committed;', 'done += 0;'),
         'progress', None, 'assisted byte progress is counted once when interrupted'),
        ('lost-unlink', altered(current, '~PendingWrite() { Unlink(); }', '~PendingWrite() {}'),
         'cancel', None, 'cancelled writer unlinks borrowed input before caller storage can retire'),
        ('old-buffer-limit', altered(current,
            'size_t Track::CopyToRing(const uint8_t* source, size_t frames) {\n  size_t limit = std::min(buffer_frames_, params_.capacity_frames);',
            'size_t Track::CopyToRing(const uint8_t* source, size_t frames) {\n  size_t limit = params_.capacity_frames;'),
         'shrink', None, 'assisted commit respects the smaller current ring limit'),
        ('reverse-waiters', altered(current,
            'if (PendingWrite* pending = pending_write_head_) {',
            'if (PendingWrite* pending = pending_write_tail_) {'),
         'multiple', None, 'currently registered waiters are refilled in their list order'),
        ('repeated-prefix', altered(current,
            'pending->data + pending->committed * frame_size_', 'pending->data'),
         'shrink', None, 'ring wrap and limit changes preserve full source byte order'),
    ]
    for name, body, case, ref, assertion in controls:
        result = execute(name, body, case, ref)
        errors = [line for line in result.stdout.splitlines() if line.startswith('FAIL ')]
        expected = 'FAIL blocked audio write: ' + assertion
        if result.returncode != 1 or not errors or any(line != expected for line in errors):
            raise RuntimeError('negative control must fail only its ownership assertion: ' + name + '\n' + result.stdout)
        if name == 'unchanged-capture':
            old_hz = re.findall(r'sound \d+: start [\d.]+ s, duration [\d.]+ s, ([\d.]+) Hz', result.stdout)
            if len(old_hz) != 1 or abs(float(old_hz[0]) - 750) <= 750 * 0.02:
                raise RuntimeError('unchanged 2% frequency oracle must reject the real baseline interruption')
        print('PASS negative ' + name + ': ' + assertion, flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
