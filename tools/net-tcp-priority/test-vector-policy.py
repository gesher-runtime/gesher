#!/usr/bin/env python3
"""Verify reviewed vector/fcntl repairs and TCP diagnostics preserve source bounds."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('selected_policy', Path(__file__).with_name('test-selected-policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
source_path = ROOT / 'compat/posix/src/system.cc'
original = source_path.read_bytes()
read_bytes = Path.read_bytes


def rejected(label, replacement, repin=False, target=source_path, pin_name='SYSTEM_SHA256'):
    assert replacement != read_bytes(target), 'negative control made no change'
    def mutated(path):
        return replacement if path == target else read_bytes(path)
    with patch.object(Path, 'read_bytes', mutated), contextlib.redirect_stdout(io.StringIO()):
        saved = getattr(policy, pin_name)
        try:
            if repin:
                setattr(policy, pin_name, hashlib.sha256(replacement).hexdigest())
            try:
                policy.check()
            except AssertionError as error:
                # The exact OFD transition also checks whole protected function
                # bodies; its assertion identifies both the path and marker.
                detail = error.args[0] if error.args else None
                path = detail[0] if isinstance(detail, tuple) else detail
                assert path == target.relative_to(ROOT).as_posix(), error
                print_result = label
            except subprocess.CalledProcessError as error:
                # A mutation inside the pinned transition may be rejected by
                # reverse application before the baseline hash comparison. This
                # counts only when that exact check rejects the mutated path.
                assert error.cmd == ['git', 'apply', '--reverse', '--unidiff-zero', '--check', '-'], error
                assert error.returncode == 1, error
                assert target.relative_to(ROOT).as_posix().encode() in error.stderr, error.stderr
                print_result = label
            else:
                raise RuntimeError('source policy accepted ' + label)
        finally:
            setattr(policy, pin_name, saved)
    print('PASS source policy rejects ' + print_result)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()
rejected('unrelated readv edit even with a new full-file hash',
         original.replace(b'ssize_t readv(', b'// unrelated readv change\nssize_t readv(', 1), True)
rejected('socket gather edit even with a new full-file hash',
         original.replace(b'bool SocketIo(', b'// unrelated socket change\nbool SocketIo(', 1), True)
rejected('ambiguous writev boundary', original + b'\n// ssize_t writev(\n', True)
rejected('unreviewed bytes inside writev',
         original.replace(b'  if (int error = gesher::EventObjectWriteError',
                          b'  // unreviewed change\n  if (int error = gesher::EventObjectWriteError', 1))
rejected('fcntl getter operand consumption even with a new full-file hash',
         original.replace(b'  if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||\n',
                          b'  if (cmd == F_GETFD || cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||\n', 1), True)
rejected('fcntl integer operand ABI change even with a new full-file hash',
         original.replace(b'    arg = va_arg(args, int);\n',
                          b'    arg = va_arg(args, long);\n', 1), True)
rejected('fcntl lock-pointer ABI change even with a new full-file hash',
         original.replace(b'    struct flock* argument = va_arg(pointers, struct flock*);\n',
                          b'    struct flock* argument = reinterpret_cast<struct flock*>(va_arg(pointers, long));\n', 1), True)
rejected('fcntl socket descriptor flags edit even with a new full-file hash',
         original.replace(b'        return FD_CLOEXEC;  // sockets are never inherited (winsock.cc)\n',
                          b'        return 0;  // sockets are never inherited (winsock.cc)\n', 1), True)
rejected('duplicate reviewed fcntl preamble even with a new full-file hash',
         original + b'\n' + policy.FCNTL_REPLACEMENTS[1][1], True)
print('vector/fcntl source policy: positive source and nine negative controls passed')

fd_path = ROOT / 'compat/posix/src/fd.cc'
fd_source = fd_path.read_bytes()
stream_retirement = policy.FD_STREAM_DEVICE_RETIREMENT
fd_controls = (
    ('unrelated descriptor bytes',
     fd_source + b'\n// unrelated descriptor change\n'),
    ('stream retirement condition changed',
     fd_source.replace(stream_retirement,
                       stream_retirement.replace(b'closed_or_replaced ||',
                                                 b'closed_or_replaced &&', 1), 1)),
    ('stream retirement errno preservation removed',
     fd_source.replace(stream_retirement,
                       stream_retirement.replace(b'  errno = saved;\n',
                                                 b'  errno = 0;\n', 1), 1)),
    ('stream retirement helper missing',
     fd_source.replace(stream_retirement, b'', 1)),
    ('stream retirement helper duplicated',
     fd_source + stream_retirement),
    ('stream retirement helper moved',
     fd_source.replace(policy.FD_DEVICE_LOOKUP + stream_retirement,
                       stream_retirement + policy.FD_DEVICE_LOOKUP, 1)),
    ('existing device lookup changed',
     fd_source.replace(policy.FD_DEVICE_LOOKUP,
                       policy.FD_DEVICE_LOOKUP.replace(b'return LookupDevice(fd, device);',
                                                       b'return false;', 1), 1)),
)
for label, replacement in fd_controls:
    rejected(label + ' even with a new full-file hash', replacement, True,
             target=fd_path, pin_name='FD_SHA256')
print('stream-device source policy: positive source and seven negative controls passed')

fixture_path = ROOT / 'tests/net-tcp-priority/contract.cc'
fixture = fixture_path.read_bytes()
rejected('unrelated TCP fixture bytes',
         fixture + b'\n// unrelated fixture change\n', target=fixture_path)
rejected('TCP fixture assertion relaxation',
         fixture.replace(b'require(pthread_join(thread, nullptr) == 0, "sender joined");',
                         b'require(true, "sender joined");', 1), target=fixture_path)
rejected('TCP delayed-arrival deadline change',
         fixture.replace(b'ready(a.receiver, true, 2000, "delayed-arrival");',
                         b'ready(a.receiver, true, 2500, "delayed-arrival");', 1), target=fixture_path)
rejected('TCP diagnostic errno reset',
         fixture.replace(b'  const int errno_before = errno;\n',
                         b'  errno = 0;\n  const int errno_before = errno;\n', 1), target=fixture_path)
rejected('duplicate reviewed TCP diagnostic anchor',
         fixture + policy.TCP_DIAGNOSTIC_REPLACEMENTS[2][1], target=fixture_path)
without_diagnostics = fixture
for before, after in policy.TCP_DIAGNOSTIC_REPLACEMENTS:
    assert without_diagnostics.count(after) == 1
    without_diagnostics = without_diagnostics.replace(after, before, 1)
rejected('TCP fixture missing required diagnostics', without_diagnostics, target=fixture_path)
print('TCP diagnostic source policy: six negative controls passed')

rejected('thread census equality relaxation',
         fixture.replace(b'actual.threads == baseline.threads',
                         b'actual.threads >= baseline.threads', 1), target=fixture_path)
rejected('thread census observer forced on successful modes',
         fixture.replace(b'ObserveOwnerThreads(mode, actual.handles != baseline.handles || actual.threads != baseline.threads)',
                         b'ObserveOwnerThreads(mode, true)', 1), target=fixture_path)
thread_diagnostic = ROOT / policy.OWNER_THREAD_DIAGNOSTIC_PATH
thread_source = thread_diagnostic.read_bytes()
rejected('thread observer querying a reused foreign-process TID',
         thread_source.replace(b'if (owner == GetCurrentProcessId()) {', b'if (true) {', 1),
         target=thread_diagnostic)
rejected('thread observer leaking its diagnostic handles',
         thread_source.replace(b'if (CloseHandle(thread)) ++closed;', b'if (true) ++closed;', 1),
         target=thread_diagnostic)
print('thread diagnostic source policy: four negative controls passed')

receipt_path = ROOT / 'tools/ci/receipt-diagnostics.ps1'
receipt = receipt_path.read_bytes()
receipt_controls = (
    ('unrelated receipt diagnostic bytes', receipt + b'\n# unrelated change\n'),
    ('receipt source changed to checkout',
     receipt.replace(b"$Expected['source'] = $SourceIdentity.api_head_sha",
                     b"$Expected['source'] = $SourceIdentity.checkout_sha", 1)),
    ('missing receipt source parameter',
     receipt.replace(policy.RECEIPT_SOURCE_PARAMETER,
                     b'[Collections.IDictionary] $Expected = $null)', 1)),
    ('duplicate receipt source block', receipt + policy.RECEIPT_SOURCE_BLOCK),
)
for label, replacement in receipt_controls:
    rejected(label + ' even with a new full-file hash', replacement, True,
             target=receipt_path, pin_name='RECEIPT_DIAGNOSTICS_SHA256')
print('receipt diagnostic source policy: four negative controls passed')
