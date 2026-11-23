#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production WebView geometry collection, publication and JNI decoding.

The complete geometry methods and their fields are extracted without rewriting.
Android View/Outline/Path and the Java native call are declared host boundaries;
traversal, shape serialization, equality, reentry ownership and Java allocation
are production code. The emitted records also run through the actual JNI shape
decoder with a small JNI/Host adapter. No Windows/ART/WebView2 claim is made.
--compare-ref checks byte-identical non-reentrant frame transcripts against an
older source. --source-ref with --case measurement is an allocation control;
--measure prints its result without the new allocation gates.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--compare-ref')
    parser.add_argument('--case', choices=('all', 'behavior', 'reentry', 'recovery', 'measurement'), default='all')
    parser.add_argument('--measure', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path, help='retain frame transcript for independent/native checks')
    args = parser.parse_args()

    def read(path: str, ref: str | None) -> str:
        if ref:
            return subprocess.check_output(['git', 'show', ref+':'+path], cwd=ROOT, text=True)
        return (ROOT/path).read_text(encoding='utf-8')

    def build_java(work: Path, ref: str | None) -> Path:
        work.mkdir()
        production = read('app/java/com/gesher/app/webview/GesherWebViewProvider.java', ref)
        field_start = production.index('    // Bounds (the WebView\'s Looper).')
        fields = production[field_start:production.index('    private volatile CountDownLatch mParked;', field_start)]
        start = production.index('    /** Moves the WebView2 window to the view\'s place')
        geometry = production[start:production.index('    // ---- ViewDelegate ----', start)]
        exact = fields + '\n' + geometry
        print('Geometry production SHA256', hashlib.sha256(exact.encode()).hexdigest(),
              'source', ref or 'working-tree', flush=True)
        template = (ROOT/'tests/java-performance/WebViewGeometryHost.java.in').read_text(encoding='utf-8')
        source = work/'WebViewGeometryHost.java'
        source.write_text(template.replace('@PRODUCTION@', exact), encoding='utf-8')
        boundary = (ROOT/'tests/java-performance/WebViewGeometryAndroid.java.in').read_text(encoding='utf-8')
        chunks = re.split(r'^// FILE: ([^\n]+)\n', boundary, flags=re.MULTILINE)
        sources = [str(source)]
        for path, text in zip(chunks[1::2], chunks[2::2]):
            target = work/path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding='utf-8')
            sources.append(str(target))
        subprocess.run([str(args.java_home/'bin/javac'), '-J-XX:-UsePerfData', '-source', '11',
                        '-target', '11', '-Xlint:-options', '-encoding', 'UTF-8', '-d', str(work),
                        *sources], check=True, timeout=60)
        return work

    def run_java(work: Path, case: str, frames: Path) -> None:
        # Interpreter allocation counters avoid both scalar replacement hiding an
        # old-source array and unrelated tiered-JIT/deoptimization allocations.
        vm = ['-Xint'] if case == 'measurement' else ['-XX:-DoEscapeAnalysis']
        subprocess.run([str(args.java_home/'bin/java'), '-XX:-UsePerfData', *vm,
                        '-Xmx128m', '-cp', str(work), 'com.gesher.app.webview.WebViewGeometryHost',
                        case, 'measure' if args.measure else 'gate', str(frames)], check=True, timeout=60)

    with tempfile.TemporaryDirectory(prefix='gesher-webview-geometry-') as temporary:
        work = Path(temporary)
        current = build_java(work/'current', args.source_ref)
        frames = work/'frames.txt'
        for case in ('behavior', 'reentry', 'recovery', 'measurement') if args.case == 'all' else [args.case]:
            run_java(current, case, frames)
        if args.compare_ref:
            baseline = build_java(work/'baseline', args.compare_ref)
            previous = work/'baseline-frames.txt'
            # Always run behavior to make the comparison independent of --case.
            run_java(current, 'behavior', frames)
            run_java(baseline, 'behavior', previous)
            if frames.read_bytes() != previous.read_bytes():
                raise AssertionError('production geometry differs from baseline frame transcript')
            print('Exact frame transcript matches', args.compare_ref, hashlib.sha256(frames.read_bytes()).hexdigest(), flush=True)
        if frames.exists():
            native = read('compat/webview/webview_jni.cc', args.source_ref)
            decoder = method(native, 'void JNICALL WebView2Host_setBounds(')
            print('Geometry JNI decoder SHA256', hashlib.sha256(decoder.encode()).hexdigest(), flush=True)
            fixture = (ROOT/'tests/webview/geometry_native_host.cc.in').read_text(encoding='utf-8')
            cpp = work/'native.cc'
            cpp.write_text(fixture.replace('@JNI_DECODER@', decoder), encoding='utf-8')
            exe = work/('native.exe' if os.name == 'nt' else 'native')
            command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                               str(cpp), '-o', str(exe)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(exe), str(frames)], check=True, timeout=60)
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_bytes(frames.read_bytes())


if __name__ == '__main__':
    main()
