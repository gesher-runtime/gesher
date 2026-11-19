#!/usr/bin/env python3
"""Check production ARM libdl dispatch with explicit lookup/binding seams.

Windows elf_dlvsym_test runs real translated guest calls separately.
--source-ref is a behavioral negative control using earlier production code.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def section(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    return text[begin:text.index(end, begin)]

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    reference = 'working-tree snapshot'
    path = 'compat/arm64/dll/guest_libc.cc'
    if args.source_ref:
        reference = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options',
                                             args.source_ref + '^{commit}'], cwd=ROOT, text=True).strip()
        body = subprocess.check_output(['git', 'show', reference + ':' + path], cwd=ROOT, text=True)
    else:
        body = (ROOT / path).read_text()
    template = (ROOT / 'tests/native/arm64_dlvsym_dispatch_host.cc.in').read_text()
    fragments = {
        '@HOST_LOOKUP@': section(body, 'void* Host(const char* name)', 'int* HostErrno()'),
        '@GUEST_LOOKUP@': section(body, 'void* GuestSymbol(const char* name)', 'int64_t GuestRawSyscall('),
        '@OVERRIDE@': section(body, 'void Override(const char* name, void* host_wrapper)', '// A mismatched host'),
        '@DISPATCH@': section(body, 'void* g_dlsym = nullptr;', '// --- setjmp / longjmp'),
        '@REGISTRATION@': section(body, '  g_dlsym = Host("dlsym");', '  for (const Fallback& f : kFenv)'),
    }
    generated = template
    for marker, text in fragments.items():
        generated = generated.replace(marker, text)
    print('BOUNDARY production ARM dispatch/registration; host lookup and BindHost seams; no JIT or Windows execution', flush=True)
    print('SOURCE_REF ' + reference, flush=True)
    for name, value in [('complete_guest_libc', body), ('fixture', template), ('generated_unit', generated)]:
        print('SHA256 ' + name + ' ' + hashlib.sha256(value.encode()).hexdigest(), flush=True)
    for name in ['arm64_translator.h', 'symbol_table.h']:
        value = (ROOT / 'compat/arm64/include/gesher' / name).read_bytes()
        print('SHA256 ' + name + ' ' + hashlib.sha256(value).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-arm-dlvsym-') as temporary:
        root = Path(temporary)
        unit, binary = root / 'dispatch.cc', root / 'dispatch'
        unit.write_text(generated)
        command = shlex.split(args.cxx) + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fno-exceptions', '-I', str(ROOT / 'compat/arm64/include'), str(unit), '-o', str(binary)]
        env = os.environ.copy()
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
            if os.name != 'nt':
                command += ['-fno-pie', '-no-pie']
            previous = env.get('ASAN_OPTIONS', '')
            env['ASAN_OPTIONS'] = previous + (':' if previous else '') + 'detect_leaks=0'
            print('SANITIZERS address,undefined; fatal findings; detect_leaks=0 (ptrace boundary)', flush=True)
        print('COMPILE_COMMAND ' + json.dumps(command), flush=True)
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30, env=env)
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print('ARM dlvsym dispatch failed (exit ' + str(error.returncode) + ')', flush=True)
        raise SystemExit(error.returncode if error.returncode > 0 else 1)
