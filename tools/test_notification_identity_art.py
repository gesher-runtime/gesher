#!/usr/bin/env python3
"""Narrow Windows/ART notification storage and PNG identity check in an isolated profile.

Uses the supplied built runtime for real Parcel/Bitmap natives; compiles changed
production classes into a DEX overlay placed before the existing app on the BCP.
No installed application, user profile, real toast UI or PendingIntent is changed.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'runtime', 'd8-jar', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--source-ref', help='Compile unchanged production source as a negative control')
    a = p.parse_args()
    runtime, work = a.runtime.resolve(), a.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    classes, dex = work / 'classes', work / 'dex'
    classes.mkdir(); dex.mkdir()
    java = str(a.java_home / 'bin/java')
    cp = os.pathsep.join(str(x.resolve()) for x in (a.app_classes, a.framework_classes, a.core_classes))
    sources = []
    for name in ('NotificationIdentity', 'NotificationStore', 'NotificationIcons'):
        path = 'app/java/com/gesher/server/' + name + '.java'
        source = ROOT / path
        if a.source_ref and name != 'NotificationIdentity':
            source = work / (name + '.java')
            source.write_bytes(subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=ROOT))
        sources.append(source)
    sources.append(ROOT / 'tests/dex-services/com/gesher/server/NotificationIdentityDevice.java')
    subprocess.run([str(a.java_home / 'bin/javac'), '-source', '17', '-target', '17',
        '-XDstringConcat=inline', '-encoding', 'UTF-8', '-cp', cp, '-d', str(classes), *map(str, sources)], check=True)
    archive = work / 'overlay-classes.jar'
    subprocess.run([str(a.java_home / 'bin/jar'), 'cf', str(archive), '-C', str(classes), '.'], check=True)
    subprocess.run([java, '-cp', str(a.d8_jar), 'com.android.tools.r8.D8', '--release',
        '--min-api', '36', '--output', str(dex), str(archive)], check=True)
    overlay = work / 'overlay.jar'
    subprocess.run([str(a.java_home / 'bin/jar'), 'cf', str(overlay), '-C', str(dex), 'classes.dex'], check=True)
    for part in ('system/framework', 'system/etc', 'system/fonts', 'data', 'art', 'i18n/etc/icu', 'logs'):
        (work / part).mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime / 'build.prop', work / 'system/build.prop')
    shutil.copy2(runtime / 'framework-res.apk', work / 'system/framework/framework-res.apk')
    for path in (runtime / 'fonts').glob('*'):
        if path.is_file(): shutil.copy2(path, work / 'system/fonts' / path.name)
    shutil.copy2(runtime / 'fonts/fonts.xml', work / 'system/etc/fonts.xml')
    icu = next(runtime.glob('icudt*l.dat'))
    shutil.copy2(icu, work / 'i18n/etc/icu' / icu.name)

    def guest(path):
        value = str(path.resolve()).replace('\\', '/')
        return value[2:] if value[:2].lower() == str(runtime)[:2].lower() else '/mnt/' + value[0].lower() + value[2:]

    env = {k: v for k, v in os.environ.items() if not k.startswith(('GESHER_', 'ANDROID_'))}
    env.update(ANDROID_ROOT=guest(work / 'system'), ANDROID_DATA=guest(work / 'data'),
        ANDROID_ART_ROOT=guest(work / 'art'), ANDROID_I18N_ROOT=guest(work / 'i18n'),
        ANDROID_TZDATA_ROOT=guest(runtime / 'tzdata'), GESHER_LOGS=guest(work / 'logs'),
        GESHER_MICROG_SPOOF='0')
    boot = [runtime / (name + '.jar') for name in ('core-oj', 'core-libart', 'core-icu4j', 'framework')]
    boot.extend((overlay, runtime / 'gesher-app.jar'))
    for phase in ('write', 'read'):
        command = [str(runtime / 'dalvikvm.exe'), '-Xbootclasspath:' + ':'.join(map(guest, boot)),
            '-Ximage:none', '-Xint', '-Xusejit:false', 'com.gesher.server.NotificationIdentityDevice', phase]
        result = subprocess.run(command, cwd=runtime, env=env, capture_output=True, timeout=60)
        (work / (phase + '.log')).write_bytes(result.stdout + result.stderr)
        output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
        if result.returncode != 0:
            raise RuntimeError(f'ART {phase} exited {result.returncode}:\n{output[-7000:]}')
        expected = 'NotificationIdentityDevice ' + phase + ' passed:'
        lines = [line.strip() for line in output.splitlines() if expected in line]
        if len(lines) != 1: raise RuntimeError('Missing unique success marker: ' + output[-3000:])
        print(lines[0])


if __name__ == '__main__':
    main()
