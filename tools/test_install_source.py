#!/usr/bin/env python3
"""Exercise actual install-source policy/registry with verified APKs and explicit host boundaries."""
from __future__ import annotations
import argparse, os, shutil, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'apksig'):
        p.add_argument('--' + key, required=True, type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    work = args.output or Path(tempfile.mkdtemp(prefix='gesher-install-source-'))
    work.mkdir(parents=True, exist_ok=True)
    classes = work / 'classes'; classes.mkdir(exist_ok=True)
    cp = os.pathsep.join(str(v.resolve()) for v in
            (args.app_classes, args.framework_classes, args.core_classes, args.apksig))
    library = work / 'libinstall_source_host.so'
    commands = [
        ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared', '-fPIC',
         '-I' + str(args.java_home / 'include'), '-I' + str(args.java_home / 'include/linux'),
         str(ROOT / 'tests/location/framework_host.c'), '-o', str(library)],
        [str(args.java_home / 'bin/javac'), '-encoding', 'UTF-8', '-cp', cp, '-d', str(classes),
         str(ROOT / 'tests/package/InstallSourceHost.java')]]
    for cmd in commands: subprocess.run(cmd, check=True)
    cleaner = work / 'cleaner/sun/misc'; cleaner.mkdir(parents=True, exist_ok=True)
    for f in (args.core_classes / 'sun/misc').glob('Cleaner*.class'): shutil.copy2(f, cleaner / f.name)
    command = [str(args.java_home / 'bin/java'), '-XX:-UsePerfData', '--patch-module',
            'jdk.unsupported=' + str(work / 'cleaner'), '-cp', str(classes) + os.pathsep + cp,
            'com.gesher.server.InstallSourceHost', str(library.resolve()), str(ROOT / 'tests/dex-services/fixtures')]
    for mode in ('0', '1'):
        env = os.environ.copy(); env['ANDROID_DATA'] = str((work / ('data-' + mode)).resolve())
        env['GESHER_MICROG_SPOOF'] = mode; env.pop('GESHER_BINDER_UID', None)
        subprocess.run(command, env=env, check=True, timeout=90)
if __name__ == '__main__': main()
