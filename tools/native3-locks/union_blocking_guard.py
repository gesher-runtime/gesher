"""Read-only exact-source gate. Run only the reviewed component in future CI."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = 'tools/native3-locks/blocking-source.json'


def git(*arguments):
    return subprocess.check_output(['git', *arguments], cwd=ROOT, text=True).strip()


def main():
    manifest = json.loads((ROOT / MANIFEST).read_text())
    base = manifest['base']
    subprocess.run(['git', 'merge-base', '--is-ancestor', base, 'HEAD'], cwd=ROOT, check=True)
    assert git('status', '--porcelain') == '', 'Source checkpoint must be clean'
    changed = set(git('diff', '--name-only', '8b1e9b509ceef7e532a7ff5bc085ecdbc476d292', 'HEAD').splitlines())
    code = {path for path in changed if not path.startswith('docs/')}
    allowed = {'.github/workflows/ci.yml', 'tools/native3-locks/union_payload.py', '.github/workflows/tsf-format-component.yml', '.github/workflows/services-geocoder-worker-component.yml', '.github/workflows/native3-locks-blocking.yml', 'tools/native3-locks/union_blocking_guard.py', 'tools/windows4/run-tsf-union.ps1', 'tools/windows4/TsfJobSupervisor.cs', 'tools/windows4/TsfJobWorker.cs', 'tools/windows4/diagnose-pwsh-job.ps1', 'tools/windows4/test-tsf-job-supervisor.ps1', '.github/workflows/coherent-union-trusted-source.yml', '.github/workflows/combined-full-trusted-source.yml', '.github/workflows/native2.yml', '.github/workflows/net-udp-component.yml', '.github/workflows/net-udp-fault-observer.yml', 'compat/posix/src/socket.cc', 'compat/posix/src/socket_internal.h', 'compat/posix/src/socket_user_copy.h', 'compat/posix/src/winsock.cc', 'tests/dex-services/gesher/services/BinderTest.java', 'tests/native2/net_udp_copy_observer.c', 'tests/native2/net_udp_fault_observer.c', 'tests/native2/net_udp_guarded_copy_probe.cpp', 'tests/native2/net_udp_trunc.cpp', 'tests/windows/tracks/50-services.ps1', 'tests/windows/tracks/58-devices.ps1', 'tests/windows/tracks/89-net-udp-trunc.ps1', 'tools/net-udp-component/payload.py', 'tools/net-udp-component/source.json', 'tools/net-udp/payload.py', '.github/workflows/net-dns-component.yml', 'compat/arm64/dll/arm64_smoke_test.cc', 'compat/arm64/dll/guest_libc.cc', 'compat/bionic/bionic.cmake', 'compat/bionic/dns_wire.cc', 'compat/bionic/shim.cc', 'compat/bionic/shim_internal.h', 'tests/native2/dns-reference/ns_name.c', 'tests/native2/net_dns_wire.cpp', 'tests/windows/tracks/92-net-dns-wire.ps1', 'tools/net-dns-component/aosp_reference.py', 'tools/net-dns-component/payload.py', 'tools/net-dns-component/source.json', 'tools/net-dns/payload.py', 'tools/net-dns/payload_shared.py', '.github/workflows/net-combined-dns-trusted-source.yml'} | set(['tests/media/ndk-muxer/muxer_test.cc', 'tests/media/ndk-muxer/verify_output.cc', 'tests/media/ndk-muxer/verify-webm-cues.py', 'tests/windows/tracks/79-media4-muxer.ps1']) | set(['tests/native3-locks/command_oracle.py', 'tests/native3-locks/command_worker.cc', 'tests/native3-locks/stdio_sink_oracle.py', 'tests/native3-locks/test_command_cleanup.py', 'tools/native3-locks/StdioSinkSupervisor.cs'])
    allowed.update(['.github/workflows/native2.yml', 'tests/native2/fonts_standalone.c', 'tests/windows/tracks/46-native3.ps1', 'compat/bionic/extra_bionic.cc', 'compat/bionic/jni.cmake', 'compat/bionic/native_fonts.cc', 'compat/posix/src/path.cc', 'compat/posix/include/gesher/font_paths.h', 'tools/fonts/verify-standalone-link.py', 'tools/fonts/test-standalone-link.py'])
    assert not (code | {'tests/native2/fonts_unreviewed.c'}) <= allowed
    print('BLOCKING_FONTS_NEGATIVE unauthorized_path rejected', flush=True)
    assert code <= allowed, ('Unexpected union automation delta', code)
    media_names = {'tests/media/ndk-muxer/muxer_test.cc', 'tests/media/ndk-muxer/verify_output.cc',
                   'tests/media/ndk-muxer/verify-webm-cues.py', 'tests/windows/tracks/79-media4-muxer.ps1'}
    assert media_names <= allowed and not (code | {'tests/media/ndk-muxer/unreviewed.py'}) <= allowed
    print('BLOCKING_MEDIA4_NEGATIVE unauthorized_path rejected; blob pins enforced by external guard', flush=True)
    for path, expected in manifest['blobs'].items():
        if path != '.github/workflows/native3-locks-blocking.yml':
            assert git('hash-object', path) == expected, ('Unreviewed source bytes', path)
    # No qualified tests, signals, descriptor/stdio ownership, Exec, Binder,
    # bionic, AOT or accepted full suppliers are allowed through this guard.
    print(f'BLOCKING_SOURCE base={base} source={git("rev-parse", "HEAD")} '
          f'pinned_blobs={len(manifest["blobs"])} public_blocking_closed=false', flush=True)


if __name__ == '__main__':
    main()
