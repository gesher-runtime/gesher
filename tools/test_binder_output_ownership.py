#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
"""Exercise the actual broker core, read/write dispatch, and reply framing.

Linux uses real local byte-stream sockets and self-process descriptor duplication
at an explicit Windows I/O boundary. Windows uses actual byte-mode named pipes
and handle duplication. Neither path replaces Binder parsing/ownership/results.
The AppContainer hello/admission path is outside this component; its one global
flag is annotated unused in the extracted core, never activated or simulated.
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


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise RuntimeError('Production extraction boundary is not unique: ' + begin)
    return source[source.index(begin):source.index(end)]


def generated(source: str) -> str:
    core = section(source, 'namespace {\n\nnamespace wire =', '// --- connections ')
    # Only ServeProc's excluded AppContainer handshake uses this new main flag.
    # Keep its actual declaration/value while retaining -Werror for this fixture.
    sandbox_flag = 'bool g_sandbox = false;'
    if core.count(sandbox_flag) > 1:
        raise RuntimeError('AppContainer admission flag boundary is not unique')
    core = core.replace(sandbox_flag, '[[maybe_unused]] ' + sandbox_flag)
    io = section(source, 'bool ReadExact(HANDLE pipe,', 'uint64_t RandomKey() {')
    dispatch = section(source, 'int DoIoctl(Proc* proc,', 'void Serve(HANDLE pipe) {')
    start = dispatch.index('if (op == wire::kWriteRead) {') + len('if (op == wire::kWriteRead) {')
    end = dispatch.index('} else if (op == wire::kClaimBuffer', start)
    body = dispatch[start:end]
    finish_start = dispatch.index('} else if (op == wire::kClaimBuffer || op == wire::kFinishBuffer) {')
    finish_start += len('} else if (op == wire::kClaimBuffer || op == wire::kFinishBuffer) {')
    finish_end = dispatch.index('} else if (op == wire::kIoctl) {', finish_start)
    finish_body = dispatch[finish_start:finish_end]
    wrapper = '''
bool DispatchWriteRead(Proc* proc, Thread* thread, In req, WireReply& reply) {
  std::unique_lock<std::mutex> lock(g_lock);
  do {
@BODY@
    return true;
  } while (false);
  return false;
}
bool DispatchBufferFinish(Thread* thread, uint32_t op, In req, WireReply& reply) {
  std::unique_lock<std::mutex> lock(g_lock);
  do {
@FINISH_BODY@
    return true;
  } while (false);
  return false;
}
}  // the unchanged production anonymous namespace
'''.replace('@BODY@', body).replace('@FINISH_BODY@', finish_body)
    production = core + io + dispatch + wrapper
    anchor = '    bytes.insert(bytes.end(), b, b + n);'
    if production.count(anchor) != 1:
        raise RuntimeError('Production output accounting anchor changed')
    production = production.replace(anchor, '    accounting::Copy(n);\n' + anchor)
    return (ROOT / 'tests/native/binder_output_ownership_test.cc.in').read_text().replace(
        '@PRODUCTION@', production)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--aosp-dir', type=Path)
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--build-only', action='store_true', help='Build a reusable host executable without running it')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--output', type=Path, help='Executable destination, separate from generated source receipts')
    parser.add_argument('--negative-controls', action='store_true')
    parser.add_argument('--measure', action='store_true')
    parser.add_argument('--measure-only', action='store_true', help='Keep all correctness checks; omit only direct-output copy budget')
    parser.add_argument('--timing', action='store_true', help='Disable allocation/copy instrumentation for CPU measurements')
    parser.add_argument('--case', default='all')
    parser.add_argument('--samples', type=int, default=3)
    args = parser.parse_args()
    if args.compile_only and (not args.output_dir or not args.aosp_dir):
        parser.error('--compile-only requires --output-dir and --aosp-dir')
    if args.negative_controls and (args.compile_only or args.build_only or args.timing or args.source_ref):
        parser.error('--negative-controls requires executing the current counted source')
    relative = 'compat/binder/binderd.cc'
    source = (subprocess.check_output(['git', 'show', args.source_ref + ':' + relative], cwd=ROOT).decode()
              if args.source_ref else (ROOT / relative).read_text())
    with tempfile.TemporaryDirectory(prefix='gesher-binder-output-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        cpp = work / 'broker-output.cc'
        cpp.write_text(generated(source))
        windows = os.name == 'nt' or args.compile_only
        executable = args.output.resolve() if args.output else work / ('broker-output.exe' if windows else 'broker-output')
        executable.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + ['-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
            '-fno-exceptions', '-pthread', '-I' + str(ROOT / 'tests/native'),
            '-I' + str(ROOT / 'compat/posix/src')]
        if windows:
            command += ['-static', '-I' + str(ROOT / 'compat/posix/include'),
                        '-idirafter', str(args.aosp_dir / 'bionic/libc/kernel/uapi')]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        if args.timing:
            command += ['-DBINDER_OUTPUT_COUNTING=0']
        command += [str(cpp), '-o', str(executable)]
        print('binderd_sha256=' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
        subprocess.run(command, check=True, timeout=120)
        receipt = {'source_ref': args.source_ref, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                   'command': command, 'generated_sha256': hashlib.sha256(cpp.read_bytes()).hexdigest()}
        (work / 'source.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if not args.compile_only and not args.build_only:
            subprocess.run([str(executable), 'measure' if args.measure else ('reference' if args.measure_only else 'gate'),
                            args.case, str(args.samples)],
                           check=True, timeout=120)
        if args.negative_controls:
            controls = (
                ('spawn-header', 'command_start(output.bytes.size())', 'command_start(0)',
                 'spawn replaces the first command without overwriting status or consumed'),
                ('read-error-status', 'memcpy(reply.bytes.data() + sizeof(uint32_t), &ret, sizeof(int32_t));', '(void)ret;',
                 'actual read failure updates status after the header was reserved'),
                ('consumed-header', 'reply.Put<uint32_t>(consumed);', 'reply.Put<uint32_t>(0);',
                 'one actual command is consumed exactly once'),
                ('read-budget', 'rs.limit = static_cast<size_t>(read_size);',
                 'rs.limit = read_size > 12 ? static_cast<size_t>(read_size - 12) : 0;',
                 'wire status header consumes no Android read-buffer budget'),
                ('zero-input', 'if (n == 0) return true;', 'if (n == 0) return false;',
                 'zero-byte empty view neither dereferences nor advances null'),
                ('extra-output-copy', 'memcpy(reply.bytes.data() + sizeof(uint32_t), &ret, sizeof(int32_t));',
                 'Out copied; copied.Write(reply.bytes.data(), reply.bytes.size()); reply.bytes.swap(copied.bytes);\n'
                 '      memcpy(reply.bytes.data() + sizeof(uint32_t), &ret, sizeof(int32_t));',
                 'one serialized body pass; no complete nested-output recopy'),
            )
            evidence = []
            for name, before, after, assertion in controls:
                if source.count(before) != 1:
                    raise RuntimeError('Negative control anchor is not unique: ' + name)
                mutant = source.replace(before, after, 1)
                folder = work / 'negative' / name
                folder.mkdir(parents=True, exist_ok=True)
                mutant_cpp = folder / 'broker-output.cc'
                mutant_cpp.write_text(generated(mutant))
                mutant_exe = folder / executable.name
                mutant_command = command[:-3] + [str(mutant_cpp), '-o', str(mutant_exe)]
                subprocess.run(mutant_command, check=True, timeout=120)
                result = subprocess.run([str(mutant_exe), 'gate'], capture_output=True, text=True, timeout=120)
                (folder / 'result.log').write_text(result.stdout + result.stderr)
                expected = 'FAIL broker output: ' + assertion
                if result.returncode == 0 or expected not in result.stderr:
                    raise RuntimeError(f'{name} did not fail its intended production assertion: {result.returncode}\n'
                                       + result.stdout + result.stderr)
                evidence.append({'name': name, 'source_sha256': hashlib.sha256(mutant.encode()).hexdigest(),
                                 'command': mutant_command, 'returncode': result.returncode, 'assertion': expected})
                print('BINDER_OUTPUT_NEGATIVE_PASS ' + name, flush=True)
            (work / 'negative-controls.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()
