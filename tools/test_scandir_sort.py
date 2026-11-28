#!/usr/bin/env python3
"""Compile the complete production directory section with real filesystem controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Shared by the archived baseline and candidate, including 7e205878, 3a309b7
# and the qualified pre-Native2 endpoint. Only this exact legacy reader
# synthesizes ordinals. Current tests require the host's actual opaque cookie.
ORDINAL_READER_SHA256 = '5063dedb9c3bb0e8eaf6374b2917dc9893a0ca2004dd6bfaaa89864d62e31f5d'
COOKIE_MARKER = '@SCANDIR_REFERENCE_USES_NATIVE_COOKIE@'


def reference_cookie(section, explicit_source):
    boundary = "// scandir with the .so's System V filter and comparator; alphasort is the"
    assert section.count(boundary) == 1, 'directory reader/sort boundary changed'
    reader = section[:section.index(boundary)]
    if explicit_source and hashlib.sha256(reader.encode()).hexdigest() == ORDINAL_READER_SHA256:
        return 'false'
    start, end = 'bool Fill(ShimDir* d, BionicDirent* out) {', 'BionicDirent* BIONIC_SYSV Readdir('
    assert reader.count(start) == reader.count(end) == 1, 'directory reader boundaries changed'
    fill = reader[reader.index(start):reader.index(end)]
    assert fill.count('out->d_off = e->d_off;') == 1, 'current reader must return the native cookie'
    return 'true'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--baseline', action='store_true', help='Retain all correctness checks; omit only new work budgets')
    parser.add_argument('--build-benchmark', action='store_true')
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    if args.source and args.source_ref:
        parser.error('Choose one source input')
    if args.build_benchmark and (args.sanitize or args.negative_controls):
        parser.error('Ordinary benchmarks use neither instrumentation nor mutations')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    path = 'compat/bionic/posix_bionic.cc'
    raw = subprocess.check_output(['git', 'show', args.source_ref + ':' + path], cwd=ROOT) if args.source_ref else (args.source or ROOT / path).read_bytes()
    source = raw.decode()
    start, end = 'struct BionicDirent {', '// --- the *at() calls and other path calls'
    assert source.count(start) == source.count(end) == 1
    section = source[source.index(start):source.index(end)]
    native_cookie = reference_cookie(section, args.source_ref is not None or args.source is not None)
    if args.build_benchmark:
        template = (ROOT / 'tests/native/scandir_sort_bench.cc.in').read_text()
        assert template.count('@DIRECTORY_IMPLEMENTATION@') == template.count(COOKIE_MARKER) == 1
        generated = template.replace('@DIRECTORY_IMPLEMENTATION@', section).replace(COOKIE_MARKER, native_cookie)
        cpp = out / 'scandir_bench.cc'; cpp.write_text(generated)
        binary = out / 'scandir_bench'
        command = shlex.split(os.environ.get('CXX', 'c++')) + ['-std=gnu++20', '-O3', '-DNDEBUG', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-fno-exceptions', '-fno-builtin', '-mno-ms-bitfields', '-fsized-deallocation', '-pthread', '-I', str(ROOT / 'compat/bionic'),
            '-I', str(ROOT / 'compat/arm64/include'), str(cpp), '-o', str(binary)]
        subprocess.run(command, check=True, timeout=120)
        receipt = {'source_ref': args.source_ref, 'production_sha256': hashlib.sha256(raw).hexdigest(),
                   'section_sha256': hashlib.sha256(section.encode()).hexdigest(),
                   'template_sha256': hashlib.sha256(template.encode()).hexdigest(),
                   'generated_sha256': hashlib.sha256(generated.encode()).hexdigest(),
                   'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'command': command,
                   'compiler': subprocess.check_output(command[:1] + ['--version'], text=True),
                   'ordinary_allocators': True, 'entry': 'volatile actual Scandir function pointer',
                   'scope': 'Complete scan/copy/sort/full-result verification/free; filesystem setup outside measured interval.'}
        (out / 'source.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps({'binary': str(binary), 'source': receipt['production_sha256']}))
        return
    original = (ROOT / 'tests/native/directory_contract_host_test.cc.in').read_text()
    assert original.count('owned[512]') == 1
    host = original.replace('owned[512]', 'owned[16384]')
    names = ('scandir', 'scandir64', 'scandirat', 'scandirat64', 'opendir', 'readdir', 'closedir', 'rewinddir', 'telldir', 'seekdir')
    registrations = []
    for name in names:
        found = re.findall(r'^  E\("' + name + r'", [A-Za-z_][A-Za-z0-9_]*\);$', source, re.M)
        assert len(found) == 1, name
        registrations.extend(found)
    for marker, value in (('@DIRECTORY_IMPLEMENTATION@', section), ('@DIRECTORY_REGISTRATIONS@', '\n'.join(registrations))):
        assert host.count(marker) == 1
        host = host.replace(marker, value)
    template = (ROOT / 'tests/native/scandir_sort_host.cc.in').read_text()
    assert template.count('@HOST_TEMPLATE@') == template.count(COOKIE_MARKER) == 1
    generated = template.replace('@HOST_TEMPLATE@', host).replace(COOKIE_MARKER, native_cookie)
    cpp = out / 'scandir_sort.cc'
    cpp.write_text(generated)
    binary = out / 'scandir_sort'
    command = shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-fno-exceptions',
        '-fno-builtin', '-pthread', '-I', str(ROOT / 'compat/bionic'), '-I', str(ROOT / 'compat/arm64/include'),
        '-I', str(ROOT / 'tests/native'), str(cpp),
        '-Wl,--wrap=malloc,--wrap=realloc,--wrap=free,--wrap=readdir,--wrap=closedir,--wrap=fdopendir', '-o', str(binary)]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True, timeout=120)
    env = os.environ.copy()
    env['ASAN_OPTIONS'] = 'allocator_may_return_null=1:' + env.get('ASAN_OPTIONS', '')
    result = subprocess.run([str(binary), '--baseline' if args.baseline else '--candidate'],
                            cwd=out, env=env, capture_output=True, text=True, timeout=120)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    receipt = {'source_ref': args.source_ref, 'production_sha256': hashlib.sha256(raw).hexdigest(),
               'section_sha256': hashlib.sha256(section.encode()).hexdigest(),
               'original_host_template_sha256': hashlib.sha256(original.encode()).hexdigest(),
               'host_boundary_change': 'Increase the bounded allocation pointer tracker from512 to16384; observe allocation sizes through delegating wrappers. All original assertions remain.',
               'public_contract_sha256': hashlib.sha256((ROOT / 'tests/native/scandir_sort_contract_test.inc').read_bytes()).hexdigest(),
               'template_sha256': hashlib.sha256(template.encode()).hexdigest(),
               'generated_sha256': hashlib.sha256(generated.encode()).hexdigest(), 'command': command,
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'baseline': args.baseline,
               'returncode': result.returncode, 'stdout_sha256': hashlib.sha256(result.stdout.encode()).hexdigest(),
               'limits': ['Complete production directory section and exact public registrations with real host directories.',
                          'Allocation/reader/closedir faults stay at declared host boundaries.',
                          'Windows enumeration and guest-ABI execution require the actual native target gates.']}
    (out / 'source.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(result.stdout, end='')
    print(result.stderr, end='')
    if result.returncode:
        raise SystemExit(result.returncode)
    if args.negative_controls:
        controls = [
            ('unstable-tie', 'to[out] = compare(&x, &y) <= 0', 'to[out] = compare(&x, &y) < 0',
             'every entry and stable tie matches independent enumeration'),
            ('missing-copyback', 'if (from != entries) std::memcpy(entries, from, count * sizeof(*entries));', '(void)from;',
             'every entry and stable tie matches independent enumeration'),
            ('retained-scratch', 'std::free(scratch);', '(void)scratch;', 'sorting retains no extra allocation bytes'),
            ('oversized-scratch', 'std::malloc(count * sizeof(*entries))',
             'std::malloc(count * (sizeof(*entries) + 1))', 'exact optional pointer scratch live bytes'),
            ('repeated-oom-attempt', 'remaining_swaps = SIZE_MAX;',
             'remaining_swaps = count * 8;', 'one optional scratch attempt'),
            ('allocation-errno', 'errno = saved_errno;\n  if (scratch == nullptr)', '(void)saved_errno;\n  if (scratch == nullptr)',
             'comparator errno chain preserved'),
            ('false-oom-success', 'if (scratch == nullptr) return false;', 'if (scratch == nullptr) return true;',
             'every entry and stable tie matches independent enumeration'),
            ('unbounded-old-sort', 'size_t remaining_swaps = count * 8;', 'size_t remaining_swaps = SIZE_MAX;',
             'one optional scratch attempt'),
            ('lost-small-path', 'compare != nullptr && count >= 128', 'compare != nullptr && count >= 2',
             'tiny and nearly sorted use no scratch'),
        ]
        receipts = []
        for label, old, new, assertion in controls:
            assert source.count(old) == 1, label
            folder = out / 'negative-controls' / label; folder.mkdir(parents=True, exist_ok=True)
            mutated = folder / 'posix_bionic.cc'; mutated.write_text(source.replace(old, new, 1))
            command = [sys.executable, str(Path(__file__).resolve()), '--source', str(mutated), '--output-dir', str(folder)]
            if args.sanitize: command.append('--sanitize')
            run = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=150)
            log = run.stdout + run.stderr; (folder / 'driver.log').write_text(log)
            assert run.returncode != 0 and 'FAIL SCANDIR_SORT ' + assertion in log, (label, run.returncode, log[-4000:])
            receipts.append({'name': label, 'assertion': assertion, 'returncode': run.returncode,
                             'source_sha256': hashlib.sha256(mutated.read_bytes()).hexdigest(),
                             'log_sha256': hashlib.sha256(log.encode()).hexdigest()})
            print('SCANDIR_SORT_NEGATIVE_PASS ' + label, flush=True)
        (out / 'negative-controls.json').write_text(json.dumps(receipts, indent=2) + '\n')


if __name__ == '__main__':
    main()
