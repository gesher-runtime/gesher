#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Independent valid DNS-SD packets and an owned UDP peer for resolution gates.

The explicit loopback mode measures Java resolver/UDP interoperability only.
Multicast mode is the required actual Windows transport gate. Neither mode
enumerates or logs unrelated names, queries or addresses.
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

from peer_scope import bind_windows_multicast

INSTANCE = 'Owned.שלום\\😀'
TYPE = '_grcontract._tcp'
HOST = ['fixture', 'local']
BINARY = bytes([0, 255, 254, 61, 128]) + bytes(range(200))


def name(labels):
    out = bytearray()
    for label in labels:
        value = label.encode('utf-8')
        if not 0 < len(value) <= 63:
            raise ValueError('fixture label exceeds DNS length')
        out += bytes([len(value)]) + value
    return bytes(out) + b'\0'


def txt():
    entries = [b'flag', b'empty=', b'bin=' + BINARY, b'dup=first', b'DUP=second',
               b'replace', b'REPLACE=later']
    entries += [('p%d=' % n).encode() + bytes([65 + n]) * 230 for n in range(4)]
    return b''.join(bytes([len(value)]) + value for value in entries)


def rr(owner, kind, data, ttl=120, cls=0x8001):
    return name(owner) + struct.pack('!HHIH', kind, cls, ttl, len(data)) + data


def response(records, flags=0x8400):
    return struct.pack('!6H', 0, flags, 0, len(records), 0, 0) + b''.join(records)


def records(instance=INSTANCE, service_type=TYPE, host=HOST, port=43123,
            address='192.0.2.10', family=4, ttl=120):
    owner = [instance] + service_type.split('.') + ['local']
    srv = rr(owner, 33, struct.pack('!HHH', 0, 0, port) + name(host), ttl)
    text = rr(owner, 16, txt(), ttl)
    ip = rr(host, 1 if family == 4 else 28,
            socket.inet_pton(socket.AF_INET if family == 4 else socket.AF_INET6, address), ttl)
    return srv, text, ip


def fixtures(path):
    owner = [INSTANCE] + TYPE.split('.') + ['local']
    srv, text, ipv4 = records()
    ipv6 = records(address='fe80::1234', family=6)[2]
    site6 = records(address='fec0::1234', family=6)[2]
    global6 = records(address='2001:db8::1234', family=6)[2]
    second4 = records(address='192.0.2.11')[2]
    changed = records(host=['changed', 'local'])
    values = {
        'srv': response([srv]), 'txt': response([text]), 'a': response([ipv4]),
        'txt_a': response([text, ipv4]),
        'aaaa': response([ipv6]), 'full4': response([srv, text, ipv4]),
        'full6': response([srv, text, ipv6]), 'fullboth': response([srv, text, ipv4, ipv6]),
        'addresses': response([ipv4, second4, ipv6]),
        'ipv6_scopes': response([srv, text, ipv6, site6, global6]),
        'flush4': response([second4]),
        'srv_changed': response([changed[0]]), 'a_changed': response([changed[2]]),
        'goodbye_srv': response([records(ttl=0)[0]]),
        'goodbye_txt': response([records(ttl=0)[1]]),
        'goodbye_a': response([records(ttl=0)[2]]),
        'goodbye_ptr': response([rr(TYPE.split('.') + ['local'], 12, name(owner), 0, 1)]),
        'other_instance': response(records(instance='Another owned instance')),
        'other_host': response([records(host=['another', 'local'])[2]]),
        'empty_txt': response([srv, rr(owner, 16, b'\0'), ipv4]),
        'short_srv': response([records(ttl=1)[0]]),
        'query': struct.pack('!6H', 0, 0, 1, 0, 0, 0) + name(owner) + struct.pack('!HH', 33, 1),
        'status': struct.pack('!6H', 0, 0x9000, 0, 0, 0, 0),
    }
    def subtype(label, ttl=120, service_type=TYPE, instance=INSTANCE):
        return rr([label, '_sub'] + service_type.split('.') + ['local'], 12,
                  name([instance] + TYPE.split('.') + ['local']), ttl, 1)
    values.update({
        'subtypes': response([subtype('_fast', 1), subtype('_slow'),
                              subtype('_other', service_type='_otherowned._tcp'),
                              subtype('_another', instance='Another owned instance')]),
        'subtype_case': response([subtype('_FAST')]),
        'subtype_goodbye': response([subtype('_fast', 0)]),
        'full_subtypes': response([subtype('_fast'), srv, text, ipv4]),
        'subtypes_budget': response([subtype('_owned%02d' % n) for n in range(65)]),
    })
    # Independently encode legal DNS name compression in owner and SRV target.
    owner_wire = name(owner)
    host_offset = 12 + len(owner_wire) + 10 + 6
    srv_body = struct.pack('!HHH', 0, 0, 43123) + name(HOST)
    compressed = [owner_wire + struct.pack('!HHIH', 33, 0x8001, 120, len(srv_body)) + srv_body,
                  b'\xc0\x0c' + struct.pack('!HHIH', 16, 0x8001, 120, len(txt())) + txt(),
                  struct.pack('!H', 0xc000 | host_offset) + struct.pack('!HHIH', 1, 0x8001, 120, 4)
                  + socket.inet_aton('192.0.2.10')]
    values['compressed'] = response(compressed)
    path.write_text(''.join(key + '\t' + value.hex() + '\n' for key, value in values.items()), encoding='utf-8')
    return {key: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()}
            for key, value in values.items()}


def read_name(data, position, depth=0):
    if depth > 16:
        raise ValueError('fixture query name nesting')
    labels = []
    while position < len(data):
        length = data[position]
        position += 1
        if not length:
            return labels, position
        if length & 0xc0 == 0xc0:
            target = ((length & 63) << 8) | data[position]
            rest, _ = read_name(data, target, depth + 1)
            return labels + rest, position + 1
        labels.append(data[position:position + length])
        position += length
    raise ValueError('incomplete fixture query')


def key(labels):
    return tuple(label.translate(bytes.maketrans(b'abcdefghijklmnopqrstuvwxyz', b'ABCDEFGHIJKLMNOPQRSTUVWXYZ'))
                 for label in labels)


def asks_owned(data, instance, service_type, host):
    if len(data) < 12:
        return False
    _, flags, count, _, _, _ = struct.unpack_from('!6H', data)
    if flags & 0xf80f or count > 8:
        return False
    owners = {key([part.encode('utf-8') for part in [instance] + service_type.split('.') + ['local']]),
              key([part.encode('utf-8') for part in host])}
    at = 12
    try:
        for _ in range(count):
            labels, at = read_name(data, at)
            kind, cls = struct.unpack_from('!HH', data, at)
            at += 4
            if key(labels) in owners and kind in (1, 16, 28, 33) and cls & 0x7fff == 1:
                return True
    except (ValueError, IndexError, struct.error):
        pass
    return False


def serve(args):
    if not re.fullmatch(r'_gr[0-9a-f]{12}\._tcp', args.type):
        raise ValueError('owned random service type required')
    args.control.mkdir(parents=True, exist_ok=True)
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    # Own both sockets before setup, including a rejected bind/option/readback.
    with socket.socket(family, socket.SOCK_STREAM) as tcp, \
            socket.socket(family, socket.SOCK_DGRAM) as sock:
        run_peer(args, tcp, sock)


def run_peer(args, tcp, sock):
    family = socket.AF_INET if args.family == 4 else socket.AF_INET6
    address = args.address.split('%')[0]
    local = (address, 0) if args.family == 4 else (address, 0, 0, args.index)
    tcp.bind(local); tcp.listen(1)
    port = tcp.getsockname()[1]
    if args.loopback:
        if not socket.inet_pton(family, address) in (socket.inet_pton(socket.AF_INET, '127.0.0.1'),
                                                    socket.inet_pton(socket.AF_INET6, '::1')):
            raise ValueError('host transport is explicitly loopback-only')
        sock.bind(local)
        destination = None
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
    host = ['h' + args.type[1:15], 'local']
    wire = response(records(args.instance, args.type, host, port, address, args.family))
    # Value-policy bytes, raw duplicate TXT bytes and full DNS bytes are distinct.
    attributes = {b'flag': None, b'empty': b'', b'bin': BINARY, b'dup': b'first', b'replace': b'later'}
    attributes.update({('p%d' % n).encode(): bytes([65 + n]) * 230 for n in range(4)})
    attribute_bytes = sum(len(k) + (0 if v is None else len(v)) + 2 for k, v in attributes.items())
    if attribute_bytes > 1300:
        raise ValueError('successful fixture exceeds Android value policy')
    metadata = {'family': args.family, 'address': address, 'index': args.index, 'port': port,
                'udpPort': sock.getsockname()[1], 'hostname': host[0], 'wireBytes': len(wire),
                'rawTxtBytes': len(txt()), 'attributePolicyBytes': attribute_bytes,
                'wireSha256': hashlib.sha256(wire).hexdigest(), 'binaryHex': BINARY.hex(),
                'transport': 'loopback-fixture' if args.loopback else 'real-multicast-peer'}
    (args.control / 'ready.json').write_text(json.dumps(metadata, indent=2) + '\n')
    ready_file = args.control / 'ready-new'
    ready_file.write_text(str(sock.getsockname()[1]) + '\n' + str(port) + '\n' + host[0] + '\n')
    ready_file.replace(args.control / 'ready')
    started = time.monotonic(); current = ''; mode = 'answer'; requests = 0; answers = 0
    try:
        while time.monotonic() - started < 120:
            control = args.control / 'command'
            command = control.read_text().strip() if control.exists() else ''
            if command and command != current:
                current = command
                serial, mode = command.split(' ', 1)
                if mode not in ('answer', 'silent', 'quit'):
                    raise ValueError('unknown fixture command')
                (args.control / 'ack').write_text(serial)
                if mode == 'quit':
                    break
            ready, _, _ = select.select([sock, tcp], [], [], 0.05)
            if tcp in ready:
                connection, _ = tcp.accept()
                with connection:
                    connection.sendall(b'gesher-owned-nsd-peer\n')
            if sock not in ready:
                continue
            query, sender = sock.recvfrom(65535)
            if not asks_owned(query, args.instance, args.type, host):
                continue
            requests += 1
            if mode == 'answer':
                sock.sendto(wire, sender if destination is None else destination)
                answers += 1
    finally:
        (args.control / 'result.json').write_text(json.dumps({**metadata,
                'ownedQueries': requests, 'answers': answers}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--type'); parser.add_argument('--instance', default=INSTANCE)
    parser.add_argument('--index', type=int); parser.add_argument('--address')
    parser.add_argument('--family', type=int, choices=(4, 6))
    parser.add_argument('--loopback', action='store_true')
    args = parser.parse_args()
    if args.fixtures:
        args.fixtures.parent.mkdir(parents=True, exist_ok=True)
        result = fixtures(args.fixtures)
        args.fixtures.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    else:
        serve(args)


if __name__ == '__main__':
    main()
