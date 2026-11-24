#!/usr/bin/env python3
"""Actual storage/text tool bodies on host files; injected errors are isolated fixtures.

This test does not execute Windows, access block devices, or claim physical-media
coverage. Windows shell and broker fixtures exercise those production boundaries.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'compat/elf/exec/gesher_tools.cc'


def main():
    source = SOURCE.read_text()
    body = source[source.index('struct DdFile {'):source.index('int Blockdev(')]
    prefix = r'''
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
int fault_error;
bool fault_used;
bool short_writes;
ssize_t FixtureRead(int fd, void* data, size_t size) {
  if (fault_error && !fault_used && lseek(fd, 0, SEEK_CUR) == 4) {
    fault_used = true; errno = fault_error; return -1;
  }
  return ::read(fd, data, size);
}
ssize_t FixtureWrite(int fd, const void* data, size_t size) {
  return ::write(fd, data, short_writes ? std::min(size, size_t{2}) : size);
}
#define read FixtureRead
#define write FixtureWrite
'''
    suffix = r'''
#undef read
#undef write
int main(int argc, char** argv) {
  if (argc < 2) return 99;
  if (const char* error = getenv("GESHER_TEST_READ_ERROR")) fault_error = atoi(error);
  short_writes = getenv("GESHER_TEST_SHORT_WRITE") != nullptr;
  std::vector<std::string> args(argv + 2, argv + argc);
  std::string command(argv[1]);
  if (command == "dd") return Dd(args);
  if (command == "grep") return Grep(args);
  if (command == "awk") return Awk(args);
  if (command == "sync") return Sync(args);
  if (command == "umount") return Umount(args);
  return 98;
}
'''
    with tempfile.TemporaryDirectory(prefix='gesher-storage-tools-') as temporary:
        folder = Path(temporary)
        fixture = folder / 'tools.cc'
        fixture.write_text(prefix + body + suffix)
        executable = folder / 'tools'
        subprocess.run(['c++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        str(fixture), '-o', str(executable)], check=True)
        (folder / 'input').write_bytes(b'AAAABBBBCCCC')
        checks = 0

        def run(args, expected, code=0, data=None, env=None):
            nonlocal checks
            environment = os.environ.copy()
            environment.update(env or {})
            result = subprocess.run([str(executable), *args], cwd=folder, input=data,
                                    capture_output=True, env=environment, timeout=10)
            assert (result.returncode, result.stdout) == (code, expected), (args, result)
            checks += 1
            return result

        run(['dd', 'if=input', 'bs=4', 'skip=1', 'count=1'], b'BBBB')
        run(['dd', 'bs=4', 'conv=sync', 'status=none'], b'AB\0\0', data=b'AB')
        run(['dd', 'bs=4', 'count=0'], b'', data=b'ABCD')
        run(['dd', 'if=input', 'bs=4', 'conv=noerror,sync'], b'AAAA\0\0\0\0CCCC', 1,
            env={'GESHER_TEST_READ_ERROR': '5'})
        run(['dd', 'if=input', 'bs=4', 'conv=noerror'], b'AAAACCCC', 1,
            env={'GESHER_TEST_READ_ERROR': '5'})
        for error in (1, 9, 13, 19, 30, 110):
            run(['dd', 'if=input', 'bs=4', 'conv=noerror,sync'], b'AAAA', 1,
                env={'GESHER_TEST_READ_ERROR': str(error)})
        run(['dd', 'if=input', 'bs=4'], b'AAAABBBBCCCC',
            env={'GESHER_TEST_SHORT_WRITE': '1'})
        run(['dd', 'of=update', 'bs=4', 'seek=1', 'conv=notrunc'], b'', data=b'NEW!')
        assert (folder / 'update').read_bytes() == b'\0\0\0\0NEW!'
        run(['dd', 'of=overflow', 'bs=512', 'seek=18446744073709551615'], b'', 1)
        assert not (folder / 'overflow').exists()
        result = run(['dd', 'status=progress'], b'1234', data=b'1234')
        assert b'4 bytes copied' in result.stderr
        run(['dd', 'conv=notrunc,unknown'], b'', 1)
        run(['grep', '^/dev/block/sdb'], b'/dev/block/sdb1 /mnt/card vfat\n',
            data=b'/dev/block/sdb1 /mnt/card vfat\nroot / ext4\n')
        run(['grep', 'missing'], b'', 1, data=b'present\n')
        run(['grep', '['], b'', 2, data=b'present\n')
        run(['grep', '-Fn', '['], b'1:[value]\n', data=b'[value]\nplain\n')
        run(['grep', '-v', 'missing'], b'present\n', data=b'present\n')
        run(['awk', '{print $2}'], b'/mnt/card\n', data=b'/dev/block/sdb1 /mnt/card vfat\n')
        run(['awk', '{print $NF}'], b'three\n', data=b'  one two  three \n')
        run(['awk', '{print $0}'], b' original  line \n', data=b' original  line \n')
        run(['awk', '{print $2}'], b'\n', data=b'one\n')
        run(['awk', 'BEGIN {exit 0}'], b'', 2)
        run(['sync', 'input'], b'')  # real fsync on an isolated ordinary host file
        run(['umount', 'input'], b'', 1)  # never claims the host file was mounted
        print(f'Storage tool contracts passed: {checks} cases; original production tool bodies; '
              'ordinary host files and explicit read/write error adapters; no physical media')


if __name__ == '__main__':
    main()
