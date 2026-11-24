#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Issue #335: Play services, Google accounts, tokens and push only where they exist.

Compiles the host fixtures in tests/gms-truthful against the unchanged production
classes (--app-classes) and runs each one. With --source-ref, the named production
sources are taken from that git ref and compiled over --app-classes first: the
negative control that shows each host fails on the code it replaces.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# host -> (JNI boundary, production sources a --source-ref replaces)
HOSTS = {
    'GmsPackageTruthHost': ('tests/uri/framework_host.c', (
        'app/java/com/gesher/server/PackageManagerService.java',
        'app/java/com/gesher/server/PackageVisibility.java',
        'app/java/com/gesher/server/ActiveServices.java')),
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'app-classes', 'apksig'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--core-classes', type=Path,
                        help='libcore classes (the coremod); omit when --framework-classes carries them')
    parser.add_argument('--source-ref', help='negative control: production sources from this git ref')
    parser.add_argument('--host', action='append', choices=sorted(HOSTS),
                        help='run only these hosts (default: all)')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is required for the native host boundaries')
    deps = [args.app_classes, args.framework_classes]
    if args.core_classes:
        deps.append(args.core_classes)
    deps.append(args.apksig)
    classpath = os.pathsep.join(str(p.resolve()) for p in deps)
    javac = str(args.java_home / 'bin/javac')
    java = str(args.java_home / 'bin/java')
    failures = []
    with tempfile.TemporaryDirectory(prefix='gesher-gms-truthful-') as directory:
        work = Path(directory)
        cleaner = None
        if args.core_classes and (args.core_classes / 'sun/misc').is_dir():
            cleaner = work / 'cleaner'
            (cleaner / 'sun/misc').mkdir(parents=True)
            for entry in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
                shutil.copy2(entry, cleaner / 'sun/misc' / entry.name)
        for host in args.host or sorted(HOSTS):
            boundary, production = HOSTS[host]
            out = work / host; classes = out / 'classes'; classes.mkdir(parents=True)
            library = out / 'libgms_truthful_host.so'
            subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                            '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                            str(ROOT / boundary), '-o', str(library)], check=True)
            sources = []
            if args.source_ref:
                for relative in production:
                    old = subprocess.run(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT,
                                         check=True, capture_output=True).stdout
                    target = out / 'old' / relative; target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(old); sources.append(str(target))
            sources.append(str(ROOT / 'tests/gms-truthful' / (host + '.java')))
            subprocess.run([javac, '-J-XX:-UsePerfData', '-nowarn', '-encoding', 'UTF-8', '-cp', classpath,
                            '-d', str(classes), *sources], check=True)
            command = [java, '-XX:-UsePerfData']
            if cleaner:
                command += ['--patch-module', 'jdk.unsupported=' + str(cleaner)]
            command += ['-cp', str(classes) + os.pathsep + classpath, host, str(library)]
            for spoof in ('0', '1'):
                env = os.environ.copy(); env['ANDROID_DATA'] = str(out / ('data-' + spoof))
                env['GESHER_MICROG_SPOOF'] = spoof; env.pop('GESHER_BINDER_UID', None)
                print(f'== {host} (GESHER_MICROG_SPOOF={spoof})', flush=True)
                if subprocess.run(command, env=env, timeout=90).returncode != 0:
                    failures.append(f'{host} spoof={spoof}')
    if failures:
        raise SystemExit('FAILED: ' + ', '.join(failures))


if __name__ == '__main__':
    main()
