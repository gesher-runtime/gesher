"""CI-only exact-source component roles; leaves historical full guards intact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = 'f1550540186b4ae3f17a5e501a886ec1c4d5d613'
ROLE = 'bionic-udp-truncation-component-v1'
MEMBERS = {'gesher-exec.exe', 'gesher_arm64.dll', 'arm64_smoke_test.exe',
           'net-udp-x86_64', 'net-udp-arm64', 'net-udp-fault-x86_64', 'net-udp-fault-arm64',
           'imports-host.txt', 'imports-arm64.txt', 'linux-reference.log',
           'linux-fault-reference.log', 'linux-commands.json', 'compiler.json', 'guest-commands.json'}
CPP = {'compat/posix/src/socket.cc', 'compat/posix/src/winsock.cc',
       'compat/bionic/socket_bionic.cc'}
DLL_CPP = {'compat/arm64/dll/translator.cc', 'compat/arm64/dll/guest_libc.cc'}
TOOLCHAIN = '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pairs(rows):
    result = {}
    for key, value in rows:
        require(key not in result, 'Duplicate JSON key')
        result[key] = value
    return result


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=pairs)


def inventory():
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    result = {}
    for name in filter(None, names):
        path = ROOT / name
        require(path.is_file() and not path.is_symlink(), 'Missing or linked source: ' + name)
        result[name] = sha(path)
    return result


def context():
    require(os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only')
    result = {key: os.environ[value] for key, value in (
        ('source', 'GITHUB_SHA'), ('run', 'GITHUB_RUN_ID'), ('attempt', 'GITHUB_RUN_ATTEMPT'),
        ('repository', 'GITHUB_REPOSITORY'), ('repository_id', 'GITHUB_REPOSITORY_ID'))}
    require(result['repository'] == 'gesher-runtime/gesher', 'Wrong repository')
    require(subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip() == result['source'], 'Checkout mismatch')
    require(subprocess.call(['git', 'diff', '--quiet', 'HEAD', '--'], cwd=ROOT) == 0, 'Dirty source')
    return result


def preflight():
    context()
    pins = read(ROOT / 'tools/net-udp-component/source.json')
    require(pins['base'] == BASE, 'Baseline mismatch')
    subprocess.run(['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'], cwd=ROOT, check=True)
    changed = subprocess.check_output(['git', 'diff', '--name-only', BASE, 'HEAD'], cwd=ROOT, text=True).splitlines()
    allowed = set(pins['files']) | {'tools/net-udp-component/source.json'}
    require(all(name in allowed or name.startswith('docs/') for name in changed), 'Unreviewed source delta')
    actual = inventory()
    require(all(actual.get(name) == digest for name, digest in pins['files'].items()), 'Pinned source bytes differ')
    print('UDP_COMPONENT_SOURCE_RECEIPT', json.dumps(context(), sort_keys=True), 'files=' + str(len(actual)))
    return actual


def compiler(build, expected):
    records = list((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    require(len(records) == 1, 'Ambiguous generated compiler record')
    values = re.findall(r'^\s*set\(CMAKE_CXX_COMPILER\s+"([^"]+)"\)', records[0].read_text(), re.M)
    require(len(values) == 1, 'Missing actual compiler driver')
    driver = Path(values[0]).resolve(strict=True)
    require(Path(values[0]).is_absolute(), 'Compiler must be absolute')
    commands = read(build / 'compile_commands.json')
    seen = {}
    for row in commands:
        source = Path(row['file']).resolve().relative_to(ROOT).as_posix()
        if source not in expected:
            continue
        args = row.get('arguments') or shlex.split(row['command'])
        require(Path(args[0]).resolve(strict=True) == driver, 'Compile driver differs from generated compiler')
        require({'-Wall', '-Wextra', '-Werror'} <= set(args), 'Missing warnings-as-errors flags')
        require(source not in seen, 'Duplicate target source command')
        seen[source] = args
    require(set(seen) == expected, 'Missing changed actual C++ commands')
    return {'driver': str(driver), 'driver_sha256': sha(driver),
            'version': subprocess.check_output([str(driver), '--version'], text=True),
            'record_sha256': sha(records[0]), 'commands': seen, 'toolchain_sha256': TOOLCHAIN}


def files(folder):
    entries = list(folder.iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in entries), 'Nonregular payload member')
    return {p.name: sha(p) for p in entries}


def validate(data, expected_context, sources, actual):
    require(data['context'] == expected_context and data['role'] == ROLE, 'Source/attempt/role mismatch')
    require(data['sources'] == sources, 'Whole source inventory differs')
    require(set(actual) == MEMBERS and data['outputs'] == actual, 'Whole members or digests differ')


def selftest():
    sources = {'source': 'digest'}
    outputs = {name: name for name in MEMBERS}
    good = {'context': {'run': '1'}, 'role': ROLE, 'sources': sources, 'outputs': outputs}
    validate(good, {'run': '1'}, sources, outputs)
    controls = [dict(good, role='wrong'), dict(good, context={'run': '2'}),
                dict(good, sources={}), dict(good, outputs={}),
                dict(good, outputs=dict(outputs, **{'imports.txt': 'wrong'}))]
    for changed in controls:
        try:
            validate(changed, {'run': '1'}, sources, outputs)
        except RuntimeError:
            continue
        raise RuntimeError('Mutation control accepted')
    for actual in (dict(outputs, unexpected='extra'), {key: value for key, value in outputs.items() if key != 'compiler.json'}):
        try:
            validate(good, {'run': '1'}, sources, actual)
        except RuntimeError:
            continue
        raise RuntimeError('Whole-member mutation accepted')
    for text in ('{"x":1,"x":2}',):
        try:
            json.loads(text, object_pairs_hook=pairs)
        except RuntimeError:
            continue
        raise RuntimeError('Duplicate JSON accepted')
    print('UDP_COMPONENT_RECEIPT_CONTROLS rejected=8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('preflight', 'record', 'verify', 'selftest', 'deps', 'guests', 'reference'))
    parser.add_argument('--directory', type=Path, default=Path('udp-fixture'))
    args = parser.parse_args()
    if args.mode == 'selftest':
        selftest()
        return
    sources = preflight()
    if args.mode == 'preflight':
        return
    if args.mode == 'reference':
        args.directory.mkdir()
        compilers = []
        with tempfile.TemporaryDirectory(prefix='net-udp-linux-') as temporary:
            for name, language, source, output in (
                ('g++', 'c++20', 'tests/native2/net_udp_trunc.cpp', 'linux-reference.log'),
                ('cc', 'c11', 'tests/native2/net_udp_fault_observer.c', 'linux-fault-reference.log')):
                driver = Path(shutil.which(name)).resolve(strict=True)
                executable = Path(temporary) / name.replace('+', 'p')
                command = [str(driver), '-std=' + language, '-O2', '-Wall', '-Wextra', '-Werror',
                           '-pthread', source, '-o', str(executable)]
                version = subprocess.check_output([str(driver), '--version'], text=True)
                print('UDP_LINUX_COMPILER', json.dumps(dict(command=command, driver_sha256=sha(driver),
                                                           version=version), sort_keys=True), flush=True)
                subprocess.run(command, cwd=ROOT, check=True, timeout=120)
                actual = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120)
                print(actual.stdout, end='', flush=True)
                print(actual.stderr, end='', flush=True)
                print('UDP_LINUX_ACTUAL_EXIT', actual.returncode, 'command=' + str(executable), flush=True)
                require(actual.returncode == 0, 'Actual Linux reference failed: ' + source)
                (args.directory / output).write_text(actual.stdout)
                compilers.append({'command': command, 'driver_sha256': sha(driver),
                                  'version': version,
                                  'executed_binary_sha256': sha(executable)})
                if name == 'g++':
                    negative = subprocess.run([str(executable), '--negative'], capture_output=True, text=True, timeout=30)
                    require(negative.returncode == 1 and 'FAIL UDP: negative missing MSG_TRUNC rejected' in
                            negative.stdout + negative.stderr, 'Actual Linux negative not rejected')
                    print(negative.stdout + negative.stderr, flush=True)
        (args.directory / 'linux-commands.json').write_text(json.dumps(
            {'context': context(), 'compilers': compilers}, sort_keys=True) + '\n')
        return
    if args.mode == 'deps':
        # Reuse the byte-pinned canonical dependency recipe unchanged. Its
        # entire workflow is required unchanged from BASE by preflight.
        lines = (ROOT / '.github/workflows/arm64.yml').read_text().splitlines()
        start = lines.index('      - name: Fetch and patch dynarmic, fmt and Boost')
        end = lines.index('      - name: Verify and patch the pinned compiler FPSR boundary')
        body = lines[start:end]
        run = body.index('        run: |')
        script = '\n'.join(line[10:] if line.startswith('          ') else '' for line in body[run + 1:])
        require(script.startswith('set -euo pipefail'), 'Canonical dependency body malformed')
        Path('cpu-clock-arm64-deps.sh').write_text(script + '\n')
        return
    if args.mode == 'guests':
        ndk = ROOT / 'android-ndk-r27c'
        props = (ndk / 'source.properties').read_text()
        require('27.2.12479018' in props, 'Unexpected NDK revision')
        bindir = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
        receipts = {}
        for abi, triple in (('x86_64', 'x86_64-linux-android'), ('arm64', 'aarch64-linux-android')):
            rows = []
            for source, suffix, language in (('tests/native2/net_udp_trunc.cpp', '', '++'),
                                             ('tests/native2/net_udp_fault_observer.c', '-fault', '')):
                driver = bindir / (triple + '29-clang' + language)
                command = [str(driver), '-std=c++20' if language else '-std=c11', '-O2',
                           '-Wall', '-Wextra', '-Werror']
                if language:
                    command.append('-static-libstdc++')
                command += [source, '-o', str(args.directory / ('net-udp' + suffix + '-' + abi))]
                subprocess.run(command, cwd=ROOT, check=True)
                rows.append({'command': command, 'driver_sha256': sha(driver),
                             'clang_sha256': sha((bindir / ('clang' + language)).resolve()),
                             'version': subprocess.check_output([str(driver), '--version'], text=True)})
            receipts[abi] = rows
        (args.directory / 'guest-commands.json').write_text(json.dumps(
            {'context': context(), 'ndk_properties': props, 'archive_sha256': sha(ROOT / 'ndk.zip'),
             'publisher_archive_sha1': '090e8083a715fdb1a3e402d0763c388abb03fb4e',
             'compilers': receipts}, sort_keys=True) + '\n')
        return
    folder = args.directory
    if args.mode == 'record':
        host = compiler(ROOT / 'out', CPP)
        dll = compiler(ROOT / 'out-arm64', DLL_CPP)
        require(host['driver_sha256'] == dll['driver_sha256'], 'Host/DLL compiler mismatch')
        (folder / 'compiler.json').write_text(json.dumps({'host': host, 'arm64_dll': dll}, sort_keys=True) + '\n')
        actual = files(folder)
        require(set(actual) == MEMBERS, 'Unexpected producer members')
        data = {'context': context(), 'role': ROLE, 'sources': sources, 'outputs': actual}
        (folder / 'provenance.json').write_text(json.dumps(data, sort_keys=True) + '\n')
    else:
        actual = files(folder)
        require(set(actual) == MEMBERS | {'provenance.json'}, 'Unexpected whole archive members')
        del actual['provenance.json']
        data = read(folder / 'provenance.json')
        validate(data, context(), sources, actual)
        for role in ('host', 'arm64_dll'):
            require(read(folder / 'compiler.json')[role]['toolchain_sha256'] == TOOLCHAIN, 'Compiler supplier mismatch')
        require(read(folder / 'guest-commands.json')['context'] == context(), 'Guest compiler context differs')
        require(read(folder / 'linux-commands.json')['context'] == context(), 'Linux reference compiler context differs')
        allowed = {'kernel32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'ntdll.dll',
                   'ws2_32.dll', 'iphlpapi.dll', 'psapi.dll', 'bcrypt.dll', 'user32.dll',
                   'api-ms-win-core-synch-l1-2-0.dll'}
        for member in ('imports-host.txt', 'imports-arm64.txt'):
            names = re.findall(r'^\s*Name:\s*(\S+)', (folder / member).read_text(), re.M)
            require(bool(names) and all(name.lower() in allowed or
                    re.fullmatch(r'api-ms-win-crt-[a-z]+-l1-1-0.dll', name.lower()) for name in names),
                    'Unexpected or missing actual PE imports')
        main_rows = (folder / 'linux-reference.log').read_text().splitlines()
        require(len(main_rows) == 200 and sum(' api=' in row for row in main_rows) == 192 and
                main_rows[-1] == 'UDP all passed; rows=192; alias=128; stream=1', 'Missing actual Linux main rows')
        faults = (folder / 'linux-fault-reference.log').read_text().splitlines()
        require(len(faults) == 176 and all(row.startswith('UDPFAULT ') for row in faults), 'Missing actual Linux fault rows')
        for abi, machine in (('x86_64', 62), ('arm64', 183)):
            for name in ('net-udp-' + abi, 'net-udp-fault-' + abi):
                elf = (folder / name).read_bytes()
                require(elf[:6] == b'\x7fELF\x02\x01' and
                        int.from_bytes(elf[18:20], 'little') == machine, 'Wrong actual guest ELF ABI: ' + name)
        for name in ('gesher-exec.exe', 'gesher_arm64.dll', 'arm64_smoke_test.exe'):
            pe = (folder / name).read_bytes()
            offset = int.from_bytes(pe[60:64], 'little')
            require(pe[:2] == b'MZ' and pe[offset:offset + 4] == b'PE\0\0' and
                    int.from_bytes(pe[offset + 4:offset + 6], 'little') == 0x8664, 'Wrong actual host PE role: ' + name)
        print('UDP_COMPONENT_VERIFIED_MEMBERS', json.dumps(data['outputs'], sort_keys=True), json.dumps(context(), sort_keys=True))


if __name__ == '__main__':
    main()
