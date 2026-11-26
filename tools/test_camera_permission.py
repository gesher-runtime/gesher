#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run complete camera admission code with explicit package/device host boundaries.

Fresh app classes are required. --source-ref compiles the two camera entry classes
from that ref into the same fixture, so the former manifest-only defect fails a
behavior assertion. No physical camera, remote Binder, or ART execution is claimed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    compiler = shutil.which('cc')
    if not compiler:
        parser.error('A C compiler is required for the declared host JNI boundaries')
    classpath = os.pathsep.join(str(path.resolve()) for path in
            (args.app_classes, args.framework_classes, args.core_classes))
    with tempfile.TemporaryDirectory(prefix='gesher-camera-permission-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'camera-permission.so'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'),
                        '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/camera-permission/framework_host.c'), '-o', str(library)],
                       check=True, timeout=30)
        sources = [ROOT / 'tests/dex-services/gesher/services/CameraPermissionContractTest.java',
                   ROOT / 'tests/camera-permission/CameraPermissionHost.java']
        if args.source_ref:
            for name in ('CameraService', 'LegacyCamera'):
                path = 'app/java/com/gesher/server/' + name + '.java'
                source = work / (name + '.java')
                source.write_text(subprocess.run(['git', 'show', args.source_ref + ':' + path],
                        cwd=ROOT, check=True, capture_output=True, text=True).stdout, encoding='utf-8')
                sources.append(source)
        subprocess.run([str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-cp', classpath,
                        '-d', str(classes), *map(str, sources)], check=True, timeout=60)
        cleaner = work / 'cleaner/sun/misc'
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '--patch-module',
                        'jdk.unsupported=' + str(work / 'cleaner'), '-cp',
                        str(classes) + os.pathsep + classpath, 'CameraPermissionHost', str(library)],
                       check=True, timeout=60)


if __name__ == '__main__':
    main()
