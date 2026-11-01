"""CI-only exact-source component roles; leaves historical full guards intact."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = 'f1550540186b4ae3f17a5e501a886ec1c4d5d613'
ROLE = 'bionic-retained-cpu-clock-core-v1'
MEMBERS = {'pthread_cpu_clock_target_test.exe', 'gesher-exec.exe', 'gesher_arm64.dll',
           'arm64_smoke_test.exe', 'cpu-clock-x86_64', 'cpu-clock-arm64',
           'imports-core.txt', 'imports-host.txt', 'imports-arm64.txt',
           'linux-reference.log', 'compiler.json', 'guest-commands.json'}
CPP = {'compat/bionic/pthread_bionic.cc', 'compat/bionic/shim.cc',
       'compat/bionic/extra_sys_bionic.cc', 'tests/native/pthread_cpu_clock_target_test.cc'}
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
    pins = read(ROOT / 'tools/pthread-cpu-clock/source.json')
    require(pins['base'] == BASE, 'Baseline mismatch')
    subprocess.run(['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'], cwd=ROOT, check=True)
    changed = subprocess.check_output(['git', 'diff', '--name-only', BASE, 'HEAD'], cwd=ROOT, text=True).splitlines()
    allowed = set(pins['files']) | {'tools/pthread-cpu-clock/source.json'}
    require(all(name in allowed or name.startswith('docs/') for name in changed), 'Unreviewed source delta')
    actual = inventory()
    require(all(actual.get(name) == digest for name, digest in pins['files'].items()), 'Pinned source bytes differ')
    print('CPU_CLOCK_SOURCE_RECEIPT', json.dumps(context(), sort_keys=True), 'files=' + str(len(actual)))
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


def linux_phase_records(text):
    records = {}
    for kind in ('BIRTH', 'JOIN', 'FAST', 'RELEASE'):
        rows = [line.split()[1:] for line in text.splitlines() if line.startswith('LINUX_' + kind + ' ')]
        require(len(rows) == 64, 'Incomplete actual Linux ' + kind + ' records')
        values = [dict(item.split('=', 1) for item in row) for row in rows]
        require([int(row['row']) for row in values] == list(range(64)), 'Linux rows missing or duplicated')
        records[kind] = values
    require(len([line for line in text.splitlines() if line.startswith('LINUX_PLATFORM ')]) == 1,
            'Missing actual kernel/libc identity')
    seen = set()
    live = 0
    dead = 0
    for index in range(64):
        birth, joined, fast, released = (records[kind][index] for kind in ('BIRTH', 'JOIN', 'FAST', 'RELEASE'))
        tid = int(birth['tid'])
        require(tid > 0 and tid == int(birth['decoded']) and tid not in seen, 'Wrong or reused Linux clock target')
        seen.add(tid)
        require(int(birth['birth']) > 0 and birth['tasks'] == '2' and
                birth['time'] == '0/0' and birth['res'] == '0/0', 'Linux live birth is not established')
        for phase in (joined, fast, released):
            require(phase['tid'] == birth['tid'] and phase['birth'] == birth['birth'], 'Linux birth changed across phases')
        require(int(joined['release_ns']) <= int(joined['joined_ns']) <= int(joined['sampled_ns']), 'Linux phase ordering differs')
        for key in ('before', 'after'):
            present, state, stamp = joined[key].split('/')
            require((present == '0' and stamp == '0') or
                    (present == '1' and stamp == birth['birth'] and state != '-'), 'Linux proc identity changed')
        for key in ('time', 'res'):
            result, error, nanos = fast[key].split('/')
            require((result, error) in {('0', '0'), ('-1', '22')} and int(nanos) >= 0,
                    'Unexpected actual immediate Linux result')
        live += fast['time'].startswith('0/0/')
        dead += fast['time'].startswith('-1/22/')
        require(released['present'] == '0/-' and released['time'] == '-1/22' and
                released['res'] == '-1/22' and released['tasks'] == '1' and
                0 <= int(released['elapsed_ns']) <= 5100000000 and int(released['observations']) > 0,
                'Actual identity-bound final Linux task release not established')
    summary = f'LINUX_IDENTITY_DIAGNOSTIC rows=64 join_live={live} join_dead={dead} controlled_final_release=64 reused_tids=0'
    require(summary in text.splitlines(), 'Actual Linux phase counts differ')
    return {'rows': 64, 'join_live': live, 'join_dead': dead, 'controlled_final_release': 64, 'reused_tids': 0}


def linux_reference(text):
    actual = linux_phase_records(text)
    require('PASS Linux retained pthread CPU clock: 64 identity-bound final releases reject gettime/getres with EINVAL' in text.splitlines(),
            'Asserted final-release Linux oracle absent')
    changed_birth = re.sub(r'(LINUX_FAST row=0 tid=\d+ birth=)(\d+)',
                           lambda match: match[1] + str(int(match[2]) + 1), text, count=1)
    mutations = (text.replace('LINUX_BIRTH row=0 ', 'MISSING_BIRTH row=0 ', 1),
                 text.replace('time=-1/22 res=-1/22 tasks=1', 'time=0/0 res=-1/22 tasks=1', 1),
                 changed_birth)
    for changed in mutations:
        require(changed != text, 'Actual Linux negative control did not mutate a record')
        try:
            linux_phase_records(changed)
        except RuntimeError:
            continue
        raise RuntimeError('Actual Linux missing-row/final-error/birth mutation accepted')
    print('CPU_CLOCK_LINUX_ADVERSARIAL rejected=3')
    return actual


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
    print('CPU_CLOCK_RECEIPT_CONTROLS rejected=8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('preflight', 'record', 'verify', 'selftest', 'deps', 'guests'))
    parser.add_argument('--directory', type=Path, default=Path('cpu-clock-fixture'))
    args = parser.parse_args()
    if args.mode == 'selftest':
        selftest()
        return
    sources = preflight()
    if args.mode == 'preflight':
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
            driver = bindir / (triple + '29-clang++')
            command = [str(driver), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-static-libstdc++', 'tests/native/pthread_cpu_clock_guest.cc',
                       '-o', str(args.directory / ('cpu-clock-' + abi))]
            subprocess.run(command, cwd=ROOT, check=True)
            receipts[abi] = {'command': command, 'driver_sha256': sha(driver),
                             'clang_sha256': sha((bindir / 'clang++').resolve()),
                             'version': subprocess.check_output([str(driver), '--version'], text=True)}
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
        allowed = {'kernel32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'ntdll.dll',
                   'ws2_32.dll', 'iphlpapi.dll', 'psapi.dll', 'bcrypt.dll', 'user32.dll',
                   'api-ms-win-core-synch-l1-2-0.dll'}
        checks = []
        for role, member, binary in (('core', 'imports-core.txt', 'pthread_cpu_clock_target_test.exe'),
                                     ('host', 'imports-host.txt', 'gesher-exec.exe'),
                                     ('arm64', 'imports-arm64.txt', 'gesher_arm64.dll')):
            raw = (folder / member).read_text()
            names = re.findall(r'^\s*Name:\s*(\S+)', raw, re.M)
            extras = [name for name in names if name.lower() not in allowed and not
                      re.fullmatch(r'api-ms-win-crt-[a-z]+-l1-1-0.dll', name.lower())]
            print('CPU_CLOCK_PE_IMPORT_DIAGNOSTIC', json.dumps({
                'role': role, 'file': binary, 'sha256': actual[binary],
                'imports_file': member, 'imports_sha256': actual[member],
                'raw_coff_imports': raw, 'parsed_names': names,
                'missing': not bool(names), 'extras': extras}, sort_keys=True), flush=True)
            checks.append(bool(names) and not extras)
        require(all(checks), 'Unexpected or missing actual PE imports')
        print('CPU_CLOCK_LINUX_PHASE_RECEIPT', json.dumps(linux_reference(
            (folder / 'linux-reference.log').read_text()), sort_keys=True))
        print('CPU_CLOCK_VERIFIED_MEMBERS', json.dumps(data['outputs'], sort_keys=True), json.dumps(context(), sort_keys=True))


if __name__ == '__main__':
    main()
