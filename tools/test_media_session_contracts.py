#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise production MediaSessionService contracts and zero-listener allocations.

Compiles the unchanged production MediaSessionService.java and StandIn.java against
minimal declared Android Binder/MediaSession/AudioService host boundaries. No
session selection, death retirement, volume fallback or callback broadcast logic is
substituted. Real Binder IPC and framework Parcel transport run in Windows/ART suites.
--source-ref compiles an older git revision of MediaSessionService.java as a
negative control or (with --measure-only) an ABBA allocation baseline.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def resolve_java_home(explicit: Path | None) -> Path:
    if explicit is not None:
        return explicit
    env = os.environ.get('JAVA_HOME')
    if env:
        return Path(env)
    javac = shutil.which('javac')
    if javac:
        return Path(javac).resolve().parent.parent
    raise SystemExit('error: --java-home or JAVA_HOME is required')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, default=None)
    parser.add_argument('--source-ref', help='Compile MediaSessionService.java from this git ref')
    parser.add_argument('--measure-only', action='store_true',
                        help='Run only the steady-state zero-listener allocation measurement')
    args = parser.parse_args()
    java_home = resolve_java_home(args.java_home)
    javac = str(java_home / 'bin/javac')
    java = str(java_home / 'bin/java')

    def read(relative: str, ref: str | None = None) -> str:
        if ref:
            return subprocess.run(['git', 'show', ref + ':' + relative], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / relative).read_text(encoding='utf-8')

    service_source = read('app/java/com/gesher/server/MediaSessionService.java', args.source_ref)
    standin_source = read('app/java/com/gesher/server/StandIn.java')
    print('MediaSessionService production SHA256 '
          + hashlib.sha256(service_source.encode()).hexdigest()
          + ' source ' + (args.source_ref or 'working-tree'), flush=True)

    template = (ROOT / 'tests/media/MediaSessionHost.java.in').read_text(encoding='utf-8')
    chunks = re.split(r'^// FILE: ([^\n]+)\n', template, flags=re.MULTILINE)
    if len(chunks) < 3:
        raise ValueError('No // FILE: units found in MediaSessionHost.java.in')

    with tempfile.TemporaryDirectory(prefix='gesher-media-session-') as temporary:
        work = Path(temporary)
        src_dir = work / 'src'
        classes = work / 'classes'
        classes.mkdir(parents=True)
        sources: list[str] = []

        def emit(relative: str, content: str) -> None:
            target = src_dir / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
            sources.append(str(target))

        emit('com/gesher/server/MediaSessionService.java', service_source)
        emit('com/gesher/server/StandIn.java', standin_source)
        for path, text in zip(chunks[1::2], chunks[2::2]):
            emit(path.strip(), text)

        subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-d', str(classes), *sources], check=True, timeout=90)

        vm_flags = ['-XX:-UsePerfData', '-Xint', '-Xmx128m'] if args.measure_only else [
            '-XX:-UsePerfData', '-XX:-DoEscapeAnalysis', '-Xmx128m']
        cmd = [java, *vm_flags, '-cp', str(classes), 'com.gesher.server.MediaSessionHost']
        if args.measure_only:
            cmd.append('--measure-only')
        subprocess.run(cmd, check=True, timeout=90)


if __name__ == '__main__':
    main()
