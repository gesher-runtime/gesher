#!/usr/bin/env python3
"""Run real APK/resource/device-admin service contracts on isolated Windows ART.

The test injects only the host lock/availability and attached-caller boundaries;
it never locks the user's workstation or modifies installed application data.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('aapt2', 'android-jar', 'java-home', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--tests-dex', type=Path)
    parser.add_argument('--fixtures-only', action='store_true')
    parser.add_argument('--app-jar', type=Path)
    args = parser.parse_args()
    if not args.fixtures_only and (args.runtime is None or args.tests_dex is None):
        parser.error('--runtime and --tests-dex are required for ART execution')
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    jdk = args.java_home / 'bin'
    suffix = '.exe' if os.name == 'nt' else ''
    def run(command):
        subprocess.run([str(x) for x in command], check=True, timeout=60)
    key = work / 'test.jks'
    if not key.exists():
        run([jdk / ('keytool' + suffix), '-genkeypair', '-keystore', key, '-storepass', 'android',
             '-keypass', 'android', '-alias', 'test', '-dname', 'CN=Isolated QA',
             '-keyalg', 'RSA', '-validity', '3650'])
    fixtures = []
    for name, policies, permission in (
            ('valid', '<force-lock/>', 'android.permission.BIND_DEVICE_ADMIN'),
            ('unsupported', '<force-lock/><wipe-data/>', 'android.permission.BIND_DEVICE_ADMIN'),
            ('unprotected', '<force-lock/>', ''),
            ('updated', '<force-lock/><wipe-data/>', 'android.permission.BIND_DEVICE_ADMIN')):
        folder = work / name
        (folder / 'res/xml').mkdir(parents=True, exist_ok=True)
        (folder / 'res/xml/admin.xml').write_text(
            '<device-admin><uses-policies>' + policies + '</uses-policies></device-admin>', encoding='utf-8')
        pkg = 'com.gesher.fixture.admin.' + ('valid' if name == 'updated' else name)
        (folder / 'AndroidManifest.xml').write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{pkg}">
<uses-sdk android:minSdkVersion="26" android:targetSdkVersion="34"/>
<application android:hasCode="false" android:label="QA admin">
<receiver android:name=".Admin" android:exported="true" android:permission="{permission}">
<meta-data android:name="android.app.device_admin" android:resource="@xml/admin"/>
<intent-filter><action android:name="android.app.action.DEVICE_ADMIN_ENABLED"/></intent-filter>
</receiver></application></manifest>''', encoding='utf-8')
        run([args.aapt2, 'compile', '--dir', folder / 'res', '-o', folder / 'resources.zip'])
        apk = folder / 'fixture.apk'
        run([args.aapt2, 'link', '-I', args.android_jar, '--manifest', folder / 'AndroidManifest.xml',
             '-o', apk, folder / 'resources.zip'])
        run([jdk / ('java' + suffix), '-jar', args.aapt2.parent / 'lib/apksigner.jar', 'sign',
             '--ks', key, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', apk])
        fixtures.append(apk)
        shutil.copy2(apk, work / ('deviceadmin-' + name + '.apk'))
    if args.fixtures_only:
        return
    runtime = args.runtime.resolve()
    profile = work / 'profile'
    for part in ('system/framework', 'data', 'art', 'i18n/etc/icu'):
        (profile / part).mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime / 'framework-res.apk', profile / 'system/framework/framework-res.apk')
    shutil.copy2(runtime / 'build.prop', profile / 'system/build.prop')
    icu = next(runtime.glob('icudt*l.dat'))
    shutil.copy2(icu, profile / 'i18n/etc/icu' / icu.name)
    def guest(path):
        path = path.resolve()
        text = path.as_posix()
        return text[2:] if path.drive.lower() == runtime.drive.lower() else '/mnt/' + text[0].lower() + text[2:]
    env = {k: v for k, v in os.environ.items() if not k.startswith(('ANDROID_', 'GESHER_'))}
    env.update(ANDROID_ROOT=guest(profile / 'system'), ANDROID_DATA=guest(profile / 'data'),
               ANDROID_ART_ROOT=guest(profile / 'art'), ANDROID_I18N_ROOT=guest(profile / 'i18n'),
               ANDROID_TZDATA_ROOT=guest(runtime / 'tzdata'), GESHER_UID='1000', GESHER_BINDER_UID='1000')
    bcp = ':'.join(guest(runtime / (name + '.jar')) for name in
                   ('core-oj', 'core-libart', 'core-icu4j', 'framework'))
    bcp += ':' + guest(args.app_jar or runtime / 'gesher-app.jar') + ':' + guest(args.tests_dex)
    command = [str(runtime / 'dalvikvm.exe'), '-Xbootclasspath:' + bcp,
               '-Ximage:' + guest(runtime / 'framework/boot.art'), '-Xint', '-Xusejit:false',
               'com.gesher.server.DeviceAdminPolicyRuntimeTest', *map(guest, fixtures)]
    result = subprocess.run(command, cwd=runtime, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=90)
    (work / 'runtime.log').write_bytes(result.stdout)
    print(result.stdout.decode('utf-8', errors='replace'))
    if result.returncode or b'DeviceAdminPolicyRuntimeTest: 30 checks passed' not in result.stdout:
        raise SystemExit(result.returncode or 1)


if __name__ == '__main__':
    main()
