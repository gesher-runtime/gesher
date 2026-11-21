#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Exercise NSD fixture setup at an explicit socket boundary, without network traffic.

These are host checks of the actual helper and peer entry points, not execution
of the Windows provider. The live peer independently requires provider readback.
"""
import argparse
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
