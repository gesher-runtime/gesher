#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Run GmsCompatibilityUnitTest on a host JDK, as tests/windows/run-gms-unit-tests.ps1 does.

The Google client-library classes under app/java/com/google are test fixtures that
tests/apps/gmsapp bundles (app/build.sh keeps them out of gesher-app.jar). This host
test, over the android stubs in tests/test-apps/stubs (a device with no installed
package and no authenticator), checks that they report Play services' absence and
never answer a sign-in, token or push registration of their own (issue #335).
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOCATION = ('Priority', 'LocationRequest', 'LocationResult', 'LocationAvailability',
            'LocationCallback', 'FusedLocationProviderClient', 'LocationServices', 'SettingsClient',
            'LocationSettingsRequest', 'LocationSettingsResponse', 'LocationSettingsStates')
FIREBASE = ('FirebaseApp', 'FirebaseOptions', 'messaging/RemoteMessage', 'messaging/FirebaseMessaging')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path, default=os.environ.get('JAVA_HOME'))
    args = parser.parse_args()
    javac = str(args.java_home / 'bin/javac') if args.java_home else 'javac'
    java = str(args.java_home / 'bin/java') if args.java_home else 'java'
    gms = ROOT / 'app/java/com/google/android/gms'
    sources = [*sorted((ROOT / 'tests/test-apps/stubs').rglob('*.java'))]
    for package in ('tasks', 'common', 'auth'):
        sources += sorted((gms / package).rglob('*.java'))
    sources.append(gms / 'gcm/GoogleCloudMessaging.java')
    sources += [gms / 'location' / (name + '.java') for name in LOCATION]
    sources += [ROOT / 'app/java/com/google/firebase' / (name + '.java') for name in FIREBASE]
    sources.append(ROOT / 'tests/test-apps/GmsCompatibilityUnitTest.java')
    with tempfile.TemporaryDirectory(prefix='gesher-gms-fixtures-') as directory:
        work = Path(directory)
        listing = work / 'sources.txt'
        listing.write_text('\n'.join(str(p) for p in sources) + '\n', encoding='utf-8')
        subprocess.run([javac, '-nowarn', '--release', '11', '-encoding', 'UTF-8', '-d', str(work / 'classes'),
                        '@' + str(listing)], check=True)
        subprocess.run([java, '-cp', str(work / 'classes'), 'com.gesher.tests.GmsCompatibilityUnitTest'],
                       check=True, timeout=120)


if __name__ == '__main__':
    main()
