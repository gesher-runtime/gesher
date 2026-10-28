"""CI-only current-production fixture/reference roles for the canonical full."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/web3'))
import host_roles

# Qualified leaf bytes; only CMake fixture isolation and accepted bionic Chdir differ.
PINNED_SOURCE = {'.github/workflows/aot3-after-resource-component.yml': '27aa2b609cb52e934796075548690a42d338e344',
 '.github/workflows/framework.yml': 'db9dc36524cd27a9a67c85460d2e98b1cbb1d44b',
 '.github/workflows/native3-locks-commands.yml': '87095a77445bd19f39bed8fc8586dbcbc0e86172',
 'CMakeLists.txt': '8ba137e29a5840600fe7d77fee4e833e6d0bfd73',
 'compat/bionic/posix_bionic.cc': '1c3d40509c6f1a2adf560aa0ce077bd563e29b03',
 'compat/posix/include/fcntl.h': 'ab3425b5226fcb4346aae9b62ff9347da9113c4f',
 'compat/posix/posix.cmake': 'bd7a73bd589c30617b9a4ee29b4d15bba3f20221',
 'compat/posix/src/fd.cc': '1ea26b0d2ae99938f3acca41f4d1e2f8df2b1b37',
 'compat/posix/src/fsops.cc': '47d68e9ff2dba40e65ed3c72b41aace41687189b',
 'compat/posix/src/internal.h': 'f3f3079b5d50f30098950108a96367b798388d66',
 'compat/posix/src/path.cc': '84b2bad0a97cd4bbb6dd5ae420b9a1a118fb88c6',
 'compat/posix/src/record_lock_descriptors.cc': 'e94030db83d9e49ff48a1696926b93f35b5063b9',
 'compat/posix/src/record_lock_descriptors.h': '22a04c5c769fa1bceef93aa1ae0f9977ad22d3de',
 'compat/posix/src/record_lock_duplicate.cc': '7925d9b852d3ebccd067287a44bd9ff70171e462',
 'compat/posix/src/record_lock_duplicate.h': '7bad2c8e0be45ede34e10cc21259d673797c60a7',
 'compat/posix/src/record_lock_liveness.cc': 'f4f978f0f36eb049073003c21846442cc0074f7a',
 'compat/posix/src/record_lock_liveness.h': '33dffb3fb0236e3bb72d072f0547e76f80de9782',
 'compat/posix/src/record_lock_operation.cc': 'bf7cdb703f394ae04295b7321cc5aefe8c629e64',
 'compat/posix/src/record_lock_operation.h': '2b7556da0cca6f3a6eb57dbd87674401b6ae26d8',
 'compat/posix/src/record_lock_ranges.h': '77df2e020e6778e1f602f49a16f5d37fbddac499',
 'compat/posix/src/record_lock_registry.cc': 'aaa91c010c276cc74a58eca96f44dc5f08ab1925',
 'compat/posix/src/record_lock_registry.h': '86e81b23b6573073cc95e037f32380826763961b',
 'compat/posix/src/system.cc': 'b02024fc823b1c8db4469a4ba877edb3a47499d7',
 'compat/posix/src/unistd.cc': 'd7766c2183ada697a48efc6d4656c80378605925',
 'patches/art/0730-aot3-validate-after-shared-library-context.patch': '9a44a9d0e25edcb84c21f61ec7a12f8ad5d9670b',
 'patches/libcore/0730-aot3-complete-after-library-hierarchy-before-dex-open.patch': 'a5020001ad8b525bee34a3bc3bd373f4cb329179',
 'tests/apps/sharedlibraryapp/build.sh': 'd3f0640f5825ccab74759fb6a633b3466ade3bc2',
 'tests/dex-graphics/gesher/Aot3SharedLibraryTest.java': 'ba534155d23864c791e0ca2f264cded734e39f2a',
 'tests/native3-locks/command_oracle.py': 'e24cb4853975fd15f23eb4ebe613ec11688ab837',
 'tests/native3-locks/command_worker.cc': 'cd06726ee15548292786f15636f06b8b2732cc5e',
 'tests/native3-locks/crt_lock_probe.h': 'dd6260081e62b24fd376a5fa055c104621e5bce7',
 'tests/native3-locks/duplicate_oracle.py': 'accd7a325f3e27e62522ec9fe94113420ab93fc3',
 'tests/native3-locks/duplicate_windows_test.cc': '3bbce47e182d274abe9417a0066aaad26ac90af3',
 'tests/native3-locks/stack_probe.h': 'b950dbdcc8e0df636904b6be526e35c3173fe143',
 'tests/native3-locks/validate_stdio_evidence.py': 'c0381b740ed2e2c291959a52b28dff5155db4c45',
 'tests/shared-libraries/after-res/values/config.xml': '8063ec46cfddf8644ab5f16d68f584a611cfff26',
 'tests/windows/run-aot3-resource-component.ps1': '65531b9d848c35e98308c60429e68be303d345d4',
 'tests/windows/tracks/73-aot3-shared-libraries.ps1': 'c5c3a03b083d0ff3e63cfca4f70ea47303166ecc',
 'tools/aot3/host_payload.py': '7a8cda9d658cfec5e06e0a53cb16a5b47b8d774c',
 'tools/aot3/preflight.py': '3a434b353a6d3b347ee65c58662b529c24891ad9',
 'tools/build-framework-res.sh': '38dbf329a0340c23cd26b2cfcb13fd1c90afc162',
 'tools/native3-locks/artifact/assemble-artifacts.py': '8d01e5084c8b0aac7f06bc7eae75cf14e431f5b2',
 'tools/native3-locks/artifact/download-verified-artifact.py': 'cd5cc415e53102c664edac22b1604df639c8804b',
 'tools/native3-locks/artifact/test-verified-artifact.py': 'af7324581ae011f39aacf87d81ae1f44ad4c85f6'}

TARGETS = ('native3_lock_command_worker', 'native3_lock_duplicate_test', 'binderd')
HOST_FILES = {name + suffix for name in TARGETS for suffix in ('.exe', '.imports.txt')}
REFERENCE_FILES = {'linux-reference.json'}
ALLOWED_IMPORTS = {
    'kernel32.dll', 'advapi32.dll', 'shell32.dll', 'ole32.dll', 'ntdll.dll',
    'ws2_32.dll', 'iphlpapi.dll', 'psapi.dll', 'bcrypt.dll',
    'api-ms-win-core-synch-l1-2-0.dll',
    *('api-ms-win-crt-' + part + '-l1-1-0.dll' for part in (
        'stdio', 'runtime', 'locale', 'heap', 'private', 'string', 'convert',
        'math', 'environment', 'filesystem', 'time', 'process', 'utility')),
}


def source_inventory():
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    result = {}
    for name in filter(None, names):
        path = ROOT / name
        assert path.is_file() and not path.is_symlink(), ('Missing/linked tracked source', name)
        result[name] = host_roles.sha(path)
    assert 'tests/native3-locks/command_worker.cc' in result
    return result


def flat_inventory(root, expected):
    assert root.is_dir() and not root.is_symlink()
    entries = list(root.iterdir())
    assert all(path.is_file() and not path.is_symlink() for path in entries), 'Non-file fixture member'
    assert {path.name for path in entries} == expected, ('Whole fixture members differ', root)
    return {path.name: host_roles.sha(path) for path in entries}


def write_manifest(root, metadata):
    path = root / 'provenance.json'
    assert not path.exists()
    path.write_text(json.dumps(metadata, sort_keys=True) + '\n', encoding='utf-8')


def check_manifest(root, role, expected, context, sources):
    actual = json.loads((root / 'provenance.json').read_text(encoding='utf-8'))
    assert actual['context'] == context and actual['sources'] == sources and actual['role'] == role
    files = flat_inventory(root, expected | {'provenance.json'})
    del files['provenance.json']
    assert files == actual['outputs'], 'Actual fixture bytes differ from producer manifest'
    return actual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('preflight', 'reference', 'host', 'controls', 'verify'))
    parser.add_argument('--host', type=Path, default=Path('dist-native3-locks'))
    parser.add_argument('--reference', type=Path, default=Path('native3-reference'))
    parser.add_argument('--destination', type=Path, default=Path('command-fixture'))
    args = parser.parse_args()
    context = host_roles.context()
    sources = source_inventory()
    if args.mode == 'preflight':
        base = 'dab3196a'
        subprocess.run(['git', 'merge-base', '--is-ancestor', base, 'HEAD'], cwd=ROOT, check=True)
        allowed = {'.github/workflows/ci.yml', 'tools/native3-locks/union_payload.py', '.github/workflows/tsf-format-component.yml', '.github/workflows/services-geocoder-worker-component.yml', '.github/workflows/native3-locks-blocking.yml', 'tools/native3-locks/union_blocking_guard.py', 'tools/windows4/run-tsf-union.ps1', '.github/workflows/coherent-union-trusted-source.yml'}
        changed = set(subprocess.check_output(['git', 'diff', '--name-only', base, 'HEAD', '--', '.',
                      ':(exclude)docs/**'], cwd=ROOT, text=True).splitlines())
        assert changed <= allowed and changed, ('Unreviewed coherent production delta', changed - allowed)
        for name, expected in PINNED_SOURCE.items():
            actual = subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip()
            assert actual == expected, ('Selected source differs from reviewed leaf/adaptation', name, actual)
        print(f'COHERENT_NATIVE_AOT_SOURCE base={base} source={context["source"]} pinned_blobs={len(PINNED_SOURCE)} non_doc_paths={len(changed)} unexpected_delta=0', flush=True)
    elif args.mode == 'reference':
        assert not args.reference.exists(), 'Reference destination must be absent'
        args.reference.mkdir()
        driver = Path(shutil.which('g++')).resolve(strict=True)
        executable = args.reference / 'linux-command-worker'
        command = [str(driver), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                   'tests/native3-locks/command_worker.cc', '-o', str(executable)]
        version = subprocess.check_output([str(driver), '--version'], text=True)
        subprocess.run(command, check=True, timeout=120)
        try:
            subprocess.run([sys.executable, 'tests/native3-locks/command_oracle.py', '--worker',
                            str(executable.resolve()), '--output', str(args.reference / 'linux-reference.json')],
                           check=True, timeout=120)
        finally:
            executable.unlink()
        outputs = flat_inventory(args.reference, REFERENCE_FILES)
        write_manifest(args.reference, dict(context=context, sources=sources, role='native3-linux-reference',
            outputs=outputs, compiler=dict(path=str(driver), sha256=host_roles.sha(driver), version=version),
            command=command))
        print('ACTUAL_NATIVE3_REFERENCE ' + json.dumps(dict(context=context, outputs=outputs,
              compiler_sha256=host_roles.sha(driver), compiler_version=version), sort_keys=True), flush=True)
    elif args.mode == 'host':
        import re
        outputs = flat_inventory(args.host, HOST_FILES)
        production = json.loads((ROOT / 'dist/web3-production-role.json').read_text())
        compiler = host_roles.compiler(ROOT / 'out')
        assert all(production.get(key) == value for key, value in context.items())
        assert production['role'] == 'production' and production['test_hook_compiled'] is False
        assert production['compiler'] == compiler
        assert production['executable_sha256'] == host_roles.sha(ROOT / 'dist/dalvikvm.exe')
        assert host_roles.cache(ROOT / 'out')['GESHER_WEB3_QUERY_TESTS'] == 'OFF'
        compile_commands = {}
        for name in TARGETS[:2]:
            relative = 'tests/native3-locks/' + ('command_worker.cc' if name.endswith('command_worker') else 'duplicate_windows_test.cc')
            rows = [argv for key, argv in host_roles.commands(ROOT / 'out').items()
                    if Path(key[1]).resolve() == (ROOT / relative).resolve()]
            assert len(rows) == 1 and '-Werror' in rows[0], ('Missing actual fixture compile command', relative)
            argv = rows[0]
            if argv[0] != compiler['configured_driver']:
                launcher = shutil.which(argv[0])
                assert len(argv) > 1 and argv[1] == compiler['configured_driver'] and launcher and compiler['launcher']
                assert str(Path(launcher).resolve()) == compiler['launcher'], 'Fixture compiler launcher differs'
            compile_commands[relative] = argv
        for name in TARGETS:
            imports = re.findall(r'^\s*Name:\s*(\S+)', (args.host / (name + '.imports.txt')).read_text(), re.MULTILINE)
            assert imports and not ({value.casefold() for value in imports} - ALLOWED_IMPORTS), (name, imports)
        for name in TARGETS[:2]:
            assert not (ROOT / 'out' / (name + '.exe')).exists()
            assert not (ROOT / 'dist' / (name + '.exe')).exists(), 'Native fixture leaked into production'
        write_manifest(args.host, dict(context=context, sources=sources, role='native3-common-host-fixture',
            outputs=outputs, compiler=compiler, production_role=production, compile_commands=compile_commands))
        print('ACTUAL_NATIVE3_CURRENT_HOST ' + json.dumps(dict(context=context, outputs=outputs,
              compiler=compiler, production_runtime_sha256=production['executable_sha256']), sort_keys=True), flush=True)
    else:
        host = check_manifest(args.host, 'native3-common-host-fixture', HOST_FILES, context, sources)
        reference = check_manifest(args.reference, 'native3-linux-reference', REFERENCE_FILES, context, sources)
        production = host['production_role']
        assert all(production.get(key) == value for key, value in context.items())
        assert production['role'] == 'production' and production['test_hook_compiled'] is False
        assert production['compiler'] == host['compiler'] and production['shared_sources'] == host_roles.sources()
        assert host['compiler']['configured_driver'] and host['compiler']['sha256'] and host['compiler']['metadata_sha256']
        assert reference['compiler']['sha256'] and reference['compiler']['version'] and reference['command']
        if args.mode == 'controls':
            assert args.host.resolve().is_relative_to(ROOT), 'Controls require an owned checkout payload'
            manifest_path = args.host / 'provenance.json'
            original = manifest_path.read_bytes()
            held = args.host.parent / ('.held-native3-' + uuid.uuid4().hex)
            tool = args.host / 'native3_lock_command_worker.exe'
            tool.rename(held)
            try:
                try:
                    check_manifest(args.host, 'native3-common-host-fixture', HOST_FILES, context, sources)
                except AssertionError:
                    pass
                else:
                    raise RuntimeError('Missing actual native worker accepted')
            finally:
                held.rename(tool)
            extra = args.host / ('.extra-native3-' + uuid.uuid4().hex)
            with extra.open('xb') as stream:
                stream.write(b'extra')
            try:
                try:
                    check_manifest(args.host, 'native3-common-host-fixture', HOST_FILES, context, sources)
                except AssertionError:
                    pass
                else:
                    raise RuntimeError('Extra actual fixture member accepted')
            finally:
                extra.unlink()
            for field in ('source', 'role'):
                wrong = json.loads(original)
                if field == 'source':
                    wrong['context']['source'] = '0' * 40
                else:
                    wrong['role'] = 'production'
                try:
                    manifest_path.write_text(json.dumps(wrong), encoding='utf-8')
                    try:
                        check_manifest(args.host, 'native3-common-host-fixture', HOST_FILES, context, sources)
                    except AssertionError:
                        pass
                    else:
                        raise RuntimeError('Wrong actual fixture ' + field + ' accepted')
                finally:
                    manifest_path.write_bytes(original)
            assert manifest_path.read_bytes() == original
            check_manifest(args.host, 'native3-common-host-fixture', HOST_FILES, context, sources)
            print('ACTUAL_NATIVE3_PAYLOAD_NEGATIVES missing/extra/source/role rejected; exact original restored', flush=True)
            return
        assert not args.destination.exists(), 'Refuse to reuse a fixture assembly'
        args.destination.mkdir()
        for root, metadata in ((args.host, host), (args.reference, reference)):
            for name, expected in metadata['outputs'].items():
                target = args.destination / name
                assert not target.exists(), 'Unexpected fixture collision'
                shutil.copyfile(root / name, target)
                assert host_roles.sha(target) == expected
                print(f'ACTUAL_COMMAND_OUTPUT name={name} sha256={expected} source={context["source"]} run={context["run"]} attempt={context["attempt"]}', flush=True)
        flat_inventory(args.destination, HOST_FILES | REFERENCE_FILES)
        print('VERIFIED_NATIVE3_CURRENT_SUPPLIERS ' + json.dumps(dict(context=context,
              compiler=host['compiler'], production_runtime_sha256=production['executable_sha256'],
              output_members=7), sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
