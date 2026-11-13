#!/usr/bin/env python3
"""Compile actual traversal code with real host filesystem calls, then run bounded contracts."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT / 'compat/posix/src/ftw.cc')
    p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--second-root', default='/dev/shm')
    a = p.parse_args()
    source = a.source.read_text()
    print('traversal source SHA256', hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-traversal-') as d:
        d = Path(d)
        (d / 'ftw.h').write_text((ROOT / 'compat/posix/include/ftw.h').read_text())
        (d / 'test.cc').write_text('''#define _GNU_SOURCE 1
// MinGW retains a 16-bit ino_t even though Gesher's stat has a 64-bit inode.
// Load the real headers first, then model that typedef width in the exact walker.
#include <ftw.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <utility>
#include <vector>
#ifdef TEST_NARROW_INO_T
using traversal_mingw_ino_t = unsigned short;
static int traversal_wide_stat(const char* path, struct stat* out) {
  int result = stat(path, out);
  if (result == 0) out->st_ino |= 1ULL << 48;
  return result;
}
static int traversal_wide_lstat(const char* path, struct stat* out) {
  int result = lstat(path, out);
  if (result == 0) out->st_ino |= 1ULL << 48;
  return result;
}
#define ino_t traversal_mingw_ino_t
#define stat(...) traversal_wide_stat(__VA_ARGS__)
#define lstat(...) traversal_wide_lstat(__VA_ARGS__)
#endif
#define nftw gesher_test_nftw
#define ftw gesher_test_ftw
''' + source + '''
#undef nftw
#undef ftw
#undef ino_t
#define TRAVERSAL_NFTW gesher_test_nftw
#define TRAVERSAL_FTW gesher_test_ftw
#include "traversal_contract_test.inc"
int main() { return traversal_run(); }
''')
        cmd = shlex.split(a.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-fno-exceptions', '-I', str(d), '-I', str(ROOT / 'tests/native'), str(d / 'test.cc'), '-o', str(d / 'test')]
        if a.sanitize: cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        env = os.environ.copy()
        env['GESHER_TRAVERSAL_SECOND_ROOT'] = str(Path(a.second_root).resolve())
        for extra in ([], ['-DTEST_NARROW_INO_T']):
            print('traversal inode model:', 'MinGW 16-bit typedef / 64-bit file IDs' if extra else 'host', flush=True)
            subprocess.run(cmd + extra, check=True, timeout=120)
            result = subprocess.run([str(d / 'test')], cwd=d, env=env, timeout=30).returncode
            if result: return result
        shim = (ROOT / 'compat/bionic/shim.cc').read_text()
        arm = (ROOT / 'compat/arm64/dll/guest_libc.cc').read_text()
        def section(text, start, end):
            if text.count(start) != 1 or text.count(end) != 1: raise ValueError('ambiguous source boundary')
            return text[text.index(start):text.index(end, text.index(start))]
        template = (ROOT / 'tests/native/traversal_adapters_host_test.cc.in').read_text()
        for name, content in (
            ('@X64_STAT_FIX@', section(shim, 'void FixStat(', 'int BIONIC_SYSV Stat(')),
            ('@X64_TRAVERSAL@', section(shim, 'using SysvFtwFn', 'void* BIONIC_SYSV Mmap(')),
            ('@ARM64_STAT_TRAVERSAL@', section(arm, 'struct X64Stat {', '// --- struct epoll_event'))):
            template = template.replace(name, content)
        for name, function in (('ftw','Ftw'),('ftw64','Ftw'),('nftw','Nftw'),('nftw64','Nftw')):
            if f'ENTRY("{name}", {function});' not in shim: raise ValueError('missing public export '+name)
        (d / 'adapters.cc').write_text(template)
        adapter_cmd = shlex.split(a.cxx) + ['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
                     '-Wno-unused-function',str(d / 'adapters.cc'),'-o',str(d / 'adapters')]
        if a.sanitize: adapter_cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(adapter_cmd, check=True, timeout=120)
        return subprocess.run([str(d / 'adapters')], cwd=d, timeout=30).returncode
if __name__ == '__main__': raise SystemExit(main())
