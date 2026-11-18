#!/usr/bin/env python3
"""Build one ART fixture with Gesher and the SHA-pinned, unmodified AOSP provider.

This builds a jar only. Run gesher.services.UserDictionaryContractTest with the
candidate or reference provider class name on an owned Android reference device.
"""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REFERENCE_URL = ('https://android.googlesource.com/platform/packages/providers/'
                 'UserDictionaryProvider/+/refs/tags/android-16.0.0_r4/'
                 'src/com/android/providers/userdictionary/UserDictionaryProvider.java?format=TEXT')
REFERENCE_SHA256 = '76184729ffb6e230fb909c2d0118cf2301654d632a121840592e0f3986524bf2'


def copy_classes(archive, source):
    if source.is_dir():
        for path in sorted(source.rglob('*.class')):
            archive.write(path, path.relative_to(source).as_posix())
    else:
        with zipfile.ZipFile(source) as incoming:
            for name in incoming.namelist():
                if name.endswith('.class'):
                    archive.writestr(name, incoming.read(name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('java-home', 'framework-classes', 'core-classes', 'app-classes', 'r8-jar', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--reference-source', type=Path,
                        help='Previously downloaded AOSP source, still checked against the pinned SHA')
    args = parser.parse_args()
    if args.reference_source:
        reference = args.reference_source.read_bytes()
    else:
        with urllib.request.urlopen(REFERENCE_URL, timeout=60) as response:
            reference = base64.b64decode(response.read().strip(), validate=True)
    digest = hashlib.sha256(reference).hexdigest()
    if digest != REFERENCE_SHA256:
        parser.error('AOSP reference SHA-256 mismatch: ' + digest)
    classpath = os.pathsep.join(str(p.resolve()) for p in
                               (args.app_classes, args.framework_classes, args.core_classes))
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='dictionary-reference-') as temporary:
        work = Path(temporary)
        source = work / 'UserDictionaryProvider.java'
        source.write_bytes(reference)
        classes = work / 'classes'
        classes.mkdir()
        subprocess.run([str(args.java_home / 'bin/javac'), '-source', '17', '-target', '17',
                        '-encoding', 'UTF-8', '-XDstringConcat=inline', '-cp', classpath,
                        '-d', str(classes), str(source),
                        str(ROOT / 'tests/dex-services/gesher/services/UserDictionaryContractTest.java')],
                       check=True)
        jar = work / 'classes.jar'
        with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as archive:
            copy_classes(archive, args.app_classes)
            copy_classes(archive, classes)
        subprocess.run([str(args.java_home / 'bin/java'), '-cp', str(args.r8_jar),
                        'com.android.tools.r8.D8', '--release', '--min-api', '36',
                        '--output', str(output), str(jar)], check=True)
    print('Reference source SHA-256:', digest)
    print('ART fixture:', output)


if __name__ == '__main__':
    main()
