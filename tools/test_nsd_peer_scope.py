#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Exercise NSD fixture setup at an explicit socket boundary, without network traffic.

These are host checks of the actual helper and peer entry points, not execution
of the Windows provider. The live peer independently requires provider readback.
"""
import argparse
import errno
import importlib
from pathlib import Path
import socket
import struct
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/nsd'))
import peer_scope
import windows_peer
from build_nsd_permission_reference import VARIANTS, validate_badging

PEERS = [(windows_peer, '_gs0123456789ab._tcp')]
if (ROOT / 'tests/nsd/resolver_peer.py').is_file():
    PEERS.append((importlib.import_module('resolver_peer'), '_gr0123456789ab._tcp'))


class SocketBoundary:
    """Only the socket API is replaced; production fixture setup runs unchanged."""
    def __init__(self, kind=socket.SOCK_DGRAM, *, index=0x01020304, fail=None,
                 enabled=None, interfaces=None, events=None):
        self.kind, self.index, self.fail = kind, index, fail
        self.enabled = struct.pack('=I', 1) if enabled is None else enabled
        self.interfaces = struct.pack('=I', index) if interfaces is None else interfaces
        self.events = [] if events is None else events
        self.closed, self.bound = False, None

    def call(self, stage, *details):
        self.events.append((self.kind, stage, *details))
        if self.fail == stage:
            raise OSError('declared socket-boundary failure: ' + stage)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self.closed = True
        self.call('close')

    def setsockopt(self, level, option, value):
        stage = ('enable' if option == 28 else 'add') if level in (socket.IPPROTO_IP,
                socket.IPPROTO_IPV6) and option in (28, 29) else 'option'
        if ((level, option) == (socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP)
                or (level, option) == (socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP)):
            stage = 'join'
        self.call(stage, level, option, value)

    def bind(self, address):
        self.call('bind' if self.kind == socket.SOCK_DGRAM else 'tcp-bind', address)
        self.bound = address

    def listen(self, backlog):
        self.call('listen', backlog)

    def getsockname(self):
        if self.bound is None:
            raise AssertionError('getsockname before bind')
        port = self.bound[1] or (41000 if self.kind == socket.SOCK_DGRAM else 40000)
        return (self.bound[0], port, *self.bound[2:])

    def getsockopt(self, level, option, length):
        if self.bound is None:
            raise AssertionError('receive restriction must be verified after bind')
        self.call('read-enabled' if option == 28 else 'read-list', level, option, length)
        return self.enabled if option == 28 else self.interfaces

    def sendto(self, data, destination):
        self.call('send', bytes(data), destination)
        return len(data)


class ReceiveScopeTest(unittest.TestCase):
    def test_native_dword_and_post_bind_verification_for_both_families(self):
        # Independent ABI expectations: the WinSDK defines options 28/29/33 and
        # documents IF_INDEX as a native DWORD for both protocol levels.
        for family, level, address in ((4, socket.IPPROTO_IP, ('', 5353)),
                                       (6, socket.IPPROTO_IPV6, ('::', 5353))):
            with self.subTest(family=family), mock.patch.object(peer_scope.sys, 'platform', 'win32'):
                sock = SocketBoundary()
                peer_scope.bind_windows_multicast(sock, family, 0x01020304)
                self.assertEqual(sock.events, [
                    (socket.SOCK_DGRAM, 'enable', level, 28, struct.pack('=I', 1)),
                    (socket.SOCK_DGRAM, 'add', level, 29, struct.pack('=I', 0x01020304)),
                    (socket.SOCK_DGRAM, 'bind', address),
                    (socket.SOCK_DGRAM, 'read-enabled', level, 28, 4),
                    (socket.SOCK_DGRAM, 'read-list', level, 33, 1024)])

    def test_invalid_family_and_indexes_do_not_touch_socket(self):
        for family, index in ((0, 1), (5, 1), (4, 0), (6, -1), (4, 1 << 32),
                              (6, True), (4, None), (6, '12')):
            with self.subTest(family=family, index=index), mock.patch.object(peer_scope.sys, 'platform', 'win32'):
                sock = SocketBoundary()
                with self.assertRaises(ValueError):
                    peer_scope.bind_windows_multicast(sock, family, index)
                self.assertEqual(sock.events, [])

    def test_non_windows_live_mode_has_no_unrestricted_fallback(self):
        with mock.patch.object(peer_scope.sys, 'platform', 'linux'):
            sock = SocketBoundary()
            with self.assertRaisesRegex(RuntimeError, 'require Windows'):
                peer_scope.bind_windows_multicast(sock, 4, 1)
            self.assertEqual(sock.events, [])

    def test_disabled_malformed_mismatched_or_extra_readback_fails(self):
        for family in (4, 6):
            for values in ({'enabled': struct.pack('=I', 0)}, {'enabled': b''},
                           {'enabled': b'\x01'}, {'interfaces': b''},
                           {'interfaces': struct.pack('=I', 42)},
                           {'interfaces': struct.pack('=II', 0x01020304, 42)},
                           {'interfaces': struct.pack('=I', 0x01020304) + b'\x00'}):
                with self.subTest(family=family, values=values), mock.patch.object(peer_scope.sys, 'platform', 'win32'):
                    with self.assertRaises(RuntimeError):
                        peer_scope.bind_windows_multicast(SocketBoundary(**values), family, 0x01020304)

    def test_option_bind_and_readback_errors_stop_setup(self):
        for family in (4, 6):
            for stage in ('enable', 'add', 'bind', 'read-enabled', 'read-list'):
                with self.subTest(family=family, stage=stage), mock.patch.object(peer_scope.sys, 'platform', 'win32'):
                    sock = SocketBoundary(fail=stage)
                    with self.assertRaisesRegex(RuntimeError, 'Cannot verify') as caught:
                        peer_scope.bind_windows_multicast(sock, family, 0x01020304)
                    self.assertIsInstance(caught.exception.__cause__, OSError)
                    self.assertEqual(sock.events[-1][1], stage)


class PeerOwnershipTest(unittest.TestCase):
    def invoke(self, module, service_type, family, *, fail=None, enabled=None,
               interfaces=None, loopback=False, platform='win32'):
        created, events = [], []

        def create(address_family, kind):
            self.assertEqual(address_family, socket.AF_INET if family == 4 else socket.AF_INET6)
            if fail == 'construct-udp' and kind == socket.SOCK_DGRAM:
                raise OSError('declared UDP construction failure')
            sock = SocketBoundary(kind, index=42, fail=fail, enabled=enabled,
                                  interfaces=interfaces, events=events)
            created.append(sock)
            return sock

        with tempfile.TemporaryDirectory(prefix='nsd-peer-scope-') as temporary:
            control = Path(temporary)
            (control / 'command').write_text('1 quit')
            args = argparse.Namespace(control=control, type=service_type, instance='Owned fixture',
                    index=0 if loopback else 42, family=family, loopback=loopback,
                    address=('127.0.0.1' if family == 4 else '::1') if loopback else
                            ('192.0.2.1' if family == 4 else '2001:db8::1'))
            with mock.patch.object(module.socket, 'socket', side_effect=create), \
                    mock.patch.object(peer_scope.sys, 'platform', platform), \
                    mock.patch.object(module.select, 'select', side_effect=AssertionError('unexpected receive loop')):
                error = None
                try:
                    module.serve(args)
                except (OSError, RuntimeError) as caught:
                    error = caught
            ready = (control / 'ready').exists()
        self.assertTrue(created)
        self.assertTrue(all(sock.closed for sock in created), 'all created sockets must close')
        self.assertEqual(len(created), 1 if fail == 'construct-udp' else 2)
        return error, ready, events

    def test_real_peer_entry_points_require_verified_scope_before_ready(self):
        for module, service_type in PEERS:
            for family in (4, 6):
                with self.subTest(peer=module.__name__, family=family):
                    error, ready, events = self.invoke(module, service_type, family)
                    self.assertIsNone(error)
                    self.assertTrue(ready)
                    stages = [event[1] for event in events]
                    self.assertLess(stages.index('enable'), stages.index('bind'))
                    self.assertLess(stages.index('bind'), stages.index('read-enabled'))
                    self.assertLess(stages.index('read-list'), stages.index('join'))

    def test_setup_failures_close_tcp_and_udp_without_publishing_ready(self):
        failures = [{'fail': stage} for stage in ('construct-udp', 'tcp-bind', 'listen',
                    'enable', 'add', 'bind', 'read-enabled', 'read-list', 'join')]
        failures += [{'enabled': struct.pack('=I', 0)},
                     {'interfaces': struct.pack('=II', 42, 43)}, {'platform': 'linux'}]
        for module, service_type in PEERS:
            for family in (4, 6):
                for failure in failures:
                    with self.subTest(peer=module.__name__, family=family, failure=failure):
                        error, ready, _ = self.invoke(module, service_type, family, **failure)
                        self.assertIsNotNone(error)
                        self.assertFalse(ready)

    @unittest.skipUnless((ROOT / 'tests/nsd/resolver_peer.py').is_file(),
                         'Resolver peer is introduced by the resolver PR')
    def test_explicit_resolver_loopback_stays_platform_independent(self):
        for module, service_type in PEERS:
            if module.__name__ != 'resolver_peer':
                continue
            for family in (4, 6):
                with self.subTest(family=family):
                    error, ready, events = self.invoke(module, service_type, family,
                                                       loopback=True, platform='linux')
                    self.assertIsNone(error)
                    self.assertTrue(ready)
                    self.assertFalse(any(event[1] in ('enable', 'add', 'read-enabled', 'read-list')
                                         for event in events))


class PeerCommandTest(unittest.TestCase):
    def test_missing_and_real_replaced_command(self):
        with tempfile.TemporaryDirectory(prefix='nsd-command-') as temporary:
            path = Path(temporary) / 'command'
            reader = windows_peer.CommandReader(path)
            self.assertEqual(reader.read(), '')
            replacement = path.with_name('command-new')
            replacement.write_text('1 announce', encoding='utf-8')
            replacement.replace(path)
            self.assertEqual(reader.read(), '1 announce')
            replacement.write_text('2 quit', encoding='utf-8')
            replacement.replace(path)
            self.assertEqual(reader.read(), '2 quit')

    def test_access_error_deadline_is_absolute_and_not_reset_by_missing_or_empty(self):
        reader = windows_peer.CommandReader(Path('command'))
        denied = PermissionError(errno.EACCES, 'controlled replacement sharing denial')
        now = [5.0]
        with mock.patch.object(windows_peer.time, 'monotonic', side_effect=lambda: now[0]), \
                mock.patch.object(Path, 'read_text', side_effect=[denied, FileNotFoundError(), '', denied]) as read:
            self.assertEqual(reader.read(), '')
            for at in (5.2, 5.5, 5.9):
                now[0] = at
                self.assertEqual(reader.read(), '')
            now[0] = 6.0
            with self.assertRaises(PermissionError) as caught:
                reader.read()
            self.assertIs(caught.exception, denied)
            self.assertEqual(read.call_count, 4, 'expired deadline admits no extra read')

    def test_successful_retry_starts_no_deadline_for_the_next_command(self):
        reader = windows_peer.CommandReader(Path('command'))
        denied = PermissionError(errno.EACCES, 'controlled sharing denial')
        now = [5.0]
        with mock.patch.object(windows_peer.time, 'monotonic', side_effect=lambda: now[0]), \
                mock.patch.object(Path, 'read_text', side_effect=[denied, '1 announce', denied, '2 quit']):
            self.assertEqual(reader.read(), '')
            now[0] = 5.5
            self.assertEqual(reader.read(), '1 announce')
            now[0] = 20.0
            self.assertEqual(reader.read(), '')
            now[0] = 20.5
            self.assertEqual(reader.read(), '2 quit')

    def test_unrelated_io_and_decode_errors_are_not_retried(self):
        for error in (OSError(errno.EIO, 'read failed'), IsADirectoryError(),
                      UnicodeDecodeError('utf-8', b'\xff', 0, 1, 'invalid command')):
            with self.subTest(error=type(error).__name__):
                reader = windows_peer.CommandReader(Path('command'))
                with mock.patch.object(Path, 'read_text', side_effect=error) as read:
                    with self.assertRaises(type(error)) as caught:
                        reader.read()
                    self.assertIs(caught.exception, error)
                    self.assertEqual(read.call_count, 1)

    def run_commands(self, *, persistent=False, malformed=None, global_deadline=False):
        created, events, observations = [], [], []
        now, loops, denied = [0.0], [0], [0]
        access_error = PermissionError(errno.EACCES, 'controlled command replacement denial')
        original_read = Path.read_text
        with tempfile.TemporaryDirectory(prefix='nsd-command-loop-') as temporary:
            control = Path(temporary)
            command = control / 'command'
            command.write_text(malformed or '1 announce', encoding='utf-8')
            args = argparse.Namespace(control=control, type='_gs0123456789ab._tcp',
                    instance='Owned fixture', index=42, family=4, address='192.0.2.1')

            def create(family, kind):
                self.assertEqual(family, socket.AF_INET)
                value = SocketBoundary(kind, index=42, events=events)
                created.append(value)
                return value

            def read(path, *args, **kwargs):
                if path == command and (persistent or denied[0]):
                    denied[0] = 0
                    raise access_error
                return original_read(path, *args, **kwargs)

            def select_ready(readers, writers, errors, timeout):
                self.assertEqual((readers, writers, errors, timeout), ([created[1]], [], [], 0.05))
                ack = original_read(control / 'ack') if (control / 'ack').exists() else ''
                observations.append(ack)
                loops[0] += 1
                if persistent:
                    now[0] += 150.0 if global_deadline else 0.4
                elif loops[0] == 1:
                    command.write_text('2 silent', encoding='utf-8')
                    denied[0] = 1
                    now[0] = 0.7
                elif loops[0] == 2:
                    # The denied control read must leave announcement mode live.
                    now[0] = 0.8
                elif loops[0] == 3:
                    command.write_text('3 quit', encoding='utf-8')
                else:
                    self.fail('peer did not honor the real quit command')
                return [], [], []

            error = None
            with mock.patch.object(windows_peer.socket, 'socket', side_effect=create), \
                    mock.patch.object(peer_scope.sys, 'platform', 'win32'), \
                    mock.patch.object(windows_peer.time, 'monotonic', side_effect=lambda: now[0]), \
                    mock.patch.object(windows_peer.select, 'select', side_effect=select_ready), \
                    mock.patch.object(Path, 'read_text', new=read):
                try:
                    windows_peer.serve(args)
                except (OSError, ValueError) as caught:
                    error = caught
            ack = original_read(control / 'ack') if (control / 'ack').exists() else ''
            ready = (control / 'ready').exists()
        self.assertEqual(len(created), 2)
        self.assertTrue(all(value.closed for value in created), 'both owned sockets retire on every exit')
        self.assertTrue(ready)
        return error, ack, observations, events, access_error

    def test_run_peer_keeps_network_loop_and_ack_order_until_real_quit(self):
        error, ack, observations, events, _ = self.run_commands()
        self.assertIsNone(error)
        self.assertEqual(ack, '3')
        self.assertEqual(observations, ['1', '1', '2'], 'denied read must not acknowledge silent')
        sent = [entry for entry in events if entry[1] == 'send']
        self.assertEqual(len(sent), 2, 'existing announcement mode continues during the denied read')
        self.assertEqual(sent[0], sent[1], 'retry sends only the previously requested packet')

    def test_run_peer_persistent_denial_fails_without_ack_and_closes_sockets(self):
        error, ack, observations, events, denied = self.run_commands(persistent=True)
        self.assertIs(error, denied)
        self.assertEqual(ack, '')
        self.assertEqual(observations, ['', '', ''])
        self.assertFalse(any(entry[1] == 'send' for entry in events))

    def test_control_retry_does_not_extend_the_existing_global_deadline(self):
        error, ack, observations, _, _ = self.run_commands(persistent=True, global_deadline=True)
        self.assertIsNone(error)  # Preserve the peer's existing 150-second exit.
        self.assertEqual(ack, '')
        self.assertEqual(observations, [''])

    def test_malformed_commands_still_fail_immediately_without_ack(self):
        for command in ('missing-separator', '1 unsupported'):
            with self.subTest(command=command):
                error, ack, observations, events, _ = self.run_commands(malformed=command)
                self.assertIsInstance(error, ValueError)
                self.assertEqual(ack, '')
                self.assertEqual(observations, [])
                self.assertFalse(any(entry[1] == 'send' for entry in events))

    @unittest.skipUnless(sys.platform == 'win32', 'requires actual Windows file sharing')
    def test_real_windows_sharing_denial_then_read_and_persistent_error(self):
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        create = kernel32.CreateFileW
        create.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                           wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        create.restype = wintypes.HANDLE
        close = kernel32.CloseHandle
        close.argtypes = [wintypes.HANDLE]
        close.restype = wintypes.BOOL
        with tempfile.TemporaryDirectory(prefix='nsd-sharing-') as temporary:
            path = Path(temporary) / 'command'
            path.write_text('1 quit', encoding='utf-8')
            reader = windows_peer.CommandReader(path)
            handle = create(str(path), 0x80000000, 0, None, 3, 0x80, None)
            self.assertIsNotNone(handle)
            self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
            try:
                # This is the failed call in the CI trace, with a real kernel
                # share denial rather than an injected Python exception.
                with self.assertRaises(PermissionError):
                    path.read_text(encoding='utf-8')
                with mock.patch.object(windows_peer.time, 'monotonic', return_value=10.0):
                    self.assertEqual(reader.read(), '')
                    expired = windows_peer.CommandReader(path)
                    self.assertEqual(expired.read(), '')
                with mock.patch.object(windows_peer.time, 'monotonic', return_value=11.0):
                    with self.assertRaises(PermissionError):
                        expired.read()
            finally:
                self.assertTrue(close(handle))
            with mock.patch.object(windows_peer.time, 'monotonic', return_value=10.5):
                self.assertEqual(reader.read(), '1 quit', 'actual released file is read before acknowledging')


class PermissionFixtureTest(unittest.TestCase):
    def test_three_manifests_preserve_permission_matrix_without_debugging(self):
        android = '{http://schemas.android.com/apk/res/android}'
        for variant, permissions in VARIANTS.items():
            with self.subTest(variant=variant):
                manifest = ET.parse(ROOT / 'tests/nsd/permissions' / variant / 'AndroidManifest.xml').getroot()
                self.assertEqual(manifest.get('package'), 'gesher.qa.nsdpermissions.' + variant)
                self.assertEqual(manifest.find('uses-sdk').get(android + 'targetSdkVersion'), '36')
                self.assertEqual({permission.get(android + 'name') for permission in manifest.findall('uses-permission')},
                                 set(permissions))
                application = manifest.find('application')
                self.assertIn(application.get(android + 'debuggable'), (None, 'false'))
                self.assertEqual(application.find('receiver').get(android + 'exported'), 'true')

    def test_built_apk_badging_rejects_debuggable_or_wrong_identity_permissions(self):
        for variant, permissions in VARIANTS.items():
            badging = "package: name='gesher.qa.nsdpermissions." + variant + "'\ntargetSdkVersion:'36'\n"
            badging += ''.join("uses-permission: name='" + permission + "'\n" for permission in permissions)
            with self.subTest(variant=variant):
                validate_badging(variant, badging, permissions)
                for changed in (badging + 'application-debuggable\n', badging.replace("Version:'36'", "Version:'35'"),
                                badging.replace('gesher.qa.nsdpermissions.', 'other.fixture.'),
                                badging.replace('uses-permission:', 'removed-permission:') if permissions else
                                badging + "uses-permission: name='android.permission.INTERNET'\n"):
                    with self.assertRaises(RuntimeError):
                        validate_badging(variant, changed, permissions)


if __name__ == '__main__':
    unittest.main(verbosity=2)
