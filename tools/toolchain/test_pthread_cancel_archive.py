import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from prepare_pthread_cancel import NAME, extract_publisher_archive


class PublisherArchiveTest(unittest.TestCase):
    def setUp(self):
        work = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.archive = work / 'compiler.tar.xz'
        self.destination = work / 'output'
        self.destination.mkdir()

    def make_archive(self, *members):
        with tarfile.open(self.archive, 'w:xz') as archive:
            for member in members:
                archive.addfile(member)

    def test_extraction_preserves_relative_links_and_file_modes(self):
        payload = b'publisher compiler fixture'
        driver = tarfile.TarInfo(f'{NAME}/bin/compiler')
        driver.size, driver.mode, driver.mtime = len(payload), 0o755, 1000
        symlink = tarfile.TarInfo(f'{NAME}/other/driver')
        symlink.type, symlink.linkname = tarfile.SYMTYPE, '../bin/compiler'
        hardlink = tarfile.TarInfo(f'{NAME}/bin/driver')
        hardlink.type, hardlink.linkname = tarfile.LNKTYPE, driver.name
        hardlink.mode, hardlink.mtime = driver.mode, driver.mtime
        with tarfile.open(self.archive, 'w:xz') as archive:
            archive.addfile(driver, io.BytesIO(payload))
            archive.addfile(symlink)
            archive.addfile(hardlink)

        extract_publisher_archive(self.archive, self.destination)

        root = self.destination / NAME
        path = root / 'bin/compiler'
        self.assertEqual(path.read_bytes(), payload)
        self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        self.assertEqual(path.stat().st_mtime, 1000)
        self.assertEqual((root / 'other/driver').readlink(), Path('../bin/compiler'))
        self.assertTrue(path.samefile(root / 'bin/driver'))

    def test_invalid_archive_leaves_no_published_root(self):
        directory = tarfile.TarInfo(NAME)
        directory.type = tarfile.DIRTYPE
        sym_escape = tarfile.TarInfo(f'{NAME}/lib')
        sym_escape.type, sym_escape.linkname = tarfile.SYMTYPE, '../sibling'
        hard_escape = tarfile.TarInfo(f'{NAME}/binary')
        hard_escape.type, hard_escape.linkname = tarfile.LNKTYPE, 'sibling'
        cases = [(), (directory, tarfile.TarInfo('wrong-root')),
                 (directory, tarfile.TarInfo(f'{NAME}/../sibling')),
                 (directory, sym_escape), (directory, hard_escape)]
        for members in cases:
            with self.subTest(members=[member.name for member in members]):
                self.make_archive(*members)
                with self.assertRaises(ValueError):
                    extract_publisher_archive(self.archive, self.destination)
                self.assertEqual(list(self.destination.iterdir()), [])
