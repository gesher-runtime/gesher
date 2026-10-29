"""Canonical native2 DNS suppliers; compilation and execution are CI only.

The enclosing native2 archive is independently checked by artifact_receipt.py.
This manifest binds its DNS subset to source, attempt, compiler and ABI roles.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/net-dns-component'))
from aosp_reference import run_reference

ASSETS = {'linux-reference.log': 'executed-linux-libresolv',
          'compilers.json': 'actual-compiler-commands-and-identities'}
for ABI in ('x86_64', 'arm64'):
    ASSETS['aosp-reference-' + ABI + '.log'] = 'executed-pinned-aosp-common-' + ABI + '-char-model'
    ASSETS['aosp-extended-' + ABI + '.log'] = 'executed-pinned-aosp-extended-' + ABI + '-char-model'
ROLES = {'net-dns/' + name: role for name, role in ASSETS.items()}
ROLES.update({abi + '/net_dns_wire': 'actual-ndk-api29-dns-' + abi
              for abi in ('x86_64', 'arm64-v8a')})
SOURCES = ('tests/native2/net_dns_wire.cpp', 'tests/native2/dns-reference/ns_name.c',
           'tools/net-dns/payload.py', 'tools/net-dns-component/aosp_reference.py')


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pairs(rows):
    value = {}
    for key, item in rows:
        require(key not in value, 'Duplicate JSON member')
        value[key] = item
    return value


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=pairs)


def context():
    require(os.environ.get('GITHUB_ACTIONS') == 'true', 'CI only')
    result = {key: os.environ[value] for key, value in (
        ('source', 'GITHUB_SHA'), ('run', 'GITHUB_RUN_ID'), ('attempt', 'GITHUB_RUN_ATTEMPT'),
        ('repository', 'GITHUB_REPOSITORY'), ('repository_id', 'GITHUB_REPOSITORY_ID'))}
    require(result['repository'] == 'gesher-runtime/gesher', 'Wrong repository')
    require(subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == result['source'],
            'Checkout source differs')
    return result


def expected(folder):
    return {'schema': 1, 'context': context(), 'producer': 'native2 / native2',
            'sources': {name: digest(ROOT / name) for name in SOURCES},
            'members': {name: {'role': role, 'sha256': digest(folder / name)}
                        for name, role in ROLES.items()}}


def validate(actual, wanted):
    require(actual == wanted, 'DNS supplier source/run/attempt/compiler/member role/digest mismatch')


def controls(wanted):
    for key in ('context', 'sources', 'members', 'producer', 'schema'):
        mutant = copy.deepcopy(wanted)
        mutant[key] = None
        try:
            validate(mutant, wanted)
        except RuntimeError:
            continue
        raise RuntimeError('DNS manifest mutation accepted: ' + key)
    for kind in ('role', 'sha256'):
        mutant = copy.deepcopy(wanted)
        mutant['members']['x86_64/net_dns_wire'][kind] = 'forged'
        try:
            validate(mutant, wanted)
        except RuntimeError:
            continue
        raise RuntimeError('DNS typed member mutation accepted')
    print('DNS_FULL_MANIFEST_CONTROLS rejected=7')


def command_record(command):
    driver = Path(command[0]).resolve(strict=True)
    return {'command': command, 'driver_sha256': digest(driver),
            'version': subprocess.check_output([str(driver), '--version'], text=True)}


def build(folder):
    context()
    out = folder / 'net-dns'
    out.mkdir()
    records = {'context': context(), 'linux': [], 'aosp': [], 'guests': {}}
    with tempfile.TemporaryDirectory(prefix='dns-full-') as temp:
        exe = Path(temp) / 'reference'
        command = [str(Path(shutil.which('g++')).resolve(strict=True)), '-std=c++20', '-O2',
                   '-Wall', '-Wextra', '-Werror', str(ROOT / SOURCES[0]), '-lresolv', '-o', str(exe)]
        record = command_record(command)
        subprocess.run(command, check=True, timeout=120)
        record['executed_binary_sha256'] = digest(exe)
        actual = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(actual.stdout + actual.stderr, flush=True)
        require(actual.returncode == 0, 'Linux DNS reference failed')
        (out / 'linux-reference.log').write_text(actual.stdout)
        negative = subprocess.run([str(exe), '--negative'], capture_output=True, text=True, timeout=30)
        print(negative.stdout + negative.stderr, flush=True)
        require(negative.returncode == 1 and 'FAIL DNS: negative corruption rejected' in
                negative.stdout + negative.stderr, 'Linux corruption negative accepted')
        records['linux'].append(record)
    records['aosp'] = run_reference(ROOT, out)
    archive = ROOT / 'dns-ndk.zip'
    require(archive.stat().st_size == 663987688, 'Wrong publisher NDK archive size')
    with archive.open('rb') as stream:
        archive_sha1 = hashlib.file_digest(stream, 'sha1').hexdigest()
    require(archive_sha1 == '090e8083a715fdb1a3e402d0763c388abb03fb4e', 'Wrong publisher NDK archive digest')
    with archive.open('rb') as stream:
        archive_sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
    records['archive'] = {'publisher_sha1': archive_sha1, 'size': archive.stat().st_size,
                          'sha256': archive_sha256}
    # Never trust the pre-existing extracted cache. Authenticate the archive
    # on every use, then compile only from a fresh extraction of those bytes.
    with tempfile.TemporaryDirectory(prefix='dns-full-authenticated-') as temporary:
        subprocess.run(['unzip', '-q', str(archive), '-d', temporary], check=True, timeout=180)
        build_guests(folder, records, Path(temporary) / 'android-ndk-r27c')
    (out / 'compilers.json').write_text(json.dumps(records, sort_keys=True) + '\n')
    (out / 'manifest.json').write_text(json.dumps(expected(folder), sort_keys=True) + '\n')


def build_guests(folder, records, ndk):
    props = (ndk / 'source.properties').read_text()
    require('27.2.12479018' in props, 'Wrong actual NDK revision')
    records['ndk_properties'] = props
    binpath = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    for abi, triple in (('x86_64', 'x86_64-linux-android'), ('arm64-v8a', 'aarch64-linux-android')):
        guest = folder / abi / 'net_dns_wire'
        command = [str(binpath / (triple + '29-clang++')), '-std=c++20', '-O2',
                   '-Wall', '-Wextra', '-Werror', '-static-libstdc++', str(ROOT / SOURCES[0]), '-o', str(guest)]
        record = command_record(command)
        record['clang_sha256'] = digest((binpath / 'clang++').resolve(strict=True))
        subprocess.run(command, check=True, timeout=120)
        stub = ndk / 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib' / triple / '29/libc.so'
        record['private_abi'] = {}
        for role, binary, pattern in (('stub', stub, r'\b__dn_skipname@@LIBC\b'),
                                      ('guest', guest, r'\b__dn_skipname@LIBC\b')):
            symbols = subprocess.check_output([str(binpath / 'llvm-readelf'), '--dyn-syms', '--wide', str(binary)], text=True)
            matches = [row for row in symbols.splitlines() if re.search(pattern, row)]
            require(len(matches) == 1 and (' UND ' in matches[0]) == (role == 'guest'), 'Wrong private DNS ABI')
            record['private_abi'][role] = {'sha256': digest(binary), 'symbol': matches[0]}
            print('DNS_FULL_PRIVATE_ABI', abi, role, matches[0], flush=True)
        records['guests'][abi] = record


def verify(folder):
    out = folder / 'net-dns'
    entries = list(out.iterdir())
    require(all(p.is_file() and not p.is_symlink() for p in entries) and
            {p.name for p in entries} == set(ASSETS) | {'manifest.json'}, 'Unexpected DNS asset members')
    wanted = expected(folder)
    controls(wanted)
    validate(read(out / 'manifest.json'), wanted)
    records = read(out / 'compilers.json')
    require(records['context'] == context(), 'Compiler context differs')
    require(records['archive']['publisher_sha1'] == '090e8083a715fdb1a3e402d0763c388abb03fb4e' and
            records['archive']['size'] == 663987688 and len(records['archive']['sha256']) == 64,
            'Compiler publisher archive identity differs')
    require(set(records['guests']) == {'x86_64', 'arm64-v8a'} and len(records['linux']) == 1 and
            len(records['aosp']) == 2, 'Missing compiler roles')
    for abi, machine, model in (('x86_64', 62, 'x86_64'), ('arm64-v8a', 183, 'arm64')):
        elf = (folder / abi / 'net_dns_wire').read_bytes()
        require(elf[:6] == b'\x7fELF\x02\x01' and int.from_bytes(elf[18:20], 'little') == machine, 'Wrong DNS ELF role')
        compiler = records['guests'][abi]
        require({'-Wall', '-Wextra', '-Werror', '-static-libstdc++'} <= set(compiler['command']) and
                'clang version' in compiler['version'] and len(compiler['clang_sha256']) == 64 and
                compiler['private_abi']['guest']['sha256'] == digest(folder / abi / 'net_dns_wire'), 'Guest compiler role differs')
        require((out / ('aosp-reference-' + model + '.log')).read_bytes() ==
                (out / 'linux-reference.log').read_bytes(), 'Common independent references differ')
        require((out / ('aosp-extended-' + model + '.log')).read_text().splitlines()[-1] ==
                'DNS all done rows=174 failures=0', 'Missing AOSP extended oracle')
    rows = (out / 'linux-reference.log').read_text().splitlines()
    require(len(rows) == 97 and rows[-1] == 'DNS all done rows=96 failures=0', 'Missing Linux reference rows')
    print('DNS_FULL_VERIFIED', json.dumps(wanted, sort_keys=True), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('build', 'verify'))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.mode == 'build':
        build(args.directory.resolve())
    verify(args.directory.resolve())
