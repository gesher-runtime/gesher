#!/usr/bin/env python3
"""Execute production notification storage migration with explicit host Parcel serialization.

No Android Parcel wire-format, real PendingIntent activation, Windows toast or ART claims.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('java-home', 'framework-classes', 'core-classes', 'app-classes'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--source-ref')
    p.add_argument('--tag-only', action='store_true', help='Run the public-internal tag API probe, also against unchanged source')
    p.add_argument('--cc', default='cc')
    a = p.parse_args()
    cp = os.pathsep.join(str(x.resolve()) for x in (a.app_classes, a.framework_classes, a.core_classes))
    javac, java = (str(a.java_home.resolve() / 'bin' / x) for x in ('javac', 'java'))
    with tempfile.TemporaryDirectory(prefix='gesher-notification-identity-') as temp:
        work = Path(temp)
        classes = work / 'classes'; classes.mkdir()
        windows = os.name == 'nt'
        library = work / ('identity_host.dll' if windows else 'libidentity_host.so')
        subprocess.run([a.cc, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-shared',
            *([] if windows else ['-fPIC']), '-I' + str(a.java_home / 'include'),
            '-I' + str(a.java_home / 'include' / ('win32' if windows else 'linux')),
            str(ROOT / 'tests/notification-identity/framework_host.c'), '-o', str(library),
            *(['-lwinpthread', '-static'] if windows else [])], check=True)
        sources = []
        for name in ('NotificationIdentity', 'NotificationDelegates', 'NotificationManagerService', 'NotificationStore', 'NotificationIcons', 'WindowsHost'):
            path = 'app/java/com/gesher/server/' + name + '.java'
            source = ROOT / path
            if a.source_ref and name not in ('NotificationIdentity', 'NotificationDelegates'):
                source = work / (name + '.java')
                source.write_bytes(subprocess.check_output(['git', 'show', a.source_ref + ':' + path], cwd=ROOT))
            sources.append(str(source))
        # Production compilation uses actual application/framework interfaces.
        if a.tag_only:
            subprocess.run([javac, '-encoding', 'UTF-8', '-cp', cp, '-d', str(classes), *sources,
                str(ROOT / 'tests/notification-identity/ToastTagProbe.java')], check=True)
            subprocess.run([java, '-cp', str(classes) + os.pathsep + cp,
                'com.gesher.server.ToastTagProbe'], check=True, timeout=30)
            return
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', cp, '-d', str(classes), *sources,
            str(ROOT / 'tests/dex-services/com/gesher/server/ToastIdentityDevice.java'),
            str(ROOT / 'tests/dex-services/com/gesher/server/NotificationStoreFormatDevice.java'),
            str(ROOT / 'tests/dex-services/gesher/services/NotificationChannelGroupsTest.java')], check=True)
        subprocess.run([javac, '-encoding', 'UTF-8', '-cp', str(classes) + os.pathsep + cp,
            '-d', str(classes), *map(str, (ROOT / 'tests/notification-identity').glob('*.java'))], check=True)
        environment = os.environ.copy()
        environment['ANDROID_DATA'] = str(work / 'data')
        environment['GESHER_MICROG_SPOOF'] = '0'
        for phase in ('write', 'read'):
            subprocess.run([java, '-XX:-UsePerfData', '-XX:+UnlockDiagnosticVMOptions',
                '-XX:-BytecodeVerificationRemote', '-cp', str(classes) + os.pathsep + cp,
                'com.gesher.server.NotificationIdentityHost', phase, str(library)], env=environment, check=True, timeout=30)


if __name__ == '__main__':
    main()
