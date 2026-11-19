#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Owned ordinary service-info updates over an independent Python UDP peer.

The host mode binds loopback only. The optional multicast mode is a separate
Windows qualification fixture: it requires an explicitly selected real adapter.
Neither mode discovers or logs other services. All transmitted records are valid.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import select
import socket
import struct
import time

from resolver_peer import asks_owned, name, records, response, rr
from peer_scope import bind_windows_multicast

INSTANCE = 'Owned.שלום\\😀'
TYPE = '_grsubscription._tcp'
HOST = ['subscriber-owned', 'local']


def values(instance=INSTANCE, service_type=TYPE, address='127.0.0.1', family=4, port=43123):
    owner = [instance] + service_type.split('.') + ['local']
    srv, txt, ip = records(instance, service_type, HOST, port, address, family)
    # Keep null/empty values separate from arbitrary binary bytes.
    entries = [b'flag', b'empty=', b'bin=\0\xff\xfe=\x80\x02\x03']
    changed = rr(owner, 16, b''.join(bytes([len(v)]) + v for v in entries))
    return {
        'complete': response([srv, txt, ip]),
        'update': response([srv, changed, ip]),
        'goodbye': response([rr(service_type.split('.') + ['local'], 12, name(owner), 0, 1)]),
        'reappear': response([srv, changed, ip]),
    }


def fixtures(path):
    packets = values()
    path.write_text(''.join(key + '\t' + value.hex() + '\n' for key, value in packets.items()))
    return {key: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
            for key, value in packets.items()}


def serve(args):
    if not re.fullmatch(r'_gr[0-9a-f]{12}\._tcp', args.type):
        raise ValueError('owned random service type required')
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    address = args.address.split('%')[0]
    local = (address, 0) if args.family == 4 else (address, 0, 0, args.index)
    if args.loopback and not socket.inet_pton(family, address) == socket.inet_pton(
            family, '127.0.0.1' if args.family == 4 else '::1'):
        raise ValueError('host transport is explicitly loopback-only')
    args.control.mkdir(parents=True, exist_ok=False)
    # Own both sockets throughout setup, including a rejected receive guard.
    with socket.socket(family, socket.SOCK_STREAM) as tcp, \
            socket.socket(family, socket.SOCK_DGRAM) as sock:
        run_peer(args, tcp, sock, address, local)


def run_peer(args, tcp, sock, address, local):
    tcp.bind(local)
    tcp.listen(1)
    port = tcp.getsockname()[1]
    if args.loopback:
        sock.bind(local)
        multicast = None
    else:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if args.family == 4:
            bind_windows_multicast(sock, args.family, args.index)
            ip = socket.inet_aton(address)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, ip)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                            socket.inet_aton('224.0.0.251') + ip)
            multicast = ('224.0.0.251', 5353)
        else:
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            bind_windows_multicast(sock, args.family, args.index)
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, args.index)
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_LOOP, 1)
            sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                            socket.inet_pton(socket.AF_INET6, 'ff02::fb') + struct.pack('@I', args.index))
            multicast = ('ff02::fb', 5353, 0, args.index)
    packets = values(args.instance, args.type, address, args.family, port)
    metadata = dict(family=args.family, address=address, index=args.index, tcpPort=port,
                    udpPort=sock.getsockname()[1], instance=args.instance, serviceType=args.type,
                    transport='loopback-fixture' if args.loopback else 'real-multicast-peer',
                    packetHashes={k: hashlib.sha256(v).hexdigest() for k, v in packets.items()})
    (args.control / 'ready.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (args.control / 'ready').write_text(str(sock.getsockname()[1]) + '\n' + str(port) + '\n')
    start, current, mode, destination = time.monotonic(), '', 'complete', multicast
    queries = sent = connections = 0
    try:
        while time.monotonic() - start < 90:
            command_path = args.control / 'command'
            try:
                command = command_path.read_text().strip()
            except FileNotFoundError:
                command = ''
            if command and command != current:
                serial, requested = command.split(' ', 1)
                if requested not in (*packets, 'silent', 'quit'):
                    raise ValueError('unknown owned peer command')
                current, mode = command, requested
                if mode in packets and destination is not None:
                    sock.sendto(packets[mode], destination)
                    sent += 1
                acknowledgment = args.control / 'ack-new'
                acknowledgment.write_text(serial)
                acknowledgment.replace(args.control / 'ack')
                if mode == 'quit':
                    break
            ready, _, _ = select.select([sock, tcp], [], [], 0.02)
            if tcp in ready:
                connection, _ = tcp.accept()
                with connection:
                    connection.sendall(b'gesher-owned-subscription-peer\n')
                connections += 1
            if sock not in ready:
                continue
            query, sender = sock.recvfrom(65535)
            if not asks_owned(query, args.instance, args.type, HOST):
                continue
            queries += 1
            destination = sender if args.loopback else multicast
            if mode in packets:
                sock.sendto(packets[mode], destination)
                sent += 1
    finally:
        (args.control / 'result.json').write_text(json.dumps({**metadata, 'ownedQueries': queries,
                'sentPackets': sent, 'tcpConnections': connections}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--type')
    parser.add_argument('--instance', default=INSTANCE)
    parser.add_argument('--address')
    parser.add_argument('--index', type=int)
    parser.add_argument('--family', type=int, choices=(4, 6))
    parser.add_argument('--loopback', action='store_true')
    args = parser.parse_args()
    if args.fixtures:
        print(json.dumps(fixtures(args.fixtures), indent=2))
    else:
        serve(args)


if __name__ == '__main__':
    main()
