#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run exact Camera2 capture-loop/open methods at declared device/framework seams.

The configuration-time open (endConfigure) and the first request's retry of a
failed open run through the same production methods.

Real Java maps, locking, iteration and production delivery/control flow remain.
Native capture, framework metadata/surfaces, callbacks and request admission are
adapters. Counts describe map queries in the loop, not physical-camera latency.
--source-ref validates old behavior; --measure omits the redundant-scan gate.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--measure', action='store_true')
    args = parser.parse_args()
    path = 'app/java/com/gesher/server/CameraDeviceUser.java'
    source = subprocess.check_output(['git', 'show', args.source_ref + ':' + path],
                                     cwd=ROOT, text=True) if args.source_ref else (
                                         ROOT / path).read_text(encoding='utf-8')
    exact = '\n'.join(method(source, marker) for marker in (
        'private void captureFrames()',
        'private Size configuredSize()',
        'private void openForConfiguration()',
        'private void open(Size size)',
        'private static final class CaptureTiming',
    ))
    print('Camera2 capture dispatch production SHA256', hashlib.sha256(exact.encode()).hexdigest(), flush=True)
    fixture = (ROOT / 'tests/java-performance/Camera2CaptureDispatchHost.java.in').read_text(encoding='utf-8')
    fixture = fixture.replace('@PRODUCTION@', exact).replace('@SCAN_GATE@', str(not args.measure).lower())
    with tempfile.TemporaryDirectory(prefix='gesher-camera2-dispatch-') as temporary:
        work = Path(temporary)
        target = work / 'Camera2CaptureDispatchHost.java'
        target.write_text(fixture, encoding='utf-8')
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-d', str(work), str(target)], check=True, timeout=60)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m',
                        '-cp', str(work), 'com.gesher.server.Camera2CaptureDispatchHost'],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
