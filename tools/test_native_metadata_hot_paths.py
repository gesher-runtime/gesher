#!/usr/bin/env python3
"""Exercise production stdio, trampoline and memfd metadata fast paths.

The extracted bodies are unchanged. FILE tokens and the Win32 EA/handle layer
are explicit host boundaries; this does not run Windows UCRT or ARM code.
Existing stdio-lifetime and ARM64-pending-rollback fixtures cover their callers.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--thread-sanitize', action='store_true')
    args = parser.parse_args()
    stdio = (ROOT / 'compat/bionic/stdio_bionic.cc').read_text()
    arm64 = (ROOT / 'compat/arm64/dll/translator.cc').read_text()
    memfd = (ROOT / 'compat/posix/src/memfd_seals.cc').read_text()
    pieces = {
        '@STDIO@': section(stdio, 'constexpr uint32_t kWeof', 'class StreamLock'),
        '@TRAMPOLINES@': section(arm64, 'struct Trampoline {', '// --- va_list host functions'),
        '@MEMFD_REGISTRY@': section(memfd, 'std::mutex g_registry_mutex;', '}  // namespace'),
        '@MEMFD_FUNCTIONS@': section(memfd, 'void RegisterMemfd(int fd)', 'int GetMemfdSeals('),
        '@MEMFD_WRITE@': section(memfd, 'int MemfdWriteHandleError(', 'int MemfdResizeHandleError('),
    }
    template = (ROOT / 'tests/native/metadata_hot_paths_host.cc.in').read_text()
    for marker, value in pieces.items():
        assert template.count(marker) == 1, marker
        print(marker, hashlib.sha256(value.encode()).hexdigest(), flush=True)
        template = template.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix='gesher-metadata-hot-paths-') as directory:
        source, binary = Path(directory) / 'test.cc', Path(directory) / 'test'
        source.write_text(template)
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
            str(source), '-o', str(binary),
        ]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        if args.thread_sanitize:
            command += ['-fsanitize=thread', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == '__main__':
    main()
