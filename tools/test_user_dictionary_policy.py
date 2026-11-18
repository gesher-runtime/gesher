#!/usr/bin/env python3
"""Run the production UserDictionary denial and metadata paths with host JNI boundaries.

The actual SQLite data contract is UserDictionaryContractTest on ART/Android.
This runner never substitutes a host database implementation for Android SQLite.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig-jar'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--source-ref', help='Compile older PMS/permission policy as a negative control')
    parser.add_argument('--case', choices=('all', 'visibility', 'cross-user-outbound',
                                          'cross-user-inbound', 'cross-user-permissions',
                                          'local-permissions'), default='all')
    args = parser.parse_args()
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes, args.apksig_jar))
    cc = shutil.which('cc')
    if not cc:
        parser.error('A C compiler is needed for declared native boundaries')
    with tempfile.TemporaryDirectory(prefix='dictionary-policy-') as temporary:
        work = Path(temporary)
        classes = work / 'classes'
        classes.mkdir()
        library = work / 'libdictionary_policy.so'
        subprocess.run([cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
                        '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
                        str(ROOT / 'tests/user-dictionary/framework_host.c'), '-o', str(library)], check=True)
        sources = [ROOT / 'tests/user-dictionary' / (name + '.java') for name in
                   ('UserDictionaryPolicyHost', 'ProviderEnumerationPolicyHost')]
        for name in ('UserDictionaryProvider', 'PackageManagerService', 'ContentProviders',
                     'PackageVisibility', 'Installer', 'PermissionPolicy'):
            relative = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / relative
            if name in ('PackageManagerService', 'PermissionPolicy') and args.source_ref:
                previous = subprocess.run(['git', 'show', args.source_ref + ':' + relative],
                                          cwd=ROOT, check=True, capture_output=True)
                source = work / (name + '.java')
                source.write_bytes(previous.stdout)
            sources.append(source)
        subprocess.run([str(args.java_home / 'bin/javac'), '-J-XX:-UsePerfData', '-encoding', 'UTF-8',
                        '-cp', classpath, '-d', str(classes), *map(str, sources)], check=True)
        cleaner = work / 'cleaner' / 'sun' / 'misc'
        cleaner.mkdir(parents=True)
        for source in (args.core_classes / 'sun/misc').glob('Cleaner*.class'):
            shutil.copy2(source, cleaner / source.name)
        for mode in ('0', '1'):
            print('UserDictionary policy mode=' + mode + '; case=' + args.case, flush=True)
            env = dict(os.environ, GESHER_MICROG_SPOOF=mode, ANDROID_DATA=str(work / ('data-' + mode)))
            command = [str(args.java_home / 'bin/java'), '-XX:-UsePerfData',
                       '-XX:+UnlockDiagnosticVMOptions', '-XX:-BytecodeVerificationRemote',
                       '--patch-module', 'jdk.unsupported=' + str(work / 'cleaner'),
                       '-cp', str(classes) + os.pathsep + classpath]
            if args.case == 'all':
                subprocess.run(command + ['com.gesher.server.UserDictionaryPolicyHost', str(library)],
                               env=env, check=True, timeout=90)
            subprocess.run(command + ['com.gesher.server.ProviderEnumerationPolicyHost', str(library),
                                      args.case, str(ROOT / 'tests/dex-services/fixtures')],
                           env=env, check=True, timeout=90)


if __name__ == '__main__':
    main()
