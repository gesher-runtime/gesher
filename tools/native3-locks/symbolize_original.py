"""CI-only analysis of the exact archived worker from the failed WCT run.

This does not execute the Windows worker, rebuild it or qualify its runtime.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

SOURCE = '4f8ad228d02157fdbd38d6b87d02688ef6a777b7'
RUN = 40674284726
ATTEMPT = 1
JOB = 122768663211
ARTIFACT = 12809029365
ARCHIVE = 'sha256:3c91e9cb37f3d6ac5df3cb7c572a5a45559d774eeb02df91250a16c71f3c1e4b'
TOOLCHAIN = '5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4'
OUTPUTS = {
    'native3_lock_command_worker.exe': '10c93728553e8ed90cb5d89450c8b090ad392e98761f26a1ee273ad70af29734',
    'linux-reference.json': '3080bbfab5e57a197194217e8900527c6bef527fa30da39f2366d1b9e58ad0f1',
    'native3_lock_duplicate_test.imports.txt': 'c2e3d8516034c6f18558cb2a01a9fe9e517c76e893d3a7886a285604403eb4a7',
    'binderd.exe': '6fbf979fce20b157a2c71e09278f2685cbadb0a06a881faafc40488d6ad229bd',
    'native3_lock_duplicate_test.exe': '259babfe8033e488717a7e71f20c89a3d27e171e8cbe8f5bf6493b08a78908af',
    'native3_lock_command_worker.imports.txt': '98a3a3da9d8e1a59e27e84b382decd8eaad296700b24ec2c94eb8503e36ed198',
    'binderd.imports.txt': 'e9c93ca16e9f5221639ac6d6d7e38b9ce97a237ee97b0b7383b33ac111db4598',
}


def command(arguments):
    return subprocess.check_output([str(arg) for arg in arguments], text=True)


def api(path):
    return json.loads(command(['gh', 'api', f'repos/gesher-runtime/gesher/{path}']))


def instant(text):
    return datetime.datetime.fromisoformat(text.replace('Z', '+00:00'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--toolchain', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--diagnostic', choices=('dwarf', 'iat'), default='dwarf')
    args = parser.parse_args()
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only'
    assert os.environ['GITHUB_REPOSITORY'] == 'gesher-runtime/gesher'
    assert command(['git', 'rev-parse', 'HEAD']).strip() == os.environ['GITHUB_SHA']
    assert command(['git', '-C', args.source_root, 'rev-parse', 'HEAD']).strip() == SOURCE
    repository_id = int(os.environ['GITHUB_REPOSITORY_ID'])
    run = api(f'actions/runs/{RUN}/attempts/{ATTEMPT}')
    assert (run['id'], run['run_attempt'], run['head_sha'], run['status'], run['conclusion']) == (
        RUN, ATTEMPT, SOURCE, 'completed', 'failure')
    assert run['path'] == '.github/workflows/native3-locks-commands.yml'
    assert run['repository']['id'] == run['head_repository']['id'] == repository_id
    jobs = api(f'actions/runs/{RUN}/attempts/{ATTEMPT}/jobs?per_page=100')
    assert jobs['total_count'] == len(jobs['jobs']) < 100
    producers = [job for job in jobs['jobs'] if job['name'] == 'compile']
    assert len(producers) == 1
    job = producers[0]
    assert (job['id'], job['run_id'], job['run_attempt'], job['head_sha'], job['conclusion']) == (
        JOB, RUN, ATTEMPT, SOURCE, 'success')
    uploads = [step for step in job['steps'] if step['name'] == 'Run actions/upload-artifact@v4']
    assert len(uploads) == 1 and uploads[0]['conclusion'] == 'success'
    artifacts = api(f'actions/runs/{RUN}/artifacts?per_page=100')
    assert artifacts['total_count'] == len(artifacts['artifacts']) < 100
    matches = [row for row in artifacts['artifacts'] if row['name'] == f'native3-locks-commands-{SOURCE}-a{ATTEMPT}']
    assert len(matches) == 1
    artifact = matches[0]
    assert artifact['id'] == ARTIFACT and artifact['digest'] == ARCHIVE
    assert not artifact['expired'] and artifact['size_in_bytes'] > 0
    assert artifact['workflow_run']['id'] == RUN and artifact['workflow_run']['head_sha'] == SOURCE
    assert artifact['workflow_run']['repository_id'] == artifact['workflow_run']['head_repository_id'] == repository_id
    assert instant(job['started_at']) <= instant(artifact['created_at']) <= instant(job['completed_at'])
    print(f'ORIGINAL_PRODUCER source={SOURCE} run={RUN} attempt={ATTEMPT} job={JOB} artifact={ARTIFACT} digest={ARCHIVE}', flush=True)
    helper = Path('tools/native3-locks/artifact/download-verified-artifact.py')
    assert command(['git', 'hash-object', helper]).strip() == 'cd5cc415e53102c664edac22b1604df639c8804b'
    subprocess.run(['python3', str(helper), '--id', str(ARTIFACT), '--digest', ARCHIVE,
                    '--destination', str(args.destination)], check=True)
    assert all(path.is_file() for path in args.destination.iterdir())
    assert {path.name for path in args.destination.iterdir()} == set(OUTPUTS) | {'provenance.json'}
    manifest = json.loads((args.destination / 'provenance.json').read_text())
    assert manifest['repository'] == 'gesher-runtime/gesher' and int(manifest['repository_id']) == repository_id
    assert (manifest['source'], int(manifest['run']), int(manifest['attempt']), manifest['toolchain_sha256']) == (
        SOURCE, RUN, ATTEMPT, TOOLCHAIN)
    assert manifest['outputs'] == OUTPUTS
    paths = command(['git', '-C', args.source_root, 'ls-files', '-z']).split('\0')
    actual_files = {name: digest(args.source_root / name) for name in paths if name and (args.source_root / name).is_file()}
    assert actual_files == manifest['files'], 'Original whole tracked source manifest differs'
    for name, expected in OUTPUTS.items():
        assert digest(args.destination / name) == expected, name
        print(f'ORIGINAL_OUTPUT name={name} sha256={expected}', flush=True)
    binary = args.destination / 'native3_lock_command_worker.exe'
    tool = args.toolchain / 'bin'
    print(command([tool / 'clang', '--version']), flush=True)
    headers = command([tool / 'llvm-readobj', '--file-headers', binary])
    print(headers, flush=True)
    bases = re.findall(r'^\s*ImageBase:\s*(0x[0-9A-Fa-f]+)\s*$', headers, re.MULTILINE)
    assert len(bases) == 1
    base = int(bases[0], 16)
    if args.diagnostic == 'iat':
        # Read actual archived PE instruction bytes, not a newly linked image.
        # Section RVAs and raw offsets come from its own PE section table.
        data = binary.read_bytes()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        assert data[pe:pe + 4] == b'PE\0\0'
        sections = struct.unpack_from('<H', data, pe + 6)[0]
        optional_size = struct.unpack_from('<H', data, pe + 20)[0]
        table = pe + 24 + optional_size

        def bytes_at(rva, size):
            matches = []
            for index in range(sections):
                section = table + index * 40
                virtual, raw_size, raw = struct.unpack_from('<III', data, section + 12)
                if virtual <= rva and rva + size <= virtual + raw_size:
                    offset = raw + rva - virtual
                    assert offset + size <= len(data)
                    matches.append(data[offset:offset + size])
            assert len(matches) == 1, ('Ambiguous or non-file-backed RVA', rva)
            return matches[0]

        indirect = bytes_at(0x9830, 6)
        assert indirect[:2] == b'\xff\x15'
        iat = 0x9836 + struct.unpack_from('<i', indirect, 2)[0]
        direct = bytes_at(0x39fd, 5)
        assert direct[0] == 0xe8
        destination = 0x3a02 + struct.unpack_from('<i', direct, 1)[0]
        assert iat == 0xc42a0 and destination == 0x9820
        imports = command([tool / 'llvm-readobj', '--coff-imports', binary])
        print(imports, flush=True)
        matches = []
        for block in re.findall(r'^Import \{\n(.*?)^\}', imports, re.MULTILINE | re.DOTALL):
            dll = re.findall(r'^\s*Name: (\S+)\s*$', block, re.MULTILINE)
            addresses = re.findall(r'^\s*ImportAddressTableRVA: (0x[0-9a-fA-F]+)\s*$', block, re.MULTILINE)
            symbols = re.findall(r'^\s*Symbol: (.*?) \([0-9]+\)\s*$', block, re.MULTILINE)
            assert len(dll) == len(addresses) == 1 and symbols
            first = int(addresses[0], 16)
            for index, symbol in enumerate(symbols):
                if first + 8 * index == iat:
                    matches.append((dll[0], symbol))
        assert len(matches) == 1 and matches[0][1], ('Unique named original IAT slot not found', matches)
        print(f'ORIGINAL_DIRECT_CALL return_rva=0x3a02 call_rva=0x39fd target_rva={hex(destination)} bytes={direct.hex()}', flush=True)
        print(f'ORIGINAL_IAT_CALL return_rva=0x9836 call_rva=0x9830 slot_rva={hex(iat)} dll={matches[0][0]} symbol={matches[0][1]} bytes={indirect.hex()}', flush=True)
        print('ORIGINAL_IAT_DIAGNOSTIC_COMPLETE selected_calls=2 private_symbols=false lock_owner_proof=false runtime_acceptance=false', flush=True)
        return
    unresolved = []
    for rva in (0x9836, 0x3a02):
        # An unwound caller PC is the return address; include its preceding
        # instruction as well, without guessing which function owns the PC.
        for adjustment in (0, -1):
            va = base + rva + adjustment
            result = command([tool / 'llvm-addr2line', '-a', '-f', '-C', '-i', '-e', binary, hex(va)])
            print(f'ORIGINAL_DWARF rva={hex(rva)} adjustment={adjustment} va={hex(va)}\n{result}', flush=True)
            if '??' in result:
                unresolved.append((rva, adjustment))
        print(command([tool / 'llvm-objdump', '--disassemble', '--demangle', '--line-numbers',
                       '--start-address=' + hex(base + rva - 64),
                       '--stop-address=' + hex(base + rva + 32), binary]), flush=True)
    assert not unresolved, ('Original DWARF mapping incomplete', unresolved)
    print('ORIGINAL_BINARY_SYMBOLIZATION_COMPLETE selected_rvas=2 runtime_acceptance=false', flush=True)


if __name__ == '__main__':
    main()
