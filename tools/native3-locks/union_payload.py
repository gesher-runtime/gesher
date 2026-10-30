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

# Exact additional leaves: Exec full40796808833 / UDP full40839298355.
SELECTED_COMBINED_SOURCE = {'.github/workflows/native2.yml': '847f1f8908a50e55392f9d62c12f15365fe680a0',
 '.github/workflows/net-udp-component.yml': 'ad3021ac7986ffa1643a3bf54817f7ef6bb868c0',
 '.github/workflows/net-udp-fault-observer.yml': '76fab381d903730d0f639034b75f52ec781a29ff',
 'compat/posix/src/socket.cc': '57b79cc74424af7cbbf8b0cf96cd255981d5618a',
 'compat/posix/src/socket_internal.h': 'faa128a88a340d0c71fc5d0ff3c84c5a7faef1a6',
 'compat/posix/src/socket_user_copy.h': '961aa5a41bb9e3576ab83cd4d305cf80d3637e08',
 'compat/posix/src/winsock.cc': '825e0b8e1274ccafed1b406e6b3545e2eebb5a6a',
 'tests/dex-services/gesher/services/BinderTest.java': '8bbf62583204c5d74ec7295506054885ce0d55ca',
 'tests/native2/net_udp_copy_observer.c': 'b7dbba2aa81db0ec435ae990447509b7f8aef18e',
 'tests/native2/net_udp_fault_observer.c': '2d63237ca4d2891e84c4c1a21c16448bbdfee2eb',
 'tests/native2/net_udp_guarded_copy_probe.cpp': '35b571e3f253950f5f0a96117d9458ee4f1b6e39',
 'tests/native2/net_udp_trunc.cpp': '4d2d46badfa7a0b90a1f134537ee4a90f72a2e17',
 'tests/windows/tracks/50-services.ps1': 'acaed0ca6823d1b88ebf8aabde0c8403ce57eb8b',
 'tests/windows/tracks/89-net-udp-trunc.ps1': '9823b72c58bb77918bbb57a40d50664d1001105b',
 'tools/net-udp-component/payload.py': 'db22caed0012086fa90b257b3ef74e963ab2df87',
 'tools/net-udp-component/source.json': '92b0538e27f9e39aba040e30bfb362400c18724a',
 'tools/net-udp/payload.py': 'dbfcf5ed59cc2cbbc726109c1ce8bb18f4e0f356'}
PINNED_SOURCE.update(SELECTED_COMBINED_SOURCE)

# Reviewed DNS merge adaptations; prior leaf assertions remain above.
MERGED_DNS_SOURCE = {'.github/workflows/native2.yml': '1d0876fdedf3d64ea006e9840d03c03bea8ab25d',
 '.github/workflows/net-dns-component.yml': '2342778354242fe2c187cc66de331e5c1ce10377',
 'compat/arm64/dll/arm64_smoke_test.cc': '46e2a606e4be23d1b444f9961fcd03aa6a144074',
 'compat/arm64/dll/guest_libc.cc': 'c3ef2087a0fefe691831ef2bd902e14b929a06f4',
 'compat/bionic/bionic.cmake': '7d5c64c1c3b19ed9eb727d49959840c9611be6d4',
 'compat/bionic/dns_wire.cc': '5a4ff2f750d378049bbbb4c1b8b57f03bc79a4e8',
 'compat/bionic/shim.cc': 'd2423662fa0256fc50b58ec2201083f2c849a0d6',
 'compat/bionic/shim_internal.h': 'f93d7008fbc55c202c59027e385072b38838a15b',
 'tests/native2/dns-reference/ns_name.c': 'c2dc80f59cbc1fd37d7d08e5464310579a1c4a2f',
 'tests/native2/net_dns_wire.cpp': '4973793ed58dce9f753c24f76e18621e3663751b',
 'tests/windows/tracks/92-net-dns-wire.ps1': '7d6438e111524b4b1761b9c53f144c7bea5ca78f',
 'tools/net-dns-component/aosp_reference.py': '7c968478f06cbedc7c2bef650eec4a7ed7e589c6',
 'tools/net-dns-component/payload.py': '8bf17e1a54fc903d635edb32ccb0763aba9eba67',
 'tools/net-dns-component/source.json': '51563a7370546df21b5e89c2b7263e1ac3bc44c2',
 'tools/net-dns/payload.py': 'a49f0de4e33fb51470bdc0c85b3e2624e4208172',
 'tools/net-dns/payload_shared.py': 'd8241f906b4ec288f2059780b9db68a58f99f6b3',
 'tools/net-udp/payload.py': '64491dc7109d00859a47abd164bf661f24f5bdbf'}
PINNED_SOURCE.update(MERGED_DNS_SOURCE)
PINNED_SOURCE['tests/windows/tracks/58-devices.ps1'] = 'b16da67c5a2286f73cda9a9162295a931cabd40e'

# Exact observer repair qualified by component 409072; historical baseline remains frozen.
PINNED_SOURCE.update({'tests/native3-locks/command_oracle.py': 'c0655cce6466619b35c3e68eb82bc20ca939fc03', 'tests/native3-locks/command_worker.cc': '6872f626e1c64671d9b0eda8f4dcc55f89dec69a', 'tests/native3-locks/stdio_sink_oracle.py': 'cc6ced799c5391c76eac312054cf242e3313bd50', 'tests/native3-locks/test_command_cleanup.py': 'fbd6815890d8dcee22136d0298e7684986b49845', 'tools/native3-locks/StdioSinkSupervisor.cs': '1b320bc00a64172733f2e87a9ee864975337a0b0'})

# TSF/Services source qualified by 408805 and 408811; runner-console trust is bounded.
PINNED_SOURCE.update({'.github/workflows/services-geocoder-worker-component.yml': 'a96e2b1aee0b45a809dfb25ca705eb13859d1c40', '.github/workflows/tsf-format-component.yml': 'a3b2021a2df42316a46d4849eb499fb5b1842a7e', 'tools/windows4/TsfJobSupervisor.cs': '625be4d5906bb48dd02d6103382fd2e85ac88738', 'tools/windows4/TsfJobWorker.cs': 'e4392ea9ee4e8faf6d2f4b28424f8e05fe692ce1', 'tools/windows4/diagnose-pwsh-job.ps1': '6956c0d1714f17761dc756c17f72cb8d119fe306', 'tools/windows4/test-tsf-job-supervisor.ps1': 'e290479e7b81fbd8f9d8705687c84fb084b789e9'})

MEDIA4_PINS = {'tests/media/ndk-muxer/muxer_test.cc': '66c58b32f1d51ffd70a0bbed99986063682ac095', 'tests/media/ndk-muxer/verify_output.cc': '5575a7573fc08bf9f9a98929bf4e7b0f6bcaf423', 'tests/media/ndk-muxer/verify-webm-cues.py': 'ad24652a7c34be72d4869775a52afb778afd16e1', 'tests/windows/tracks/79-media4-muxer.ps1': '9f82ca9f402a782bb62228de08c06a790be66cd2'}
PINNED_SOURCE.update(MEDIA4_PINS)
FONTS_PINS = {'.github/workflows/native2.yml': 'ed994230b37385578efcf051296da915b609d031', 'tests/native2/fonts_standalone.c': '784fb1abb5a06839784cc48bee3fb2d317e2b2dc', 'tests/windows/tracks/46-native3.ps1': '5227ae87be03aa93c79c25a7d7ffbb1b8ca5a877', 'compat/bionic/extra_bionic.cc': '25a6824aaa1f83a3f50943637e1b39de9f54231a', 'compat/bionic/jni.cmake': '0068b5f75ceb3f508cf2c749c67384fc15a4fda8', 'compat/bionic/native_fonts.cc': '3a191e07371cdf05b31c86985228f6152e9e86df', 'compat/posix/src/path.cc': '8857487491fb45ff21bb467a3b2ffc32f28f63c0', 'compat/posix/include/gesher/font_paths.h': '2f200999a4c11d7b624195478c58770638402537', 'tools/fonts/verify-standalone-link.py': '675e154ce89cebed8a053ed5c424ec2fdc16348f', 'tools/fonts/test-standalone-link.py': '4f34f1195f2df7bf8806a2e55d66071ecd911670'}
PINNED_SOURCE.update(FONTS_PINS)

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
        base = '8b1e9b509ceef7e532a7ff5bc085ecdbc476d292'
        subprocess.run(['git', 'merge-base', '--is-ancestor', base, 'HEAD'], cwd=ROOT, check=True)
        allowed = {'.github/workflows/ci.yml', 'tools/native3-locks/union_payload.py', '.github/workflows/tsf-format-component.yml', '.github/workflows/services-geocoder-worker-component.yml', '.github/workflows/native3-locks-blocking.yml', 'tools/native3-locks/union_blocking_guard.py', 'tools/windows4/run-tsf-union.ps1', 'tools/windows4/TsfJobSupervisor.cs', 'tools/windows4/TsfJobWorker.cs', 'tools/windows4/diagnose-pwsh-job.ps1', 'tools/windows4/test-tsf-job-supervisor.ps1', '.github/workflows/coherent-union-trusted-source.yml', '.github/workflows/combined-full-trusted-source.yml', '.github/workflows/native2.yml', '.github/workflows/net-udp-component.yml', '.github/workflows/net-udp-fault-observer.yml', 'compat/posix/src/socket.cc', 'compat/posix/src/socket_internal.h', 'compat/posix/src/socket_user_copy.h', 'compat/posix/src/winsock.cc', 'tests/dex-services/gesher/services/BinderTest.java', 'tests/native2/net_udp_copy_observer.c', 'tests/native2/net_udp_fault_observer.c', 'tests/native2/net_udp_guarded_copy_probe.cpp', 'tests/native2/net_udp_trunc.cpp', 'tests/windows/tracks/50-services.ps1', 'tests/windows/tracks/58-devices.ps1', 'tests/windows/tracks/89-net-udp-trunc.ps1', 'tools/net-udp-component/payload.py', 'tools/net-udp-component/source.json', 'tools/net-udp/payload.py'}
        allowed.update(['tests/native3-locks/command_oracle.py', 'tests/native3-locks/command_worker.cc', 'tests/native3-locks/stdio_sink_oracle.py', 'tests/native3-locks/test_command_cleanup.py', 'tools/native3-locks/StdioSinkSupervisor.cs'])
        allowed.update(MERGED_DNS_SOURCE)
        allowed.update(MEDIA4_PINS)
        allowed.update(FONTS_PINS)
        allowed.add('.github/workflows/net-combined-dns-trusted-source.yml')
        changed = set(subprocess.check_output(['git', 'diff', '--name-only', base, 'HEAD', '--', '.',
                      ':(exclude)docs/**'], cwd=ROOT, text=True).splitlines())
        assert changed <= allowed and changed, ('Unreviewed coherent production delta', changed - allowed)
        for name, expected in PINNED_SOURCE.items():
            actual = subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip()
            assert actual == expected, ('Selected source differs from reviewed leaf/adaptation', name, actual)
        for name in FONTS_PINS:
            actual = {key: subprocess.check_output(['git', 'hash-object', key], cwd=ROOT, text=True).strip() for key in FONTS_PINS}
            actual[name] = '0' * 40
            assert actual != FONTS_PINS
            print('FONTS_PREFLIGHT_NEGATIVE blob=' + name + ' rejected', flush=True)
        assert not (changed | {'tests/native2/fonts_unreviewed.c'}) <= allowed
        print('FONTS_PREFLIGHT_NEGATIVE unauthorized_path rejected', flush=True)
        # Exercise the same comparisons with a forged blob for each new leaf.
        actual_media = {name: subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip()
                        for name in MEDIA4_PINS}
        assert actual_media == MEDIA4_PINS
        for name in MEDIA4_PINS:
            mutant = dict(actual_media)
            mutant[name] = '0' * 40
            assert mutant != MEDIA4_PINS, ('Mutation was not rejected', name)
            print('MEDIA4_PREFLIGHT_NEGATIVE blob=' + name + ' rejected', flush=True)
        assert not (changed | {'tests/media/ndk-muxer/unreviewed.py'}) <= allowed
        print('MEDIA4_PREFLIGHT_NEGATIVE unauthorized_path rejected', flush=True)
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
