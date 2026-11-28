"""Exercise the production portable packager with isolated filesystem fixtures."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SHELL = os.environ.get('GESHER_POWERSHELL') or shutil.which('powershell') or shutil.which('pwsh')
VERSION = '0.9.1'
NAME = f'gesher-{VERSION}-portable.zip'


@unittest.skipUnless(SHELL, 'PowerShell is required to execute the production packager')
class PortablePackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='gesher-package-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / 'build [literal]'
        self.out = self.root / 'output [literal]'
        self.build.mkdir()
        names = (
            'dalvikvm.exe libgesher_account_crypto.so libgesher_install_pipe.so '
            'libgesher_webview_bridge.so core-oj.jar core-libart.jar core-icu4j.jar '
            'webview-auth-extension/manifest.json webview-auth-extension/worker.js '
            'icudt68l.dat hello.dex framework.jar framework-res.apk build.prop gesher-app.jar '
            'org.apache.http.legacy.jar com.android.location.provider.jar android.test.base.jar '
            'android.test.mock.jar android.test.runner.jar binderd.exe '
            'servicemanager.exe gesher-exec.exe gesher-launch.exe gesher_shellext.dll '
            'dex2oat.exe profman.exe oatdump.exe lifecycle.apk gesher-camera.apk gesher-tts.apk gesher-stt.apk libEGL.dll '
            'libGLESv2.dll gesher_arm64.dll ANGLE-NOTICES.txt DYNARMIC-NOTICES.txt '
            'FONTS-NOTICES.txt TZDATA-NOTICES.txt NET-NOTICES.txt '
            'framework/x86_64/boot.art fonts/Roboto-Regular.ttf fonts/fonts.xml '
            'tzdata/etc/tz/tzdata cacerts/01234567.0 media/audio/notifications/Test.ogg'
        ).split()
        for name in names:
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('fixture bytes for ' + name).encode())
        # This is deliberately outside the installer's production manifest.
        (self.build / 'unshipped-test-fixture.jar').write_bytes(b'never ship this')

    def package(self, *extra, success=True, version=VERSION):
        result = subprocess.run(
            [SHELL, '-NoProfile', '-File', str(ROOT / 'tools/package/package-portable.ps1'),
             '-BuildDir', str(self.build), '-OutDir', str(self.out), '-Version', version, *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_complete_archive_and_both_checksums(self):
        self.out.mkdir()
        installer_line = 'a' * 64 + '  gesher-setup-0.9.1.exe\n'
        (self.out / 'SHA256SUMS.txt').write_text(installer_line)
        self.package()
        archive = self.out / NAME
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        line = f'{digest}  {NAME}'
        self.assertEqual((self.out / (NAME + '.sha256')).read_text().strip(), line)
        self.assertEqual((self.out / 'SHA256SUMS.txt').read_text().splitlines(),
                         [installer_line.strip(), line])
        with zipfile.ZipFile(archive) as z:
            names = set(z.namelist())
            prefix = 'GesherPortable/'
            for name in ('Gesher.exe', 'portable.marker', 'Data/', 'README.txt', 'security-notice.txt',
                         'security-notice.he.txt', 'App/gesher-panel-native.ps1',
                         'App/licenses/ANGLE-NOTICES.txt', 'App/licenses/APKSIG-LICENSE.txt',
                         'App/etc/permissions/org.apache.http.legacy.xml',
                         'App/tzdata/etc/tz/tzdata', 'App/media/audio/notifications/Test.ogg',
                         'App/framework/x86_64/boot.art', 'App/gesher-tts.apk', 'App/gesher-stt.apk',
                         'App/webview-auth-extension/manifest.json', 'App/webview-auth-extension/worker.js'):
                self.assertIn(prefix + name, names)
            self.assertNotIn(prefix + 'App/unshipped-test-fixture.jar', names)
            self.assertEqual(z.read(prefix + 'Gesher.exe'), (self.build / 'gesher-launch.exe').read_bytes())
            self.assertEqual(z.read(prefix + 'App/gesher-launch.exe'), (self.build / 'gesher-launch.exe').read_bytes())
            self.assertIn(b'Version=0.9.1', z.read(prefix + 'App/gesher-version.ini'))
        self.assertFalse(any(p.name.startswith('.portable-') for p in self.out.iterdir()))

    def test_missing_required_runtime_never_reports_success(self):
        (self.build / 'libgesher_account_crypto.so').unlink()
        self.assertIn('Missing required runtime input', self.package(success=False))
        self.assertFalse((self.out / NAME).exists())

    def test_missing_license_preserves_existing_archive_and_checksum(self):
        self.package()
        before = {p.name: p.read_bytes() for p in self.out.iterdir()}
        (self.build / 'ANGLE-NOTICES.txt').unlink()
        self.package(success=False)
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, before)

    def test_missing_auth_worker_preserves_existing_archive(self):
        self.package()
        before = {p.name: p.read_bytes() for p in self.out.iterdir()}
        (self.build / 'webview-auth-extension/worker.js').unlink()
        self.assertIn('Missing required runtime input', self.package(success=False))
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, before)

    def test_existing_portable_data_is_never_removed(self):
        self.package('-SkipZip')
        sentinel = self.out / 'GesherPortable/Data/user-data.bin'
        sentinel.write_bytes(b'keep user data')
        self.assertIn('Portable output already exists', self.package('-SkipZip', success=False))
        self.assertEqual(sentinel.read_bytes(), b'keep user data')

    def test_version_must_be_canonical_and_bounded(self):
        for version in ('../outside', '0.09.1', '0.9.1.0', '65536.0.1'):
            with self.subTest(version=version):
                self.package(version=version, success=False)
        self.assertFalse((self.root / 'outside').exists())


if __name__ == '__main__':
    unittest.main()
