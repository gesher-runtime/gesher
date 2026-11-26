#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Owned, independent raw-wire mDNS PTR fixture for the Windows production gate.

Only a per-run random service type is emitted; unrelated service types are never
queried or logged. No Gesher or Windows DNS-SD API builds the fixture packets.
"""
import argparse
from pathlib import Path
import select
import re
import socket
import struct
import time

from peer_scope import bind_windows_multicast


class CommandReader:
    """Observe replaced control files without blocking the network loop.

    Windows can deny an open briefly while the Java peer replaces the old
    command. Retry that access error for at most one second, without an ack or
    a mode change. Missing/empty observations never extend an active deadline.
    """
    def __init__(self, path):
        self.path = path
        self.retry_deadline = None
        self.retry_error = None

    def read(self):
        if self.retry_deadline is not None and time.monotonic() >= self.retry_deadline:
            raise self.retry_error
        try:
            command = self.path.read_text(encoding='utf-8').strip()
        except FileNotFoundError:
            return ''  # The first command or its replacement is not visible yet.
        except PermissionError as error:
            if self.retry_deadline is None:
                self.retry_deadline = time.monotonic() + 1.0
                self.retry_error = error
            return ''
        if command:
            self.retry_deadline = None
            self.retry_error = None
        return command


def name(labels):
    out = bytearray()
    for label in labels:
        encoded = label.encode('utf-8')
        if not 0 < len(encoded) <= 63:
            raise ValueError('invalid fixture label length')
        out.append(len(encoded))
        out.extend(encoded)
    out.append(0)
    return bytes(out)


def packet(service_type, instance, ttl, port, address, family, base_goodbye=False,
           base_only=False):
    labels = service_type.split('.') + ['local']
    target = name([instance] + labels)
    records = []
    for owner in ((labels,) if base_only else (labels, ['_printer', '_sub'] + labels)):
        records.append(name(owner) + struct.pack('!HHIH', 12, 1, ttl, len(target)) + target)
    if base_goodbye:
        return struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0) + records[0]
    host = name(['h' + service_type.split('.')[0][1:], 'local'])
    srv = struct.pack('!HHH', 0, 0, port) + host
    txt_value = b'bin=before\x00after'
    txt = bytes([len(txt_value)]) + txt_value
    service_ttl = (180 if ttl >= 120 else 5) if ttl else 0
    ip = socket.inet_pton(socket.AF_INET if family == 4 else socket.AF_INET6,
                          address.split('%')[0])
    additional = [target + struct.pack('!HHIH', 33, 0x8001, service_ttl, len(srv)) + srv,
                  target + struct.pack('!HHIH', 16, 0x8001, service_ttl, len(txt)) + txt,
                  host + struct.pack('!HHIH', 1 if family == 4 else 28,
                                     0x8001, service_ttl, len(ip)) + ip]
    return struct.pack('!6H', 0, 0x8400, 0, len(records), 0, len(additional)) \
            + b''.join(records + additional)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--control', type=Path, required=True)
    p.add_argument('--type', required=True)
    p.add_argument('--instance', required=True)
    p.add_argument('--index', type=int, required=True)
    p.add_argument('--address', required=True)
    p.add_argument('--family', type=int, choices=(4, 6), required=True)
    args = p.parse_args()
    serve(args)


def serve(args):
    if not re.fullmatch(r'_gs[0-9a-f]{12}\._tcp', args.type):
        raise ValueError('fixture requires an owned unique service type')
    args.control.mkdir(parents=True, exist_ok=True)
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    # Own both sockets before setup, including a rejected bind/option/readback.
    with socket.socket(family, socket.SOCK_STREAM) as service, \
            socket.socket(family, socket.SOCK_DGRAM) as sock:
        run_peer(args, service, sock)


def run_peer(args, service, sock):
    service.bind((args.address, 0) if args.family == 4 else
                 (args.address.split('%')[0], 0, 0, args.index))
    service.listen(1)
    port = service.getsockname()[1]
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if args.family == 4:
        bind_windows_multicast(sock, args.family, args.index)
        local = socket.inet_aton(args.address)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, local)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                        socket.inet_aton('224.0.0.251') + local)
        destination = ('224.0.0.251', 5353)
    else:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        bind_windows_multicast(sock, args.family, args.index)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, args.index)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_LOOP, 1)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                        socket.inet_pton(socket.AF_INET6, 'ff02::fb') + struct.pack('@I', args.index))
        destination = ('ff02::fb', 5353, 0, args.index)
    query_name = name(args.type.split('.') + ['local'])
    current_type, current_instance, positive_ttl = args.type, args.instance, 2
    current = ''
    mode = 'silent'
    next_send = 0
    started = time.monotonic()
    commands = CommandReader(args.control / 'command')
    (args.control / 'ready').write_text('ready', encoding='utf-8')
    while time.monotonic() - started < 150:
        command = commands.read()
        if command and command != current:
            current = command
            serial, mode = command.split(' ', 1)
            if mode not in ('announce', 'announce-long', 'announce-subtype-long', 'announce-base-only-long',
                            'silent', 'goodbye', 'base-goodbye', 'quit'):
                raise ValueError('unknown fixture command')
            if mode == 'announce':
                current_type, current_instance, positive_ttl = args.type, args.instance, 2
            elif mode == 'announce-long':
                current_type, current_instance, positive_ttl = args.type.replace('_gs', '_gl', 1), args.instance + '-long', 120
            elif mode in ('announce-subtype-long', 'announce-base-only-long'):
                current_type, current_instance, positive_ttl = args.type.replace('_gs', '_gu', 1), args.instance + '-sub-long', 120
            query_name = name(current_type.split('.') + ['local'])
            if mode.startswith('announce') or mode in ('goodbye', 'base-goodbye'):
                sock.sendto(packet(current_type, current_instance, positive_ttl if mode.startswith('announce') else 0,
                                   port, args.address, args.family, mode == 'base-goodbye',
                                   mode == 'announce-base-only-long'), destination)
                next_send = time.monotonic() + 0.5
            (args.control / 'ack').write_text(serial, encoding='utf-8')
            if mode == 'quit':
                return
        now = time.monotonic()
        readable, _, _ = select.select([sock], [], [], 0.05)
        asked = False
        if readable:
            data, _ = sock.recvfrom(9000)
            # Respond only to this fixture type; never enumerate another peer.
            asked = len(data) >= 12 and not (data[2] & 0x80) and query_name in data
        if mode.startswith('announce') and (now >= next_send or asked):
            sock.sendto(packet(current_type, current_instance, positive_ttl, port, args.address,
                               args.family, base_only=mode == 'announce-base-only-long'), destination)
            next_send = now + 0.5


if __name__ == '__main__':
    main()
