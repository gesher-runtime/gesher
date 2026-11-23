#!/usr/bin/env python3
"""Exercise production Java settings, Proxy dispatch and native-library discovery.

Settings File*Stream/RandomAccessFile and ZIP constructor sites use counting
subclasses which delegate to real host file I/O; explicit settings fault/crash
controls cover write, sync, close and replacement failures. No storage, encoding
or discovery algorithm is substituted. StandIn uses small Binder/list-slice host boundaries and the actual
Java Proxy/reflection path. Its portable behavioral fixture also runs on ART.
HotSpot allocation timing is informational; a generous allocation ceiling detects
the original per-call method lookup. --source-ref runs the same performance gates
against an older production source as a negative control.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/java-performance'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, required=True)
    parser.add_argument('--source-ref')
    parser.add_argument('--component', choices=('settings', 'standin', 'native'))
    parser.add_argument('--settings-benchmark', action='store_true',
                        help='Measure the same settings mutation workload, including older source refs')
    args = parser.parse_args()
    java = str(args.java_home / 'bin/java')
    javac = str(args.java_home / 'bin/javac')

    def read(relative: str) -> str:
        if args.source_ref:
            return subprocess.run(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / relative).read_text(encoding='utf-8')

    for component in [args.component] if args.component else ['settings', 'standin', 'native']:
        with tempfile.TemporaryDirectory(prefix='gesher-java-performance-') as temporary:
            work = Path(temporary)
            classes = work / 'classes'; classes.mkdir()
            sources = []
            def write(relative: str, content: str) -> str:
                target = work / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content, encoding='utf-8')
                sources.append(str(target))
                return str(target)

            name = {'settings': 'server/SettingsStore', 'standin': 'server/StandIn',
                    'native': 'app/NativeLibs'}[component]
            source = read('app/java/com/gesher/' + name + '.java')
            print(component + ' production SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
            if component == 'settings':
                for kind in ['Input', 'Output']:
                    needle = 'new File' + kind + 'Stream('
                    if source.count(needle) != 1:
                        raise ValueError('Expected one file-stream construction boundary: ' + needle)
                    source = source.replace(needle, 'new CountingFile' + kind + 'Stream(')
                # Real filesystem adapters provide explicit append/sync/replace fault
                # boundaries. Older GST1 sources have no RandomAccessFile constructor.
                source = source.replace('new RandomAccessFile(', 'new CountingRandomAccessFile(')
                source = source.replace('bytes.getFD().sync()', 'SettingsIO.sync(bytes.getFD())')
                source = source.replace('Files.move(', 'SettingsIO.move(')
                sources.append(str(FIXTURES / 'SettingsStoreHost.java'))
                main_classes = ['com.gesher.server.SettingsStoreHost']
            elif component == 'standin':
                write('android/os/IInterface.java', 'package android.os; public interface IInterface { IBinder asBinder(); }')
                write('android/os/IBinder.java', 'package android.os; public interface IBinder {}')
                write('android/os/Binder.java', 'package android.os; public class Binder implements IBinder { public void attachInterface(IInterface impl, String descriptor) {} }')
                write('android/content/pm/ParceledListSlice.java', 'package android.content.pm; public class ParceledListSlice<T> { private final java.util.List<T> values; public ParceledListSlice(java.util.List<T> values) { this.values=values; } public java.util.List<T> getList() { return values; } }')
                sources += [str(ROOT / 'tests/dex-services/gesher/services/StandInDispatchTest.java'),
                            str(FIXTURES / 'StandInAllocationsHost.java')]
                main_classes = ['gesher.services.StandInDispatchTest', 'gesher.performance.StandInAllocationsHost']
            else:
                # Preserve java.util.zip.ZipFile for types; instrument only its constructor.
                needle = 'import java.util.zip.ZipFile;'
                if source.count(needle) != 1 or source.count('new ZipFile(') != 2:
                    raise ValueError('ZIP construction boundaries changed')
                source = source.replace(needle, needle + '\nimport gesher.performance.CountingZipFile;')
                source = source.replace('new ZipFile(', 'new CountingZipFile(')
                sources += [str(FIXTURES / 'CountingZipFile.java'), str(FIXTURES / 'NativeLibraryScanHost.java')]
                main_classes = ['gesher.performance.NativeLibraryScanHost']
            write('com/gesher/' + name + '.java', source)
            subprocess.run([javac, '-J-XX:-UsePerfData', '-encoding', 'UTF-8', '-d', str(classes), *sources],
                           check=True, timeout=60)
            for main_class in main_classes:
                command = [java, '-XX:-UsePerfData', '-Xmx256m', '-cp', str(classes), main_class]
                if main_class == 'gesher.services.StandInDispatchTest':
                    command.append('--host')
                if main_class == 'com.gesher.server.SettingsStoreHost' and args.settings_benchmark:
                    command.append('--benchmark-only')
                subprocess.run(command, check=True, timeout=60)


if __name__ == '__main__':
    main()
