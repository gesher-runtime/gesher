#!/usr/bin/env python3
"""Run the complete WindowsHost dispatcher with production native event wait/post.

Windows also executes the production hidden window and startup code. Linux has
explicit native window/condition-variable boundaries and is not Windows evidence.
The default 65-second idle interval crosses the former one-minute empty wake.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source: str, marker: str) -> str:
    start = source.index(marker); end = source.index('{', start) + 1; depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref', help='previous complete WindowsHost as idle-wake control')
    parser.add_argument('--idle-seconds', type=int, default=65)
    parser.add_argument('--cxx', default='cl' if os.name == 'nt' else 'c++')
    args = parser.parse_args()
    java_path = 'app/java/com/gesher/server/WindowsHost.java'
    java = (subprocess.check_output(['git', 'show', args.source_ref + ':' + java_path],
            cwd=ROOT) if args.source_ref else (ROOT / java_path).read_bytes())
    native = (ROOT / 'compat/devsvc/devsvc_win32.cc').read_text(encoding='utf-8')
    print('WindowsHost.java SHA256', hashlib.sha256(java).hexdigest(), flush=True)
    print('devsvc_win32.cc SHA256', hashlib.sha256(native.encode()).hexdigest(), flush=True)
    window = native[native.index('struct Activation {'):native.index('// --- COM')]
    template = (ROOT / 'tests/windows-host-events/events_jni.cc.in').read_text(encoding='utf-8')
    for marker, replacement in {
            '@WINDOW@': window,
            '@START@': method(native, 'unsigned long long StartEvents()'),
            '@IDLE@': method(native, 'uint32_t IdleMillis()') + '\n' + method(native, 'uint64_t JobIdleMillis()'),
            '@POST@': method(native, 'void Post(unsigned events)'),
            '@WAIT@': method(native, 'unsigned WaitEvents(int timeout_ms)')}.items():
        template = template.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix='gesher-windows-events-') as temporary:
        work = Path(temporary); classes = work / 'classes'; classes.mkdir()
        source = work / 'WindowsHost.java'; source.write_bytes(java)
        sources = [source, ROOT / 'tests/windows-host-events/WindowsHostEventsHost.java']
        for name, body in {
                'Process': 'public static final int SYSTEM_UID = 1000;',
                'SystemClock': 'public static long uptimeMillis() { return System.nanoTime()/1000000; }'}.items():
            path = work / (name + '.java')
            path.write_text('package android.os; public final class ' + name + ' { ' + body + ' }')
            sources.append(path)
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                '-d', str(classes), *map(str, sources)], check=True, timeout=60)
        cpp = work / 'events_jni.cc'; cpp.write_text(template, encoding='utf-8')
        library = work / ('windows_events.dll' if os.name == 'nt' else 'libwindows_events.so')
        includes = [args.java_home / 'include', args.java_home / 'include' / ('win32' if os.name == 'nt' else 'linux'),
                    ROOT / 'compat/devsvc']
        cxx = shlex.split(args.cxx)
        if Path(cxx[0]).name.lower() in ('cl', 'cl.exe'):
            command = cxx + ['/nologo', '/std:c++17', '/EHsc', '/W4', '/WX', '/LD',
                    '/D_WIN32_WINNT=0x0a00', *('/I' + str(path) for path in includes), str(cpp),
                    '/Fe:' + str(library), '/Fo:' + str(work) + '/', '/link', 'user32.lib']
        else:
            command = cxx + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-pthread',
                    *('-I' + str(path) for path in includes), str(cpp), '-o', str(library)]
        subprocess.run(command, check=True, timeout=60)
        for case in ('unavailable', 'deadline', 'dispatch'):
            subprocess.run([str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '-cp', str(classes),
                    'com.gesher.server.WindowsHostEventsHost', str(library), case, str(args.idle_seconds)],
                    check=True, timeout=max(20, args.idle_seconds + 30))


if __name__ == '__main__':
    main()
