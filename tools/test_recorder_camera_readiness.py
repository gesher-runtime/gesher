#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run the actual recorderCamera test method with controlled camera startup.

The Android camera/recorder API and codec output are recording adapters. The
fixture's complete method, callback bodies, real latches, callback thread and
cleanup execute unchanged apart from explicit wait/clock boundaries. A delayed
capture and an expired readiness deadline need no scheduler-dependent sleeps.
This checks fixture ordering and cleanup, not native capture or encoded video.
--source-ref runs the same checks against the old fixture as a negative control.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

from test_install_metadata_comparison import method

ROOT = Path(__file__).resolve().parents[1]


def run(java_home: Path, source_ref: str | None = None, case: str = 'all') -> None:
    path = 'tests/apps/recordapp/java/com/gesher/recordapp/RecordActivity.java'
    source = (subprocess.check_output(['git', 'show', source_ref + ':' + path],
                                     cwd=ROOT, text=True) if source_ref else
              (ROOT / path).read_text(encoding='utf-8'))
    parts = []
    if 'private interface CameraCleanup' in source:
        parts += [method(source, 'private interface CameraCleanup'),
                  method(source, 'private static Throwable cleanupCamera(')]
    parts.append(method(source, 'void recorderCamera()'))
    exact = '\n'.join(parts)
    print('Recorder camera fixture method SHA256', hashlib.sha256(exact.encode()).hexdigest(), flush=True)
    # Only scheduling/time boundaries change. In particular, no latch release,
    # callback, request, recorder operation or cleanup is inserted into the body.
    if exact.count('Thread.sleep(2000);') != 1:
        raise ValueError('recorder camera collection boundary changed')
    exact = exact.replace('Thread.sleep(2000);', 'Boundary.sleep(2000);')
    exact = exact.replace('System.nanoTime()', 'Boundary.nanoTime()')
    wait = 'firstCapture.await(10, TimeUnit.SECONDS)'
    if exact.count(wait) > 1:
        raise ValueError('recorder camera readiness boundary changed')
    exact = exact.replace(wait, 'Boundary.await(firstCapture, 10, TimeUnit.SECONDS)')
    fixture = (ROOT / 'tests/media/RecorderCameraReadinessHost.java.in').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='gesher-recorder-camera-readiness-') as temporary:
        work = Path(temporary)
        target = work / 'RecorderCameraReadinessHost.java'
        target.write_text(fixture.replace('@SUBJECT@', exact), encoding='utf-8')
        subprocess.run([str(java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-d', str(work), str(target)], check=True, timeout=60)
        subprocess.run([str(java_home / 'bin/java'), '-XX:-UsePerfData', '-Xmx128m',
                        '-cp', str(work), 'RecorderCameraReadinessHost', case],
                       check=True, timeout=20)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--case', choices=('all', 'delayed', 'absent'), default='all')
    args = parser.parse_args()
    run(args.java_home, args.source_ref, args.case)


if __name__ == '__main__':
    main()
