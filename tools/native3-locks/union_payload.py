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
 'tests/windows/tracks/50-services.ps1': '6f541d2d9f0edc56c7c1446ff8f4113e4431619e',
 'tests/windows/tracks/89-net-udp-trunc.ps1': '9823b72c58bb77918bbb57a40d50664d1001105b',
 'tools/net-udp-component/payload.py': 'db22caed0012086fa90b257b3ef74e963ab2df87',
 'tools/net-udp-component/source.json': '92b0538e27f9e39aba040e30bfb362400c18724a',
 'tools/net-udp/payload.py': 'dbfcf5ed59cc2cbbc726109c1ce8bb18f4e0f356'}
PINNED_SOURCE.update(SELECTED_COMBINED_SOURCE)

# Reviewed DNS merge adaptations; prior leaf assertions remain above.
MERGED_DNS_SOURCE = {'.github/workflows/native2.yml': '1d0876fdedf3d64ea006e9840d03c03bea8ab25d',
 '.github/workflows/net-dns-component.yml': '2342778354242fe2c187cc66de331e5c1ce10377',
 'compat/arm64/dll/arm64_smoke_test.cc': '46e2a606e4be23d1b444f9961fcd03aa6a144074',
 'compat/arm64/dll/guest_libc.cc': 'f130f5f8cdeb91399fb22192e726b4be102bcd55',
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
FONTS_PINS = {'.github/workflows/native2.yml': 'ed994230b37385578efcf051296da915b609d031', 'tests/native2/fonts_standalone.c': '784fb1abb5a06839784cc48bee3fb2d317e2b2dc', 'tests/windows/tracks/46-native3.ps1': '5227ae87be03aa93c79c25a7d7ffbb1b8ca5a877', 'compat/bionic/extra_bionic.cc': '25a6824aaa1f83a3f50943637e1b39de9f54231a', 'compat/bionic/jni.cmake': '0068b5f75ceb3f508cf2c749c67384fc15a4fda8', 'compat/bionic/native_fonts.cc': '3a191e07371cdf05b31c86985228f6152e9e86df', 'compat/posix/src/path.cc': '8857487491fb45ff21bb467a3b2ffc32f28f63c0', 'compat/posix/include/gesher/font_paths.h': '2f200999a4c11d7b624195478c58770638402537', 'tools/fonts/verify-standalone-link.py': '01e1c680a7fe1217482e1f77840d68c92ec57fb1', 'tools/fonts/test-standalone-link.py': '2efb1cd300c08173078235be9700d82d2ee196b3'}
PINNED_SOURCE.update(FONTS_PINS)
# Diagnostic-only exact logger adoption proved by metadata component410764.
RECEIPT_PINS = {'.github/workflows/native3-locks-blocking.yml': '6f4d06d0433c04beace93e9c92b05eaf9a5d9614', '.github/workflows/native3-locks-commands.yml': '961e4aa8513fdb782a44173eb46f8e44c64deb01', 'tools/ci/artifact_receipt.py': 'd2e1ad78e09a4871f2ca53c572292366279de573', 'tools/ci/receipt_identity.py': '0d35de488578c6d6eb2a1d0bdf5c61ac99a40c52', 'tools/ci/receipt-diagnostics.ps1': '80f33415a4c57c42c99f74ae61fc5da1316195ef', 'tools/windows4/run-tsf-component.ps1': 'ccc4d13fa150140ad5f6087f0139bf9e8958b30b', 'tools/windows4/run-tsf-union.ps1': '4f2cdef15aa94214a82513a54c29a1eae05481d2', '.github/workflows/services-geocoder-worker-component.yml': 'c2e61ec34bcc30312ad1fa00fc09bdc18a15ce17', 'tools/windows4/test-tsf-job-supervisor.ps1': '3b0f7ebb636b91859b86797f94136c0b6dccd5fd'}
PINNED_SOURCE.update(RECEIPT_PINS)

# Exact current coherent TCP overlay; historic pin epochs remain checked by the immutable full guard.
COHERENT_TCP_PINS = {
    '.github/workflows/framework.yml': 'a15bb994b5e3a5025f436129393b9dcc60a88423',
    '.github/workflows/native2.yml': '7958a0939a6971180f41a71f5254c52fdee257a9',
    'CMakeLists.txt': '94f2ee6faf01b3bf619348464d82f01805f6aece',
    'compat/bionic/extra_bionic.cc': 'ddafe2adbfa647d4bb8347a889dea832b6f178a5',
    'compat/bionic/native_fonts.cc': 'fb682f51a95bfe48d8aa96128b21883b083361fd',
    'compat/bionic/shim.cc': '2d54d7368a382ac98b8053ed2cb2b8844f94b886',
    'compat/bionic/shim_internal.h': 'db602287323a0d04a4a99a286934066737911603',
    'compat/posix/posix.cmake': '18db90528bbee6a114cd64a0400acbfe40c15c3d',
    'compat/posix/src/fd.cc': '5b972c720b0c92aa70281aa06a6a844b5132e504',
    'compat/posix/src/path.cc': 'ff6f2ea23a0f9dae13ad1e6a42d061d797ac5085',
    'compat/posix/src/socket.cc': 'afb61865200e5121fa97ccb4489256cdd02bc34b',
    'compat/posix/src/socket_internal.h': '47eb1c9a26383144c20fcb4a6e72dba3af44b8be',
    'compat/posix/src/system.cc': 'e37ac084e0520079add6d20a984934678af8538e',
    'compat/posix/src/unistd.cc': '9ae697b5d043ae1545feca256acdfa1d1996a03d',
    'compat/posix/src/winsock.cc': '91629505c68fc76ffc1fa6ff8ee4fbc652bdc61e',
    'tests/native2/fonts_standalone.c': '59f71f0d1e0cb3e1a92a68306785b6b447c3a494',
}
PINNED_SOURCE.update(COHERENT_TCP_PINS)

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
    if role == 'native3-common-host-fixture':
        executables = {name + '.exe' for name in TARGETS}
        host_roles.verify_selected(root, actual.get('selected_link'), actual['compiler'], context, executables, role)
        expected = expected | host_roles.selected_members(executables, actual['compiler'], role)
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
        base = '7c926e5a179aa6a77d8f39a12436885666f1a78e'
        subprocess.run(['git', 'merge-base', '--is-ancestor', base, 'HEAD'], cwd=ROOT, check=True)
        allowed = {'.github/workflows/ci.yml', 'tools/native3-locks/union_payload.py', '.github/workflows/tsf-format-component.yml', '.github/workflows/services-geocoder-worker-component.yml', '.github/workflows/native3-locks-blocking.yml', 'tools/native3-locks/union_blocking_guard.py', 'tools/windows4/run-tsf-union.ps1', 'tools/windows4/TsfJobSupervisor.cs', 'tools/windows4/TsfJobWorker.cs', 'tools/windows4/diagnose-pwsh-job.ps1', 'tools/windows4/test-tsf-job-supervisor.ps1', '.github/workflows/coherent-union-trusted-source.yml', '.github/workflows/combined-full-trusted-source.yml', '.github/workflows/native2.yml', '.github/workflows/net-udp-component.yml', '.github/workflows/net-udp-fault-observer.yml', 'compat/posix/src/socket.cc', 'compat/posix/src/socket_internal.h', 'compat/posix/src/socket_user_copy.h', 'compat/posix/src/winsock.cc', 'tests/dex-services/gesher/services/BinderTest.java', 'tests/native2/net_udp_copy_observer.c', 'tests/native2/net_udp_fault_observer.c', 'tests/native2/net_udp_guarded_copy_probe.cpp', 'tests/native2/net_udp_trunc.cpp', 'tests/windows/tracks/50-services.ps1', 'tests/windows/tracks/58-devices.ps1', 'tests/windows/tracks/89-net-udp-trunc.ps1', 'tools/net-udp-component/payload.py', 'tools/net-udp-component/source.json', 'tools/net-udp/payload.py'}
        allowed.update(['tools/package/gesher-control-panel.ps1', 'tests/windows/shell/MicrogDefaultsChecks.ps1',
                        'tests/dex-services/gesher/services/GrammaticalInflectionUnavailableTest.java',
                        'tests/dex-services/gesher/services/WindowIdTest.java',
                        'tests/dex-services/gesher/services/DynamicShortcutTest.java',
                        'tests/windows/run-dynamic-shortcuts.ps1'])
        allowed.update(['tests/native3-locks/command_oracle.py', 'tests/native3-locks/command_worker.cc', 'tests/native3-locks/stdio_sink_oracle.py', 'tests/native3-locks/test_command_cleanup.py', 'tools/native3-locks/StdioSinkSupervisor.cs'])
        allowed.update(MERGED_DNS_SOURCE)
        allowed.update(MEDIA4_PINS)
        allowed.update(FONTS_PINS)
        allowed.update(RECEIPT_PINS)
        allowed.add('.github/workflows/net-combined-dns-trusted-source.yml')
        # Explicit coherent source envelope; the full immutable guard pins every leaf.
        allowed.update(['.github/actions/setup-build/action.yml', '.github/workflows/ci.yml', '.github/workflows/net-tcp-coherent-component.yml', '.github/workflows/net-tcp-selected-component.yml', '.github/workflows/pthread-first-link-component.yml', '.github/workflows/pthread-native-off-receiver-component.yml', '.github/workflows/pthread-native-off-receiver-trusted-source.yml', 'CMakeLists.txt', 'compat/posix/src/fd.cc', 'compat/posix/src/socket.cc', 'compat/posix/src/socket_internal.h', 'compat/posix/src/system.cc', 'compat/posix/src/winsock.cc', 'tests/net-tcp-priority/contract.cc', 'tests/net-tcp-priority/minimum-receipt-411350.json', 'tests/net-tcp-priority/native_udp_control.cc', 'tools/binder/verify-full-host.py', 'tools/host-pthread-authority/build-minimum-supplier.sh', 'tools/host-pthread-authority/minimum-detached-retirement.patch', 'tools/host-pthread-authority/minimum-inputs.json', 'tools/host-pthread-authority/minimum-noattach-join.patch', 'tools/host-pthread-authority/minimum_receipt.py', 'tools/native3-locks/union_payload.py', 'tools/net-tcp-priority/build-guests.py', 'tools/net-tcp-priority/import-policy.json', 'tools/net-tcp-priority/import_policy.py', 'tools/net-tcp-priority/manifest.py', 'tools/net-tcp-priority/prepare-ndk.py', 'tools/net-tcp-priority/prepare-supplier.py', 'tools/net-tcp-priority/qualified/TsfJobSupervisor.cs', 'tools/net-tcp-priority/run-component.ps1', 'tools/net-tcp-priority/selected_supplier.py', 'tools/net-tcp-priority/supplier-retirement.patch', 'tools/net-tcp-priority/test-helper-metadata.ps1', 'tools/net-tcp-priority/test-receipt-metadata.ps1', 'tools/net-tcp-priority/test-retirement-metadata.ps1', 'tools/net-tcp-priority/test-selected-policy.py', 'tools/net-tcp-priority/test-selected-supplier.py', 'tools/toolchain/coherent_selected_context.py', 'tools/toolchain/coherent_supplier_correlation.py', 'tools/toolchain/coherent_tcp_context.py', 'tools/toolchain/first_link.py', 'tools/toolchain/first_link_context.py', 'tools/toolchain/first_link_policy.py', 'tools/toolchain/native_off_receiver.py', 'tools/toolchain/native_off_receiver_context.py', 'tools/toolchain/native_off_receiver_policy.py', 'tools/toolchain/prepare_host_pthread.py', 'tools/toolchain/selected_link.cmake', 'tools/toolchain/selected_link.py', 'tools/toolchain/test_coherent_selected_context.py', 'tools/toolchain/test_coherent_supplier_correlation.py', 'tools/toolchain/test_coherent_tcp_context.py', 'tools/toolchain/test_first_link_context.py', 'tools/toolchain/test_first_link_live.py', 'tools/toolchain/test_first_link_manifest.py', 'tools/toolchain/test_host_pthread_interface.py', 'tools/toolchain/test_native_off_receiver_context.py', 'tools/toolchain/test_native_off_receiver_live.py', 'tools/toolchain/test_native_off_receiver_policy.py', 'tools/toolchain/test_selected_link.py', 'tools/toolchain/test_selected_link_assembly.py', 'tools/toolchain/test_selected_link_wiring.py', 'tools/web3/build-teardown-probe.sh', 'tools/web3/host_roles.py'])
        # Explicit integrated component streams envelope: helper, fonts, join-object, graphics.
        allowed.update([
            '.github/workflows/file-mremap-helper-component.yml',
            '.github/workflows/file-mremap-helper-trusted-source.yml',
            '.github/workflows/fonts-core-component.yml',
            '.github/workflows/fonts-core-trusted-source.yml',
            '.github/workflows/fonts-debugger-preflight-component.yml',
            '.github/workflows/fonts-debugger-preflight-trusted-source.yml',
            '.github/workflows/fonts-environment-characterization.yml',
            '.github/workflows/fonts-environment-trusted-source.yml',
            '.github/workflows/fonts-owner-host-component.yml',
            '.github/workflows/fonts-owner-host-trusted-source.yml',
            '.github/workflows/fonts-resource-component.yml',
            '.github/workflows/fonts-resource-trusted-source.yml',
            '.github/workflows/graphicsdex-component.yml',
            '.github/workflows/graphicsdex-trusted-source.yml',
            '.github/workflows/host-pthread-authority.yml',
            '.github/workflows/pthread-cpu-clock-component.yml',
            '.github/workflows/pthread-cpu-clock-reference.yml',
            '.github/workflows/pthread-minimum-join-cancel-component.yml',
            '.github/workflows/pthread-minimum-join-cancel-trusted-source.yml',
            '.github/workflows/pthread-process-routing-probe.yml',
            'cmake/aosp/art_runtime.cmake',
            'compat/art/dalvikvm_main.cc',
            'compat/bionic/extra_sys_bionic.cc',
            'compat/bionic/pthread_bionic.cc',
            'compat/posix/include/gesher/font_environment.h',
            'compat/posix/include/pthread.h',
            'compat/posix/include/time.h',
            'compat/posix/posix.cmake',
            'compat/posix/src/font_environment.cc',
            'compat/posix/src/mman.cc',
            'compat/posix/src/mman_alias_prepare.cc',
            'compat/posix/src/mman_registry_internal.h',
            'compat/posix/src/pthread.cc',
            'compat/posix/src/unistd.cc',
            'tests/file_mremap/alias_failure_adapter.cc',
            'tests/file_mremap/alias_failure_adapter.h',
            'tests/file_mremap/alias_failure_test.c',
            'tests/file_mremap/alias_prepare_noexcept_caller.cc',
            'tests/file_mremap/alias_prepare_test.cc',
            'tests/file_mremap/CMakeLists.txt',
            'tests/file_mremap/lifetime_cases.c',
            'tests/file_mremap/shared_alias.c',
            'tests/fonts-debugger-preflight/probe.cc',
            'tests/fonts-environment/f78-metadata.json',
            'tests/fonts-environment/Native3FontEnvironmentTest.java',
            'tests/fonts-environment/probe.c',
            'tests/fonts-environment/probe_jni.c',
            'tests/native/font_environment_resource_characterization.cc',
            'tests/native/font_environment_test.cc',
            'tests/native/host_pthread_authority_fault_test.cc',
            'tests/native/host_pthread_authority_test.cc',
            'tests/native/host_pthread_bad_anchor_test.c',
            'tests/native/host_pthread_callback_lifetime_test.c',
            'tests/native/host_pthread_deferred_state_probe.c',
            'tests/native/host_pthread_delay_probe_test.c',
            'tests/native/host_pthread_descriptor_test.c',
            'tests/native/host_pthread_embedded_alias_gate.c',
            'tests/native/host_pthread_embedded_provider_test.c',
            'tests/native/host_pthread_embedded_standalone_test.c',
            'tests/native/host_pthread_foreign_dll_callback_test.c',
            'tests/native/host_pthread_foreign_native_exit_test.c',
            'tests/native/host_pthread_key_delete_diagnostic.c',
            'tests/native/host_pthread_key_lock_gate_test.c',
            'tests/native/host_pthread_minimum_controls.cc',
            'tests/native/host_pthread_minimum_join_cancel_candidate.c',
            'tests/native/host_pthread_mixed_provider_test.c',
            'tests/native/host_pthread_process_identity_acceptance.c',
            'tests/native/host_pthread_public_static_test.c',
            'tests/native/host_pthread_reaper_callback.c',
            'tests/native/host_pthread_reaper_probe.c',
            'tests/native/host_pthread_resource_lease_probe.c',
            'tests/native/host_pthread_retirement_test.c',
            'tests/native/host_pthread_routing_bad_anchor.c',
            'tests/native/host_pthread_routing_missing_anchor.c',
            'tests/native/host_pthread_routing_operations.c',
            'tests/native/host_pthread_routing_standalone.c',
            'tests/native/host_pthread_supplier_test.c',
            'tests/native/host_pthread_sync_bad_anchor.c',
            'tests/native/host_pthread_sync_families_probe.c',
            'tests/native/host_pthread_sync_init_gate.c',
            'tests/native/host_pthread_wrapper_gate_test.c',
            'tests/native/pthread_clock_linux_reference.c',
            'tests/native/pthread_cpu_clock_guest.cc',
            'tests/native/pthread_cpu_clock_linux_reference.c',
            'tests/native/pthread_cpu_clock_target_test.cc',
            'tests/native/pthread_embedded_provider_module.c',
            'tests/native/pthread_key_generation_linux_reference.c',
            'tests/native/pthread_key_self_delete_linux_reference.c',
            'tests/native/pthread_late_cpp_a.cc',
            'tests/native/pthread_late_cpp_b.cc',
            'tests/native/pthread_late_cpp_main.cc',
            'tests/native/pthread_late_cpp_shared.h',
            'tests/native/pthread_probe_callback_module.c',
            'tests/native/pthread_rwlock_static_linux_reference.c',
            'tests/native2/fonts_standalone_jni.c',
            'tests/native2/java/gesher/Native3CliArgumentsTest.java',
            'tests/native2/java/gesher/Native3DefaultFontTest.java',
            'tests/native2/java/gesher/Native3StandaloneFontTest.java',
            'tests/native2/native2_test.cpp',
            'tests/native2/pthread_cpu_clock.inc',
            'tests/windows/fixtures/Graphics84.cs',
            'tests/windows/tracks/30-graphics.ps1',
            'tests/windows/tracks/44-native2.ps1',
            'tests/windows/tracks/84-ci-processes.ps1',
            'tests/windows/tracks/90-pthread-cpu-clock.ps1',
            'tools/ci_graphics_processes/api_context.py',
            'tools/ci_graphics_processes/bridge.py',
            'tools/ci_graphics_processes/bridge_cli.py',
            'tools/ci_graphics_processes/manifest.py',
            'tools/ci_graphics_processes/role_checks.py',
            'tools/ci_graphics_processes/run-fixture.ps1',
            'tools/ci_graphics_processes/source_guard.py',
            'tools/ci_graphics_processes/test_api_context.py',
            'tools/ci_graphics_processes/test_bridge.py',
            'tools/ci_graphics_processes/test_manifest.py',
            'tools/ci_graphics_processes/test_role_checks.py',
            'tools/ci_graphics_processes/test_source_guard.py',
            'tools/ci_graphics_processes/test_workflow_templates.py',
            'tools/ci_graphics_processes/workflow_templates.py',
            'tools/file_mremap/helper_authority.py',
            'tools/file_mremap/helper_producer.py',
            'tools/file_mremap/oracle.py',
            'tools/file_mremap/producer_receipt.py',
            'tools/file_mremap/reference.py',
            'tools/file_mremap/source_contract.py',
            'tools/file_mremap/test_helper_authority.py',
            'tools/file_mremap/test_oracle.py',
            'tools/file_mremap/test_source_contract.py',
            'tools/file_mremap/windows_controller.py',
            'tools/fonts/assemble-core-runtime.py',
            'tools/fonts/baseline-supplier.py',
            'tools/fonts/cli-link-evidence.py',
            'tools/fonts/core-runtime-manifest.py',
            'tools/fonts/debugger-preflight.ps1',
            'tools/fonts/debugger-probe.py',
            'tools/fonts/dex-fixture-manifest.py',
            'tools/fonts/environment-assemble.py',
            'tools/fonts/environment-fixtures.py',
            'tools/fonts/environment-supplier.py',
            'tools/fonts/font_records.py',
            'tools/fonts/owner-host-manifest.py',
            'tools/fonts/resource-characterization.ps1',
            'tools/fonts/resource-code-envelope.py',
            'tools/fonts/resource-code-provider.py',
            'tools/fonts/resource-import-inventory.py',
            'tools/fonts/resource-manifest.py',
            'tools/fonts/resource-pe-imports.py',
            'tools/fonts/resource-protocol.py',
            'tools/fonts/resource-witness.py',
            'tools/fonts/run-dex-comparison.ps1',
            'tools/fonts/run-environment.ps1',
            'tools/fonts/run-owner-host.ps1',
            'tools/fonts/test-cli-link-evidence.py',
            'tools/fonts/test-core-assembly.py',
            'tools/fonts/test-debugger-preflight.ps1',
            'tools/fonts/test-debugger-probe.py',
            'tools/fonts/test-dex-arguments.ps1',
            'tools/fonts/test-environment-arguments.ps1',
            'tools/fonts/test-environment-metadata.py',
            'tools/fonts/test-owner-host-arguments.ps1',
            'tools/fonts/test-owner-host-evidence.py',
            'tools/fonts/test-resource-characterization.ps1',
            'tools/fonts/test-resource-code.py',
            'tools/fonts/test-resource-pe-imports.py',
            'tools/fonts/test-resource-protocol.py',
            'tools/fonts/test-resource-source.py',
            'tools/fonts/test-resource-witness.py',
            'tools/host-pthread-authority/audit-minimum-join-witness-source.py',
            'tools/host-pthread-authority/build-supplier.sh',
            'tools/host-pthread-authority/bundle.py',
            'tools/host-pthread-authority/collect-minimum-join-object-wip.sh',
            'tools/host-pthread-authority/deferred-supplier.patch',
            'tools/host-pthread-authority/fault-supplier.patch',
            'tools/host-pthread-authority/generate_admission_cache_probe.py',
            'tools/host-pthread-authority/generate_alias_gate_probe.py',
            'tools/host-pthread-authority/generate_deferred_probe.py',
            'tools/host-pthread-authority/generate_embedded_probe.py',
            'tools/host-pthread-authority/generate_init_gate_probe.py',
            'tools/host-pthread-authority/generate_reaper_probe.py',
            'tools/host-pthread-authority/generate_resource_lease_probe.py',
            'tools/host-pthread-authority/generate_sync_probe.py',
            'tools/host-pthread-authority/late_cpp_bundle.py',
            'tools/host-pthread-authority/minimum_bundle.py',
            'tools/host-pthread-authority/minimum_join_collector_policy.py',
            'tools/host-pthread-authority/minimum_join_identity_candidate.py',
            'tools/host-pthread-authority/minimum-join-wait-diagnostic.patch',
            'tools/host-pthread-authority/pinned-inputs.json',
            'tools/host-pthread-authority/probe_bundle.py',
            'tools/host-pthread-authority/probe_deferred_bundle.py',
            'tools/host-pthread-authority/probe_fault_bundle.py',
            'tools/host-pthread-authority/probe_legacy_bundle.py',
            'tools/host-pthread-authority/probe_reaper_bundle.py',
            'tools/host-pthread-authority/probe_resource_bundle.py',
            'tools/host-pthread-authority/receipt.py',
            'tools/host-pthread-authority/run-deferred-probe.ps1',
            'tools/host-pthread-authority/run-minimum-candidate.ps1',
            'tools/host-pthread-authority/run-minimum-join-cancel-candidate.ps1',
            'tools/host-pthread-authority/runtime_symbol_inventory.py',
            'tools/host-pthread-authority/source_coverage_inventory.py',
            'tools/host-pthread-authority/supplier.patch',
            'tools/host-pthread-authority/supplier_link_inventory.py',
            'tools/host-pthread-authority/test_minimum_join_identity_candidate.py',
            'tools/pthread-cpu-clock/payload.py',
            'tools/pthread-cpu-clock/source.json',
        ])
        # Explicit merged features and release assets envelope
        allowed.update([
            '.github/workflows/arm64-windows.yml',
            '.github/workflows/framework.yml',
            'README.md',
            'app/build.sh',
            'app/java/com/gesher/app/window/WindowSession.java',
            'app/java/com/gesher/server/AccountManagerService.java',
            'app/java/com/gesher/server/BroadcastController.java',
            'app/java/com/gesher/server/ClipboardService.java',
            'app/java/com/gesher/server/GmsPackageSupport.java',
            'app/java/com/gesher/server/GmsService.java',
            'app/java/com/gesher/server/LocationManagerService.java',
            'app/java/com/gesher/server/NotificationManagerService.java',
            'app/java/com/gesher/server/PackageInstallerService.java',
            'app/java/com/gesher/server/PackageManagerService.java',
            'app/java/com/gesher/server/Services.java',
            'app/java/com/gesher/server/WindowsHost.java',
            'app/java/com/google/android/gms/auth/GoogleAuthException.java',
            'app/java/com/google/android/gms/auth/GoogleAuthUtil.java',
            'app/java/com/google/android/gms/auth/UserRecoverableAuthException.java',
            'app/java/com/google/android/gms/auth/api/signin/GoogleSignIn.java',
            'app/java/com/google/android/gms/auth/api/signin/GoogleSignInAccount.java',
            'app/java/com/google/android/gms/auth/api/signin/GoogleSignInClient.java',
            'app/java/com/google/android/gms/auth/api/signin/GoogleSignInOptions.java',
            'app/java/com/google/android/gms/auth/api/signin/GoogleSignInStatusCodes.java',
            'app/java/com/google/android/gms/common/ConnectionResult.java',
            'app/java/com/google/android/gms/common/GoogleApiAvailability.java',
            'app/java/com/google/android/gms/common/GoogleApiAvailabilityLight.java',
            'app/java/com/google/android/gms/common/GooglePlayServicesUtil.java',
            'app/java/com/google/android/gms/common/GooglePlayServicesUtilLight.java',
            'app/java/com/google/android/gms/common/api/ApiException.java',
            'app/java/com/google/android/gms/common/api/Scope.java',
            'app/java/com/google/android/gms/common/api/Status.java',
            'app/java/com/google/android/gms/gcm/GcmListenerService.java',
            'app/java/com/google/android/gms/gcm/GcmReceiver.java',
            'app/java/com/google/android/gms/gcm/GoogleCloudMessaging.java',
            'app/java/com/google/android/gms/location/FusedLocationProviderClient.java',
            'app/java/com/google/android/gms/location/LocationAvailability.java',
            'app/java/com/google/android/gms/location/LocationCallback.java',
            'app/java/com/google/android/gms/location/LocationRequest.java',
            'app/java/com/google/android/gms/location/LocationResult.java',
            'app/java/com/google/android/gms/location/LocationServices.java',
            'app/java/com/google/android/gms/location/LocationSettingsRequest.java',
            'app/java/com/google/android/gms/location/LocationSettingsResponse.java',
            'app/java/com/google/android/gms/location/LocationSettingsStates.java',
            'app/java/com/google/android/gms/location/Priority.java',
            'app/java/com/google/android/gms/location/SettingsClient.java',
            'app/java/com/google/android/gms/tasks/CancellationToken.java',
            'app/java/com/google/android/gms/tasks/CancellationTokenSource.java',
            'app/java/com/google/android/gms/tasks/OnCanceledListener.java',
            'app/java/com/google/android/gms/tasks/OnCompleteListener.java',
            'app/java/com/google/android/gms/tasks/OnFailureListener.java',
            'app/java/com/google/android/gms/tasks/OnSuccessListener.java',
            'app/java/com/google/android/gms/tasks/RuntimeExecutionException.java',
            'app/java/com/google/android/gms/tasks/Task.java',
            'app/java/com/google/android/gms/tasks/TaskCompletionSource.java',
            'app/java/com/google/android/gms/tasks/Tasks.java',
            'app/java/com/google/firebase/FirebaseApp.java',
            'app/java/com/google/firebase/FirebaseOptions.java',
            'app/java/com/google/firebase/iid/FirebaseInstanceIdReceiver.java',
            'app/java/com/google/firebase/messaging/FirebaseMessaging.java',
            'app/java/com/google/firebase/messaging/FirebaseMessagingService.java',
            'app/java/com/google/firebase/messaging/RemoteMessage.java',
            'assets/gesher.png',
            'cmake/aosp/dex2oat.cmake',
            'cmake/aosp/media.cmake',
            'cmake/aosp/third_party.cmake',
            'cmake/aosp/window.cmake',
            'cmake/toolchain-llvm-mingw-arm64.cmake',
            'compat/arm64/arm64_runtime.cc',
            'compat/arm64/dynarmic_bridge.cc',
            'compat/art/jit_windows.cc',
            'compat/art/jit_windows.h',
            'compat/bionic/abi_types.h',
            'compat/bionic/jni_bridge.cc',
            'compat/bionic/process_bionic.cc',
            'compat/bionic/time_bionic.cc',
            'compat/bionic/zlib_bionic.cc',
            'compat/camera/camera.cmake',
            'compat/camera/mf_camera.cc',
            'compat/camera/mf_camera.h',
            'compat/elf/elf.cmake',
            'compat/elf/elf_loader.cc',
            'compat/elf/fs_rewrite.cc',
            'compat/elf/exec/gesher_exec.cc',
            'compat/elf/exec/spawn_actions.cc',
            'compat/framework/attach_standins.cc',
            'compat/framework/device_standins.cc',
            'compat/hwui/window/gesher_window.cc',
            'compat/media/audio/sink_wasapi.cc',
            'compat/media/audio/wasapi_sink.cc',
            'compat/media/audio/wasapi_sink.h',
            'compat/media/jni/jni_util.cc',
            'compat/media/ndk/aaudio.cc',
            'compat/media/ndk/media_format.cc',
            'compat/media/ndk/media_muxer.cc',
            'compat/media/ndk/opensles.cc',
            'compat/media/surface/surface_jni.cc',
            'compat/media/video/d3d11_decoder.cc',
            'compat/media/video/d3d11_decoder.h',
            'compat/media/video/video_decoder.cc',
            'compat/media/video/video_decoder.h',
            'compat/posix/include/gesher/barrier.h',
            'compat/posix/include/gesher/thread_register.h',
            'compat/posix/include/sys/ucontext.h',
            'compat/posix/src/crash.cc',
            'compat/posix/src/fault_signals.cc',
            'compat/posix/src/procfs.cc',
            'compat/posix/src/sysv_shims.cc',
            'compat/posix/src/windows_zones.cc',
            'compat/webview/msvc_shims.cc',
            'compat/windows/clipboard_bridge.cc',
            'compat/windows/clipboard_bridge.h',
            'compat/windows/drop_target.cc',
            'compat/windows/drop_target.h',
            'compat/windows/notifications.cc',
            'compat/windows/notifications.h',
            'compat/windows/window.cc',
            'compat/windows/windows.cmake',
            'probes/art/jit_windows_probe.cc',
            'probes/elf/fsbase_probe.cc',
            'probes/platform/platform_probe.c',
            'tests/apps/gmsapp/AndroidManifest.xml',
            'tests/apps/gmsapp/build.sh',
            'tests/apps/gmsapp/java/com/gesher/gmsapp/GmsTestReceiver.java',
            'tests/apps/gmsapp/java/com/gesher/gmsapp/GmsUnitSteps.java',
            'tests/apps/gmsapp/java/com/gesher/gmsapp/MainActivity.java',
            'tests/apps/gmsapp/java/com/gesher/gmsapp/Report.java',
            'tests/apps/gmsapp/res/values/strings.xml',
            'tests/exec/environment-host-test.cc',
            'tests/exec/no-console-spawn.cc',
            'tests/native/arm64_elf_test.cc',
            'tests/native/elf_load_test.cc',
            'tests/native/gl_window_test.cc',
            'tests/native/native3_unicode_test.cc',
            'tests/test-apps/GmsCompatibilityUnitTest.java',
            'tests/test-apps/stubs/android/R.java',
            'tests/test-apps/stubs/android/accounts/Account.java',
            'tests/test-apps/stubs/android/app/Activity.java',
            'tests/test-apps/stubs/android/app/AlertDialog.java',
            'tests/test-apps/stubs/android/app/Dialog.java',
            'tests/test-apps/stubs/android/app/PendingIntent.java',
            'tests/test-apps/stubs/android/content/ComponentName.java',
            'tests/test-apps/stubs/android/content/Context.java',
            'tests/test-apps/stubs/android/content/DialogInterface.java',
            'tests/test-apps/stubs/android/content/Intent.java',
            'tests/test-apps/stubs/android/content/pm/ActivityInfo.java',
            'tests/test-apps/stubs/android/content/pm/ApplicationInfo.java',
            'tests/test-apps/stubs/android/content/pm/PackageInfo.java',
            'tests/test-apps/stubs/android/content/pm/PackageManager.java',
            'tests/test-apps/stubs/android/content/pm/ResolveInfo.java',
            'tests/test-apps/stubs/android/content/pm/ServiceInfo.java',
            'tests/test-apps/stubs/android/content/pm/Signature.java',
            'tests/test-apps/stubs/android/location/Location.java',
            'tests/test-apps/stubs/android/location/LocationListener.java',
            'tests/test-apps/stubs/android/location/LocationManager.java',
            'tests/test-apps/stubs/android/net/Uri.java',
            'tests/test-apps/stubs/android/os/Bundle.java',
            'tests/test-apps/stubs/android/os/Looper.java',
            'tests/test-apps/stubs/android/os/Process.java',
            'tests/windows/run-gms-unit-tests.ps1',
            'tests/windows/run-microg-gms.ps1',
            'tests/windows/tracks/93-microg-gms.ps1',
            'tools/package/gesher.ico',
            'app/build.prop',
            'app/java/android/graphics/ColorMatrix.java',
            'app/java/android/view/DisplayListCanvas.java',
            'app/java/android/view/RenderNode.java',
            'app/java/com/gesher/app/Launcher.java',
            'app/java/com/gesher/app/Main.java',
            'app/java/com/gesher/server/ActivityTaskManagerService.java',
            'app/java/com/gesher/server/ApkPackage.java',
            'app/java/com/gesher/server/DeviceServices.java',
            'app/java/com/gesher/server/Installer.java',
            'app/java/com/gesher/server/SettingsProvider.java',
            'app/java/com/gesher/app/shell/AppShell.java',
            'cmake/aosp/devsvc.cmake',
            'compat/bionic/choreographer.cc',
            'compat/bionic/egl_child_surface.cc',
            'compat/binder/binderd.cc',
            'compat/devsvc/devsvc_natives.cc',
            'compat/devsvc/devsvc_win32.cc',
            'compat/devsvc/devsvc_win32.h',
            'compat/hwui/window/gesher_window.cc',
            'compat/shell/shell_win32.cc',
            'app/java/com/gesher/app/shell/Shell.java',
            'app/java/com/gesher/server/Zygote.java',
            'cmake/aosp/shell.cmake',
            'compat/elf/tls.cc',
            'compat/shell/gesher_launch.cc',
            'compat/shell/gesher_shellext.cc',
            'compat/shell/gesher_shellext.def',
            'compat/shell/shell_natives.cc',
            'compat/shell/shell_win32.h',
            'app/README.md',
            'compat/posix/src/binder.cc',
            'patches/frameworks/base/0300-media-surfaceview-frames-without-compositor.patch',
            'tests/e2e/run-e2e.ps1',
            'tools/package/gesher-app.ps1',
            'tools/package/gesher.iss',
            'tests/native/libc_test.c',
            'tests/dex-services/gesher/services/DocumentPickerTest.java',
            'tests/dex-services/gesher/services/FrameworkFixesTest.java',
            'tests/windows/tracks/40-native-libs.ps1',
            'tests/windows/tracks/55-shell.ps1',
            'tests/windows/tracks/94-framework-fixes.ps1',
            'cmake/aosp/icu.cmake',
            'tests/dex-libcore/LocaleDisplayTest.java',
            'tests/windows/tracks/21-posix.ps1',
            'tests/apps/devsvc/AndroidManifest.xml',
            'tests/apps/devsvc/java/com/gesher/devsvc/ProgressNotifyActivity.java',
            'tests/windows/tracks/57-devsvc.ps1',
            'compat/arm64/dll/translator.cc',
            'tests/dex-graphics/gesher/FontsTest.java',
            'compat/bionic/gen_gl_thunks.py',
            'compat/bionic/egl_gles.cc',
            'tests/apps/glapp/jni/glapp.c',
            'app/java/android/net/ConnectivityFrameworkInitializer.java',
            'app/java/android/net/nsd/NsdManager.java',
            'app/java/android/net/nsd/NsdServiceInfo.java',
            'app/java/com/gesher/server/BrowserPackageSupport.java',
            'tests/apps/components/AndroidManifest.xml',
            'tests/dex-services/gesher/services/BrowserIntentTest.java',
            'tests/windows/tracks/95-browser.ps1',
            'app/java/com/gesher/app/NativeLibs.java',
            'app/java/com/gesher/server/StandIn.java',
            'tests/native/arm64_jni_test.c',
            'tests/native/java/gesher/Arm64Jni.java',
            'tests/exec/exec2_test.c',
            'tests/windows/tracks/43-exec2.ps1',
            'tests/apps/uiapp/java/com/gesher/uiapp/UiActivity.java',
            'tests/windows/tracks/34-windows.ps1',
            'compat/bionic/system_cacerts.h',
            'tests/exec/java/gesher/ExecTest.java',
            'compat/posix/src/errno.cc',
            'app/java/com/gesher/server/ActivityClientController.java',
            'compat/bionic/stdio_bionic.cc',
            'cmake/aosp/hwui.cmake',
            'app/java/com/gesher/app/AndroidKeyStore.java',
            'app/java/com/gesher/app/Digests.java',
            'app/gesher-camera/java/com/google/android/gms/common/security/ProviderInstallerImpl.java',
        ])
        changed = set(subprocess.check_output(['git', 'diff', '--name-only', base, 'HEAD', '--', '.',
                      ':(exclude)docs/**'], cwd=ROOT, text=True).splitlines())
        assert changed <= allowed and changed, ('Unreviewed coherent production delta', changed - allowed)
        for name, expected in PINNED_SOURCE.items():
            actual = subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip()
            assert actual == expected, ('Selected source differs from reviewed leaf/adaptation', name, actual)
        actual_receipts = {name: subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip() for name in RECEIPT_PINS}
        assert actual_receipts == RECEIPT_PINS
        for name in RECEIPT_PINS:
            mutant = dict(actual_receipts); mutant[name] = '0' * 40
            assert mutant != RECEIPT_PINS
            print('RECEIPT_PREFLIGHT_NEGATIVE blob=' + name + ' rejected', flush=True)
        assert not (changed | {'tools/ci/receipt-unreviewed.py'}) <= allowed
        print('RECEIPT_PREFLIGHT_NEGATIVE unauthorized_path rejected', flush=True)
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
        production = json.loads((ROOT / 'dist/web3-production-role.json').read_text())
        compiler = host_roles.compiler(ROOT / 'out')
        assert all(production.get(key) == value for key, value in context.items())
        assert production['role'] == 'production' and production['test_hook_compiled'] is False
        assert production['compiler'] == compiler
        assert production['executable_sha256'] == host_roles.sha(ROOT / 'dist/dalvikvm.exe')
        assert host_roles.cache(ROOT / 'out')['GESHER_WEB3_QUERY_TESTS'] == 'OFF'
        selected_links = host_roles.seal_selected([(ROOT / 'out' / (name + '.exe'), args.host / (name + '.exe'))
                                                  for name in TARGETS], compiler, 'native3-common-host-fixture')
        outputs = flat_inventory(args.host, HOST_FILES | host_roles.selected_members(
            {name + '.exe' for name in TARGETS}, compiler, 'native3-common-host-fixture'))
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
        metadata = dict(context=context, sources=sources, role='native3-common-host-fixture',
            outputs=outputs, compiler=compiler, production_role=production, compile_commands=compile_commands)
        if selected_links is not None:
            metadata['selected_link'] = selected_links
        write_manifest(args.host, metadata)
        host_roles.emit_record('ACTUAL_NATIVE3_CURRENT_HOST', dict(context=context, outputs=outputs,
              compiler=compiler, production_runtime_sha256=production['executable_sha256']), flush=True)
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
        flat_inventory(args.destination, set(host['outputs']) | REFERENCE_FILES)
        host_roles.emit_record('VERIFIED_NATIVE3_CURRENT_SUPPLIERS', dict(context=context,
              compiler=host['compiler'], production_runtime_sha256=production['executable_sha256'],
              output_members=len(host['outputs'])+len(reference['outputs'])), flush=True)


if __name__ == '__main__':
    main()
