# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Receive-interface guard for the owned Windows multicast peers.

Group membership and MULTICAST_IF select multicast membership and outgoing
traffic; neither limits all datagrams received by a wildcard-bound socket.
Keep that bind for real multicast and require Windows' receive allowlist.
"""
import socket
import struct
import sys

# IP_* and IPV6_* share these WinSDK ws2ipdef.h values. Python does not expose
# them on every Windows version. Each value is a native four-byte DWORD, not
# the network-byte-order encoding used by some other IPv4 socket options.
IFLIST = 28
ADD_IFLIST = 29
GET_IFLIST = 33


def bind_windows_multicast(sock, family, index):
    """Bind only after enabling a singleton receive-interface allowlist.

    The caller owns and closes the socket on every setup/verification error.
    This is intentionally unavailable on other hosts; the resolver's explicit
    loopback mode and packet-only generation do not call this function.
    """
    if sys.platform != 'win32':
        raise RuntimeError('Live NSD multicast fixtures require Windows receive-interface filtering')
    if family not in (4, 6):
        raise ValueError('NSD multicast family must be 4 or 6')
    if type(index) is not int or not 0 < index <= 0xffffffff:
        raise ValueError('NSD multicast requires a nonzero DWORD interface index')
    level = socket.IPPROTO_IP if family == 4 else socket.IPPROTO_IPV6
    selected = struct.pack('=I', index)
    try:
        sock.setsockopt(level, IFLIST, struct.pack('=I', 1))
        sock.setsockopt(level, ADD_IFLIST, selected)
        # Windows can defer pre-bind option validation until bind. Never publish
        # readiness or receive traffic until bind AND both readbacks succeed.
        sock.bind(('', 5353) if family == 4 else ('::', 5353))
        enabled = sock.getsockopt(level, IFLIST, 4)
        if len(enabled) != 4 or struct.unpack('=I', enabled)[0] == 0:
            raise RuntimeError('NSD multicast receive-interface filtering is not enabled')
        # Request room for more than one DWORD so an additional interface cannot
        # be mistaken for a singleton through truncation. An oversized provider
        # result or unsupported readback is an error, never a broad fallback.
        actual = sock.getsockopt(level, GET_IFLIST, 1024)
        if actual != selected:
            raise RuntimeError('NSD multicast receive allowlist differs from the selected interface')
    except OSError as error:
        raise RuntimeError('Cannot verify the Windows NSD multicast receive-interface restriction') from error
