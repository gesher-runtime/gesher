"""CI-only NDK compilation with the executed compiler identity and argv recorded."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

assert os.environ.get('GITHUB_ACTIONS') == 'true'
out = Path('tcp-fixture')
out.mkdir()
ndk = Path(os.environ['NDK_DIR']).resolve()
records = {}
for abi, triple in [('x86_64', 'x86_64-linux-android'), ('arm64-v8a', 'aarch64-linux-android')]:
    compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin' / (triple + '29-clang++')
    target = out / ('tcp-priority-' + abi)
    argv = [str(compiler), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static-libstdc++', '-pthread', 'tests/net-tcp-priority/contract.cc', '-o', str(target)]
    subprocess.check_call(argv)
    backend = compiler.parent / 'clang++'
    records[abi] = dict(backend_resolved=str(backend.resolve(strict=True)), backend_sha256=hashlib.sha256(backend.read_bytes()).hexdigest(), argv=argv, configured=str(compiler), resolved=str(compiler.resolve(strict=True)), sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), version=subprocess.check_output([str(compiler), '--version'], text=True))
for abi, triple in [('x86_64', 'x86_64-linux-android'), ('arm64-v8a', 'aarch64-linux-android')]:
    records[abi]['guarded_commands'] = []
    for source, name, language in [('tests/native2/net_udp_trunc.cpp', 'udp-guarded', '++'), ('tests/native2/net_udp_fault_observer.c', 'udp-guarded-fault', '')]:
        compiler = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin' / (triple + '29-clang' + language)
        target = out / (name + '-' + abi)
        argv = [str(compiler), '-std=c++20' if language else '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
        if language: argv += ['-static-libstdc++', '-pthread']
        argv += [source, '-o', str(target)]
        subprocess.check_call(argv)
        records[abi]['guarded_commands'].append(dict(source=source, output=target.name, argv=argv,
            driver_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
Path('tcp-guests-compiler.json').write_text(json.dumps(records))
