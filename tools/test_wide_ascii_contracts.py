#!/usr/bin/env python3
"""Actual UTF conversion bodies, pinned Android reference and real guard pages.

Host execution establishes component behavior only. Whole Windows translation
unit compilation and actual platform CI are separate qualification boundaries.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'compat/bionic/locale_bionic.cc'


def production(source):
    start = source.index('struct BionicMbstate {')
    end = source.index('// --- wide characters', start)
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--source-file', type=Path)
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--out-dir', type=Path)
    args = parser.parse_args()
    if args.source_ref and args.source_file:
        parser.error('choose source-ref or source-file')
    source = (args.source_file.read_text() if args.source_file else
              subprocess.check_output(['git', 'show', args.source_ref + ':' + SOURCE], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / SOURCE).read_text())
    with tempfile.TemporaryDirectory(prefix='gesher-wide-ascii-') as temporary:
        work = args.out_dir.resolve() if args.out_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        commands = []

        def run(command, label, timeout=120):
            environment = dict(os.environ)
            if args.sanitize:
                environment['ASAN_OPTIONS'] = 'detect_leaks=0:abort_on_error=1'
                environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
            result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
            (work / (label + '.stdout')).write_text(result.stdout)
            (work / (label + '.stderr')).write_text(result.stderr)
            commands.append({'command': list(map(str, command)), 'exit': result.returncode,
                             'stdout': label + '.stdout', 'stderr': label + '.stderr'})
            (work / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
            if result.returncode:
                raise RuntimeError(f'{label} failed: {result.stdout[-2000:]}\n{result.stderr[-4000:]}')
            return result

        run(['python3', str(ROOT/'probes/native3/wide/prepare.py'), str(work/'reference.cpp')], 'reference-prepare')
        exact = production(source)
        template = (ROOT/'tests/native/wide_ascii_contract_host.cc.in').read_text()
        assert template.count('@PRODUCTION@') == 1
        (work/'host.cc').write_text(template.replace('@PRODUCTION@', exact))
        (work/'production.cc').write_text(source)
        version = run([args.compiler, '--version'], 'compiler-version').stdout
        flags = ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-Wno-unused-function']
        # GCC warns about unchanged pinned StrToI attributes and C memset of
        # the adapter's byte-only state constructor. Keep those warnings
        # visible without rewriting supplier bodies or the existing adapter.
        if 'Free Software Foundation' in version:
            flags += ['-Wno-error=attributes', '-Wno-error=class-memaccess']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        run([args.compiler, *flags, '-I', str(ROOT/'compat/bionic'), '-I', str(ROOT/'tests/native2'),
             str(work/'host.cc'), '-o', str(work/'host')], 'compile')
        result = run([str(work/'host')], 'run')
        assert 'WIDE_ASCII_CONTRACT_PASS ' in result.stdout
        proof = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                 'actual_utf8_block_sha256': hashlib.sha256(exact.encode()).hexdigest(),
                 'source_ref': args.source_ref, 'sanitizers': args.sanitize,
                 'binary_sha256': hashlib.sha256((work/'host').read_bytes()).hexdigest(),
                 'commands': commands, 'result': result.stdout,
                 'boundary': 'Actual complete UTF conversion bodies and pinned source on Linux; no Windows/ART execution claim.'}
        (work/'receipt.json').write_text(json.dumps(proof, indent=2) + '\n')
        print(result.stdout, end='')


if __name__ == '__main__':
    main()
