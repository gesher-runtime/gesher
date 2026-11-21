#!/usr/bin/env python3
"""Exercise the production ART JNI installer with controlled OS/VM boundaries.

The host fixture executes the actual generated x86-64 dispatch stubs with both
ABIs, and injects allocation/protection/cache failures before publication. This
also extracts the production proxy representation and identity helpers. Its
declared real-environment boundary retains ART16r4's thread/VM prefix; it is not
an ART runtime. This does not replace the Windows ART/native-library tests.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-ref')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()

    def read(path):
        if args.source_ref:
            return subprocess.run(['git', 'show', args.source_ref + ':' + path], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout
        return (ROOT / path).read_text(encoding='utf-8')

    source = read('compat/bionic/jni_bridge.cc')
    start = re.search(r'^(?:void|bool) InstallCallerAbiJniTable\(\) \{', source, re.M)
    if start is None:
        raise ValueError('ART JNI installer boundary changed')
    end = source.index('\nvoid* ElfOpen(', start.start())
    implementation = source[start.start():end]
    declaration = re.findall(r'^extern "C" .* gesher_art_load_class_hook;', source, re.M)
    patch = read('patches/art/0800-packer-load-class-hook.patch')
    definition = re.findall(r'^\+((?:std::atomic<void\*>|void\* volatile) '
                            r'gesher_art_load_class_hook[^;]*;)$', patch, re.M)
    hook_read = re.findall(r'^\+  if \((void\* hook = [^;]+);$', patch, re.M)
    real_start, real_end = 'inline JNIEnv* Real(JNIEnv* env) {', '\n// --- stubs'
    real = source[source.index(real_start):source.index(real_end, source.index(real_start))]
    identity = {}
    for marker, begin, end in [
            ('@PROXY_ENV@', 'struct ProxyEnv {', '\nstruct ProxyVm {'),
            ('@GUEST_ENV@', 'bool IsGuestEnv(JNIEnv* env) {', '\nbool IsGuestVm(')]:
        if source.count(begin) != 1 or source.count(end) != 1:
            raise ValueError('proxy identity boundary changed: ' + begin)
        identity[marker] = source[source.index(begin):source.index(end, source.index(begin))]
    if len(declaration) != 1 or len(definition) != 1 or len(hook_read) != 1:
        raise ValueError('ART hook publication boundaries changed')
    fixture = (ROOT / 'tests/native/art_jni_dispatch_host.cc.in').read_text(encoding='utf-8')
    for marker, value in [('@INSTALLER@', implementation), ('@HOOK_DECLARATION@', declaration[0]),
                          ('@HOOK_DEFINITION@', definition[0]), ('@HOOK_READ@', hook_read[0] + ';'),
                          ('@REAL_ENV@', real), *identity.items()]:
        if fixture.count(marker) != 1:
            raise ValueError('fixture marker changed: ' + marker)
        fixture = fixture.replace(marker, value)
    print('ART JNI source SHA256 ' + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-art-jni-') as temporary:
        folder = Path(temporary)
        cpp, binary = folder / 'test.cc', folder / 'test'
        cpp.write_text(fixture, encoding='utf-8')
        command = shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++20', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
            str(cpp), '-o', str(binary)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        subprocess.run(command, check=True, timeout=120)
        environment = dict(os.environ)
        environment.pop('GESHER_ART_SHIM', None)
        failed = False
        for scenario in ['null-table', 'first-allocation', 'second-allocation',
                         'code-protection', 'table-protection', 'cache-flush',
                         'architecture', 'success', 'concurrent', 'proxy-identity']:
            case_folder = folder / scenario
            case_folder.mkdir()
            result = subprocess.run([str(binary), scenario], cwd=case_folder,
                                    env=environment, timeout=30)
            failed |= result.returncode != 0
        return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
