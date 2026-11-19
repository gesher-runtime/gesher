#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
"""Independent valid owned Windows DNS-SD peer; no runtime codec/provider imports.

Only the random fixture service is queried and retained. Actual WSARecvMsg
metadata distinguishes multicast, QU and legacy replies. --self-test exercises
valid fixture encodings and layouts only; it does not execute Windows.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import time
import traceback


def attributes(epoch):
    result = {b'flag': None, b'empty': b'', b'bin': bytes([0, 255, 254, 61, 128]) + bytes(range(200)),
              b'epoch': str(epoch).encode('ascii')}
    result.update({('p%d' % n).encode(): bytes([65 + n]) * 230 for n in range(4)})
    result[b'tail'] = bytes(255 - n for n in range(127))
    assert sum(len(k) + 2 + (len(v) if v is not None else 0) for k, v in result.items()) == 1300
    return result


def public_attributes(): return {b'flag': None, b'empty': b'', b'public': 'héllo\0'.encode('utf-8')}


def attributes_hash(values):
    raw = b''.join(struct.pack('!i', len(k)) + k + struct.pack('!i', -1 if v is None else len(v))
                   + (v if v is not None else b'') for k, v in sorted(values.items()))
    return hashlib.sha256(raw).hexdigest()


def txt_bytes(values):
    entries = [k + (b'=' + v if v is not None else b'') for k, v in values.items()]
    assert all(len(e) <= 255 for e in entries)
    return b''.join(bytes([len(e)]) + e for e in entries)


def txt_values(data):
    result = {}; at = 0
    while at < len(data):
        size = data[at]; at += 1
        if at + size > len(data): raise ValueError('TXT framing')
        entry = data[at:at + size]; at += size
        if entry:
            key, sep, value = entry.partition(b'=')
            result.setdefault(key.lower(), value if sep else None)
    return result


def name(labels):
    labels = [x.encode('utf-8') if isinstance(x, str) else x for x in labels]
    if any(not 0 < len(x) <= 63 for x in labels): raise ValueError('fixture label bounds')
    return b''.join(bytes([len(x)]) + x for x in labels) + b'\0'


def read_name(data, at, depth=0):
    if depth > 16: raise ValueError('pointer nesting')
    labels = []
    while at < len(data):
        size = data[at]; at += 1
        if not size: return tuple(labels), at
        if size & 0xc0 == 0xc0:
            tail, _ = read_name(data, ((size & 63) << 8) | data[at], depth + 1)
            return tuple(labels) + tail, at + 1
        if size > 63 or at + size > len(data): raise ValueError('name framing')
        labels.append(data[at:at + size]); at += size
    raise ValueError('incomplete name')


def key(labels): return tuple(x.lower() for x in labels)


def decode(data):
    ident, flags, qd, an, ns, ar = struct.unpack_from('!6H', data)
    if qd + an + ns + ar > 2048: raise ValueError('fixture packet resource bound')
    questions = []; records = []; at = 12
    for _ in range(qd):
        owner, at = read_name(data, at); kind, cls = struct.unpack_from('!HH', data, at); at += 4
        questions.append((owner, kind, cls))
    for section, count in [('answer', an), ('authority', ns), ('additional', ar)]:
        for _ in range(count):
            owner, at = read_name(data, at)
            kind, cls, ttl, size = struct.unpack_from('!HHIH', data, at); at += 10
            end = at + size
            if end > len(data): raise ValueError('RR framing')
            rr = dict(owner=owner, type=kind, cls=cls, ttl=ttl, section=section, raw=data[at:end])
            if kind in (12, 33):
                offset = at
                if kind == 33:
                    rr['priority'], rr['weight'], rr['port'] = struct.unpack_from('!HHH', data, at)
                    offset += 6
                rr['target'], after = read_name(data, offset)
                if after != end: raise ValueError('name RDATA framing')
            if kind == 16: rr['attributes'] = txt_values(rr['raw'])
            records.append(rr); at = end
    if at != len(data): raise ValueError('DNS tail')
    return dict(id=ident, flags=flags, questions=questions, records=records, counts=[qd, an, ns, ar])


def query(owner, kind, qu=False, ident=0):
    return struct.pack('!6H', ident, 0, 1, 0, 0, 0) + name(owner) + struct.pack('!HH', kind, 0x8001 if qu else 1)


def record(owner, kind, body, ttl=120, cls=0x8001):
    return name(owner) + struct.pack('!HHIH', kind, cls, ttl, len(body)) + body


class WsaBuf(C.Structure):
    _fields_ = [('length', C.c_uint32), ('buffer', C.c_void_p)]


class WsaMsg(C.Structure):
    _fields_ = [('name', C.c_void_p), ('nameLength', C.c_int32), ('buffers', C.POINTER(WsaBuf)),
                ('bufferCount', C.c_uint32), ('control', WsaBuf), ('flags', C.c_uint32)]


class Guid(C.Structure):
    _fields_ = [('a', C.c_uint32), ('b', C.c_uint16), ('c', C.c_uint16), ('d', C.c_ubyte * 8)]


def windows_layout():
    return dict(pointer=C.sizeof(C.c_void_p), WSABUF=C.sizeof(WsaBuf), WSAMSG=C.sizeof(WsaMsg),
                buffersOffset=WsaMsg.buffers.offset, controlOffset=WsaMsg.control.offset,
                flagsOffset=WsaMsg.flags.offset, GUID=C.sizeof(Guid))


class WindowsReceiver:
    """Independent extended receive with actual Windows destination/interface/hops."""
    def __init__(self, sock, family):
        if os.name != 'nt' or C.sizeof(C.c_void_p) != 8:
            raise RuntimeError('actual 64-bit Windows required for network qualification')
        self.sock, self.family = sock, family
        self.ws = C.WinDLL('Ws2_32.dll'); self.ws.WSAGetLastError.restype = C.c_int
        self.ws.WSAIoctl.argtypes = [C.c_size_t, C.c_uint32, C.c_void_p, C.c_uint32, C.c_void_p,
                                    C.c_uint32, C.POINTER(C.c_uint32), C.c_void_p, C.c_void_p]
        self.ws.WSAIoctl.restype = C.c_int
        guid = Guid(0xf689d7c8, 0x6f1f, 0x436b, (C.c_ubyte * 8)(0x8a, 0x53, 0xe5, 0x4f, 0xe3, 0x51, 0xc3, 0x22))
        pointer, returned = C.c_void_p(), C.c_uint32()
        if self.ws.WSAIoctl(sock.fileno(), 0xc8000006, C.byref(guid), C.sizeof(guid),
                            C.byref(pointer), C.sizeof(pointer), C.byref(returned), None, None):
            raise OSError(self.ws.WSAGetLastError(), 'WSARecvMsg lookup')
        if returned.value != C.sizeof(pointer) or not pointer.value: raise RuntimeError('WSARecvMsg unavailable')
        signature = C.WINFUNCTYPE(C.c_int, C.c_size_t, C.POINTER(WsaMsg), C.POINTER(C.c_uint32), C.c_void_p, C.c_void_p)
        self.receive = signature(pointer.value)
        level = socket.IPPROTO_IP if family == 4 else socket.IPPROTO_IPV6
        sock.setsockopt(level, 19, 1)  # IP_PKTINFO / IPV6_PKTINFO in selected Windows SDK.
        sock.setsockopt(level, 21, 1)  # IP_RECVTTL / IPV6_HOPLIMIT.

    def read(self):
        source, data, control = C.create_string_buffer(128), C.create_string_buffer(65535), C.create_string_buffer(256)
        vector = WsaBuf(C.sizeof(data), C.addressof(data))
        message = WsaMsg(C.addressof(source), C.sizeof(source), C.pointer(vector), 1,
                         WsaBuf(C.sizeof(control), C.addressof(control)), 0)
        received = C.c_uint32()
        if self.receive(self.sock.fileno(), C.byref(message), C.byref(received), None, None):
            error = self.ws.WSAGetLastError()
            if error == 10035: return None
            raise OSError(error, 'WSARecvMsg')
        if message.flags & (0x100 | 0x200): raise RuntimeError('truncated receive/control data')
        raw = source.raw[:message.nameLength]
        af = socket.AF_INET if self.family == 4 else socket.AF_INET6
        if struct.unpack_from('=H', raw)[0] != af: raise RuntimeError('source family')
        meta = dict(source=raw[4:8] if self.family == 4 else raw[8:24],
                    port=struct.unpack_from('!H', raw, 2)[0], index=0, destination=None, hops=-1)
        raw = control.raw[:message.control.length]; at = 0
        while at + 16 <= len(raw):
            size, level, kind = struct.unpack_from('=Qii', raw, at)
            if size < 16 or at + size > len(raw): raise RuntimeError('ancillary framing')
            body = raw[at + 16:at + size]
            if level == (socket.IPPROTO_IP if self.family == 4 else socket.IPPROTO_IPV6):
                if kind == 19:
                    width = 4 if self.family == 4 else 16
                    meta['destination'] = body[:width]; meta['index'] = struct.unpack_from('=I', body, width)[0]
                elif kind == 21 or self.family == 4 and kind == 4:
                    # IPv4 reports IP_TTL or IP_HOPLIMIT; setting IP_RECVTTL
                    # enables it. IPv6 reports IPV6_HOPLIMIT.
                    meta['hops'] = struct.unpack_from('=i', body)[0]
            at += (size + 7) & ~7
        if not meta['index'] or meta['destination'] is None: raise RuntimeError('missing actual receive metadata')
        return data.raw[:received.value], meta


def atomic(path, value):
    temporary = path.with_name(path.name + '.new')
    temporary.write_text(json.dumps(value, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


class Peer:
    def __init__(self, args):
        self.args = args; self.af = socket.AF_INET if args.family == 4 else socket.AF_INET6
        self.address = socket.inet_pton(self.af, args.address.split('%')[0])
        self.group = socket.inet_pton(self.af, '224.0.0.251' if args.family == 4 else 'ff02::fb')
        self.service_type = tuple(x.encode() for x in args.type.split('.')) + (b'local',)
        self.initial = (args.instance.encode('utf-8'),) + self.service_type
        self.sock = None; self.legacy = None; self.conflict_server = None
        try:
            self.sock = self.udp(True); self.legacy = self.udp(False)
            self.receivers = {s: WindowsReceiver(s, args.family) for s in (self.sock, self.legacy)}
            self.conflict_server = socket.socket(self.af, socket.SOCK_STREAM)
            self.conflict_server.bind(self.endpoint(0)); self.conflict_server.listen(1)
        except BaseException:
            for s in (self.sock, self.legacy, self.conflict_server):
                if s is not None: s.close()
            raise
        self.destination = ('224.0.0.251', 5353) if args.family == 4 else ('ff02::fb', 5353, 0, args.index)
        self.start = time.monotonic(); self.records = {}; self.hosts = set(); self.probes = {}; self.goodbyes = {}
        self.samples = []; self.txt_observed = []; self.steps = []; self.failure = None; self.serial = 0; self.pending = None
        self.query_started = False; self.unsolicited = set(); self.large_txt = False
        self.reuse_guard = None; self.reuse_seen = None; self.exit_names = []; self.exit_ports = []
        self.legacy_pending = None; self.legacy_answer = None; self.qu_pending = None; self.qu_answer = None
        self.conflicts_sent = 0; self.fragmented = False; self.emitted = set(); self.seeds = []

    def endpoint(self, port):
        address = self.args.address.split('%')[0]
        scoped = self.args.family == 6 and self.address[0] == 0xfe and self.address[1] & 0xc0 == 0x80
        return (address, port) if self.args.family == 4 else (address, port, 0, self.args.index if scoped else 0)

    def udp(self, multicast):
        s = socket.socket(self.af, socket.SOCK_DGRAM)
        try:
            if multicast: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if self.args.family == 4:
                s.bind(('', 5353) if multicast else self.endpoint(0))
                for option, value in ((socket.IP_MULTICAST_IF, self.address), (socket.IP_MULTICAST_TTL, 255),
                                      (socket.IP_MULTICAST_LOOP, 1), (socket.IP_TTL, 255)):
                    s.setsockopt(socket.IPPROTO_IP, option, value)
                if multicast: s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, self.group + self.address)
            else:
                s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                s.bind(('::', 5353) if multicast else self.endpoint(0))
                for option, value in ((socket.IPV6_MULTICAST_IF, self.args.index), (socket.IPV6_MULTICAST_HOPS, 255),
                                      (socket.IPV6_MULTICAST_LOOP, 1), (socket.IPV6_UNICAST_HOPS, 255)):
                    s.setsockopt(socket.IPPROTO_IPV6, option, value)
                if multicast:
                    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, self.group + struct.pack('@I', self.args.index))
            s.setblocking(False); return s
        except BaseException:
            s.close(); raise

    def owned(self, owner):
        return len(owner) == len(self.service_type) + 1 and key(owner[1:]) == key(self.service_type)

    def state(self):
        path = self.args.control / 'app-state.json'
        if not path.exists(): return None
        value = json.loads(path.read_text(encoding='utf-8'))
        if value.get('error'): raise AssertionError('installed application failed: ' + value['error'])
        return value

    def send(self, sock, data):
        self.emitted.add(hashlib.sha256(data).digest())
        if sock.sendto(data, self.destination) != len(data): raise AssertionError('peer datagram send incomplete')

    def conflict(self, parsed):
        if self.args.mode != 'conflict' or not any(key(q[0]) == key(self.initial) for q in parsed['questions']): return
        # A real owned listener publishes a valid conflicting SRV answer, only
        # in response to the fixture's original instance probe.
        port = self.conflict_server.getsockname()[1]; host = ('peer-' + self.args.type[3:15], 'local')
        srv = record(self.initial, 33, struct.pack('!HHH', 0, 0, port) + name(host))
        address = record(host, 1 if self.args.family == 4 else 28, self.address)
        self.send(self.sock, struct.pack('!6H', 0, 0x8400, 0, 2, 0, 0) + srv + address)
        self.conflicts_sent += 1

    def seed(self, parsed):
        if parsed['flags'] & 0x8000: return False
        seed_type = (('_gs' + self.args.type[3:15]).encode(), b'_tcp', b'local')
        seed_host = (('seed-' + self.args.type[3:15]).encode(), b'local')
        for owner, kind, cls in parsed['questions']:
            if key(owner) == key(seed_host) and kind == (1 if self.args.family == 4 else 28):
                self.send(self.sock, struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0)
                          + record(seed_host, kind, self.address))
                return True
            if len(owner) != 4 or key(owner[1:]) != key(seed_type) or kind not in (16, 33): continue
            if key(owner[:1]) not in ((b'seed-1',), (b'seed-2',)): continue
            epoch = int(owner[0][-1:]); host = ('seed-' + self.args.type[3:15], 'local')
            port = self.conflict_server.getsockname()[1]
            srv = record(owner, 33, struct.pack('!HHH', 0, 0, port) + name(host))
            address = record(host, 1 if self.args.family == 4 else 28, self.address)
            text = record(owner, 16, txt_bytes(attributes(epoch)))
            # Two complete valid packets, each independently decodable. The TXT
            # is one record and is never shortened to fit a writer buffer.
            wires = [struct.pack('!6H', 0, 0x8400, 0, 2, 0, 0) + srv + address,
                     struct.pack('!6H', 0, 0x8400, 0, 1, 0, 0) + text]
            for wire in wires: self.send(self.sock, wire)
            if epoch not in [s['epoch'] for s in self.seeds]:
                self.seeds.append(dict(epoch=epoch, attributePolicyBytes=1300, rawTxtBytes=1299,
                    txtSha256=hashlib.sha256(txt_bytes(attributes(epoch))).hexdigest(),
                    datagrams=[dict(bytes=len(w), sha256=hashlib.sha256(w).hexdigest(), wireHex=w.hex()) for w in wires]))
            return True
        return False

    def consume(self, sock):
        received = self.receivers[sock].read()
        if received is None: return
        data, meta = received
        if hashlib.sha256(data).digest() in self.emitted: return
        try: parsed = decode(data)
        except (ValueError, IndexError, struct.error): return  # Do not retain other traffic.
        if parsed['flags'] & 0x780f or meta['index'] != self.args.index: return
        if sock is self.legacy:
            if self.legacy_pending and parsed['id'] == self.legacy_pending[0] and parsed['flags'] & 0x8000:
                self.legacy_answer = data, parsed, meta
            return
        if meta['port'] == 5353 and meta['destination'] == self.group and self.seed(parsed): return
        relevant = [r for r in parsed['records'] if self.owned(r['owner'])
                    or key(r['owner']) == key(self.service_type) or key(r['owner']) in self.hosts
                    or (r['type'] == 12 and self.owned(r.get('target', ())))]
        questions = [q for q in parsed['questions'] if self.owned(q[0])]
        if not relevant and not questions or meta['port'] != 5353: return
        multicast = meta['destination'] == self.group
        if multicast and meta['hops'] != 255: raise AssertionError('owned multicast hop limit is not 255')
        if not multicast and meta['destination'] != self.address: return
        now = time.monotonic()
        if not parsed['flags'] & 0x8000:
            if parsed['counts'][2] and relevant:
                for rr in relevant:
                    if rr['type'] == 33 and self.owned(rr['owner']): self.hosts.add(key(rr['target']))
                for owner, kind, _ in questions:
                    if kind == 255: self.probes.setdefault(key(owner), []).append(now)
                self.conflict(parsed)
            return
        if parsed['id'] != 0 or not parsed['flags'] & 0x0400: raise AssertionError('mDNS ID/AA framing')
        if len(self.samples) >= 2048: raise AssertionError('owned receipt capacity')
        if len(data) + (28 if self.args.family == 4 else 48) > self.args.mtu:
            if sum(parsed['counts'][1:]) > 1: raise AssertionError('multi-RR datagram exceeds actual MTU')
            self.fragmented = True
        self.samples.append(dict(seconds=now-self.start, bytes=len(data), wireHex=data.hex(),
                                 sha256=hashlib.sha256(data).hexdigest(), index=meta['index'], hops=meta['hops'],
                                 source=socket.inet_ntop(self.af, meta['source']), port=meta['port'],
                                 destination=socket.inet_ntop(self.af, meta['destination']), counts=parsed['counts']))
        for rr in relevant:
            owner = key(rr['owner'])
            if rr['type'] == 12 and self.owned(rr.get('target', ())):
                target = key(rr['target'])
                if rr['ttl'] == 0:
                    if rr['cls'] & 0x8000: raise AssertionError('PTR goodbye has cache-flush')
                    self.goodbyes.setdefault(target, []).append(now)
                    if self.reuse_guard == target and self.reuse_seen is not None:
                        raise AssertionError('old goodbye followed the replacement TXT')
                elif not self.query_started: self.unsolicited.add(target)
            elif rr['ttl'] == 0: raise AssertionError('unique/shared-host record withdrawn with service PTR')
            if rr['type'] == 33 and self.owned(rr['owner']) and rr['ttl'] > 0: self.hosts.add(key(rr['target']))
            if rr['type'] in (1, 28, 33, 16) and rr['ttl'] > 0:
                self.records[(owner, rr['type'])] = rr
                if rr['type'] == 16 and self.owned(rr['owner']):
                    digest = hashlib.sha256(rr['raw']).hexdigest()
                    label = [x.decode('utf-8') for x in rr['owner']]
                    if not any(t['owner'] == label and t['rawTxtSha256'] == digest for t in self.txt_observed):
                        self.txt_observed.append(dict(owner=label, rawTxtBytes=len(rr['raw']), rawTxtSha256=digest,
                            rawTxtHex=rr['raw'].hex(), datagramSha256=hashlib.sha256(data).hexdigest(),
                            attributesCanonicalSha256=attributes_hash(rr['attributes']),
                            attributes={k.decode('ascii'): None if v is None else v.hex() for k,v in rr['attributes'].items()}))
                    if rr['attributes'] == attributes(2) and owner == self.reuse_guard:
                        self.reuse_seen = self.reuse_seen or now
                    self.large_txt |= len(rr['raw']) == 1299 and len(data) > 512
        # A/AAAA may precede SRV in the same packet.
        for rr in parsed['records']:
            if rr['type'] in (1, 28) and key(rr['owner']) in self.hosts and rr['ttl'] > 0:
                self.records[(key(rr['owner']), rr['type'], rr['raw'])] = rr
        if self.qu_pending and not multicast and any(key(r['owner']) == self.qu_pending
                and r['type'] == 33 and r['ttl'] > 0 for r in relevant):
            self.qu_answer = data, parsed, meta

    def verify(self, slot, connect=False):
        owner = key((slot['name'].encode('utf-8'),) + self.service_type)
        srv, txt = self.records.get((owner, 33)), self.records.get((owner, 16))
        expected = public_attributes() if self.args.mode == 'public' else attributes(slot['epoch'])
        if not srv or not txt or srv['port'] != slot['port'] or txt['attributes'] != expected: return False
        if slot['attributesCanonicalSha256'] != attributes_hash(txt['attributes']):
            raise AssertionError('wire attributes differ from the actual pre-register snapshot hash')
        if len(txt['raw']) != len(txt_bytes(expected)) or not txt['cls'] & 0x8000 or not srv['cls'] & 0x8000:
            raise AssertionError('full binary TXT and unique cache-flush')
        if (key(srv['target']), 1 if self.args.family == 4 else 28, self.address) not in self.records: return False
        if connect:
            with socket.socket(self.af, socket.SOCK_STREAM) as tcp:
                tcp.settimeout(3); tcp.connect(self.endpoint(slot['port']))
                tcp.sendall(('NSD_CONNECT ' + self.args.type[3:15] + '\n').encode())
                response = b''
                while not response.endswith(b'\n') and len(response) < 128:
                    piece = tcp.recv(128-len(response))
                    if not piece: break
                    response += piece
                if response != ('NSD_REGISTERED ' + self.args.type[3:15] + ' ' + str(slot['epoch']) + '\n').encode():
                    raise AssertionError('advertised TCP listener/epoch mismatch')
        return True

    def closed_listener(self, port):
        try:
            with socket.socket(self.af, socket.SOCK_STREAM) as tcp:
                tcp.settimeout(1); tcp.connect(self.endpoint(port))
            return False
        except ConnectionRefusedError: return True
        except TimeoutError: return False  # Silence is not proof of closure.

    def ack(self, serial, command):
        self.serial = serial
        self.steps.append(dict(serial=serial, command=command, seconds=time.monotonic()-self.start))
        atomic(self.args.control / 'peer-ack.json', dict(serial=serial, command=command, ok=True))

    def begin(self, serial, command):
        state = self.state()
        if command == 'quit':
            self.ack(serial, command); return False
        if state is None or (not state.get('primary') and command in ('positive', 'arm-reuse', 'arm-withdraw')):
            raise AssertionError('installed application state not ready')
        if command == 'arm-reuse':
            self.reuse_guard = key((state['primary']['name'].encode('utf-8'),) + self.service_type)
            self.reuse_seen = None; self.old_port = state['primary']['port']; self.ack(serial, command)
        elif command in ('arm-withdraw', 'arm-finish'):
            slots = [state.get('primary')] if command == 'arm-withdraw' else [state.get('primary'), state.get('sibling')]
            slots = [s for s in slots if s is not None]
            self.exit_names = [key((s['name'].encode('utf-8'),) + self.service_type) for s in slots]
            self.exit_ports = [s['port'] for s in slots]
            for owner in self.exit_names: self.goodbyes[owner] = []
            self.reuse_guard = None; self.ack(serial, command)
        elif command in ('positive', 'reuse', 'withdraw', 'retired'):
            self.pending = dict(serial=serial, command=command, start=time.monotonic(),
                                until=time.monotonic()+25, stage=0)
        else: raise AssertionError('unknown peer command')
        return True

    def progress(self):
        job = self.pending
        if job is None: return
        now = time.monotonic()
        if now >= job['until']: raise AssertionError('fixture deadline: %s stage %d' % (job['command'], job['stage']))
        state = self.state()
        if state is None: return
        command = job['command']; complete = False
        if command == 'positive':
            slot = state['primary']; owner = key((slot['name'].encode('utf-8'),) + self.service_type)
            if job['stage'] == 0 and self.verify(slot) and (not state.get('sibling') or self.verify(state['sibling'])):
                if owner not in self.unsolicited or len(self.probes.get(owner, [])) < 3: return
                if self.args.mode == 'conflict' and (slot['name'] == self.args.instance or self.conflicts_sent == 0):
                    raise AssertionError('owned answer conflict did not rename the instance')
                self.verify(slot, True)
                if state.get('sibling'): self.verify(state['sibling'], True)
                self.query_started = True; self.legacy_pending = (0x4711, owner, 33)
                self.send(self.legacy, query(owner, 33, ident=0x4711)); job['stage'] = 1
            elif job['stage'] in (1, 2) and self.legacy_answer:
                data, result, meta = self.legacy_answer; ident, expected_owner, kind = self.legacy_pending
                if len(data) > 512 or meta['destination'] != self.address or meta['port'] != 5353:
                    raise AssertionError('legacy size/destination/source')
                if result['questions'] != [(expected_owner, kind, 1)] or result['id'] != ident:
                    raise AssertionError('legacy ID/question echo')
                if any(r['cls'] & 0x8000 or r['ttl'] > 10 for r in result['records']):
                    raise AssertionError('legacy class/TTL conversion')
                answers = [r for r in result['records'] if r['section'] == 'answer' and r['type'] == kind]
                if kind == 33:
                    if not answers or result['flags'] & 0x200 or answers[0]['port'] != slot['port']:
                        raise AssertionError('complete legacy SRV answer')
                    self.steps.append(dict(legacySrvBytes=len(data), wireHex=data.hex()))
                    self.legacy_answer = None; self.legacy_pending = (0x4712, owner, 16)
                    self.send(self.legacy, query(owner, 16, ident=0x4712)); job['stage'] = 2
                else:
                    if self.args.mode == 'public':
                        if not answers or result['flags'] & 0x200 or answers[0]['attributes'] != public_attributes():
                            raise AssertionError('complete small legacy TXT')
                    elif answers or not result['flags'] & 0x200: raise AssertionError('oversized legacy TXT omission/TC')
                    self.steps.append(dict(legacyTxtBytes=len(data), wireHex=data.hex(), truncated=bool(result['flags'] & 0x200)))
                    self.legacy_answer = None; self.legacy_pending = None; self.qu_pending = owner
                    self.send(self.sock, query(owner, 33, qu=True)); job['stage'] = 3
            elif job['stage'] == 3 and self.qu_answer:
                data, result, meta = self.qu_answer
                if result['questions'] or meta['destination'] == self.group: raise AssertionError('QU framing/unicast')
                self.steps.append(dict(quBytes=len(data), wireHex=data.hex(),
                                       actualDestination=socket.inet_ntop(self.af, meta['destination'])))
                self.qu_pending = None; complete = True
        elif command == 'reuse':
            if state.get('primary') and state['primary']['epoch'] == 2 and self.verify(state['primary']):
                if self.reuse_seen is not None and now-self.reuse_seen >= 5.0:
                    self.verify(state['primary'], True)
                    if not self.closed_listener(self.old_port): raise AssertionError('old TCP listener remains open')
                    complete = True
        elif command in ('withdraw', 'retired'):
            if self.exit_names and all(len(self.goodbyes.get(n, [])) >= 3 for n in self.exit_names):
                if not all(self.closed_listener(port) for port in self.exit_ports): return
                if command == 'withdraw':
                    if not state.get('sibling') or not self.verify(state['sibling'], True): return
                    sibling = key((state['sibling']['name'].encode('utf-8'),) + self.service_type)
                    if self.goodbyes.get(sibling): raise AssertionError('surviving shared-host service withdrawn')
                    if job['stage'] == 0:
                        self.records.pop((sibling, 33), None); self.send(self.sock, query(sibling, 33))
                        job['stage'] = 1; return
                complete = True
        if complete:
            self.ack(job['serial'], command); self.pending = None

    def run(self):
        self.args.control.mkdir(parents=True, exist_ok=True)
        expected = public_attributes() if self.args.mode == 'public' else attributes(1)
        policy = sum(len(k) + 2 + (len(v) if v is not None else 0) for k,v in expected.items())
        raw_size = len(txt_bytes(expected))
        atomic(self.args.control / 'peer-ready.json', dict(pid=os.getpid(), family=self.args.family,
                index=self.args.index, address=self.args.address, mtu=self.args.mtu,
                transport='Windows-WSARecvMsg', attributePolicyBytes=policy, rawTxtBytes=raw_size, layout=windows_layout()))
        deadline = time.monotonic()+200
        try:
            running = True
            while running:
                if (self.args.control / 'peer-stop').exists():
                    raise AssertionError('runner requested cleanup before all peer checks completed')
                if time.monotonic() >= deadline: raise AssertionError('200-second peer fixture deadline')
                for ready in select.select(list(self.receivers), [], [], .02)[0]: self.consume(ready)
                request = self.args.control / 'peer-command'
                if self.pending is None and request.exists():
                    values = request.read_text(encoding='utf-8').strip().split(' ', 1)
                    if len(values) != 2: raise AssertionError('peer command framing')
                    if int(values[0]) > self.serial: running = self.begin(int(values[0]), values[1])
                self.progress()
        except BaseException as error:
            self.failure = repr(error); traceback.print_exc(); raise
        finally:
            self.sock.close(); self.legacy.close()
            if self.conflict_server: self.conflict_server.close()
            display = lambda k: '.'.join(x.decode('utf-8') for x in k)
            atomic(self.args.control / 'peer-receipt.json', dict(family=self.args.family,
                index=self.args.index, address=self.args.address, mode=self.args.mode, actualMtu=self.args.mtu,
                steps=self.steps, error=self.failure, socketCleanup=True, samples=self.samples,
                firstTxtValues=self.txt_observed, binarySeeds=self.seeds,
                largeTxtReceived=self.large_txt, fragmentedSingleRecordObserved=self.fragmented,
                conflictAnswersSent=self.conflicts_sent, probes={display(k): len(v) for k,v in self.probes.items()},
                goodbyes={display(k): len(v) for k,v in self.goodbyes.items()}, attributePolicyBytes=policy, rawTxtBytes=raw_size))


def self_test():
    owner = ('Owned.שלום\\😀', '_gr0123456789ab', '_tcp', 'local'); host = ('owned-host', 'local')
    raw = txt_bytes(attributes(1)); assert len(raw) == 1299 and txt_values(raw) == attributes(1)
    srv = record(owner, 33, struct.pack('!HHH', 0, 0, 43123) + name(host))
    wire = struct.pack('!6H', 0, 0x8400, 0, 2, 0, 1) + srv + record(owner, 16, raw) + record(host, 1, socket.inet_aton('127.0.0.1'))
    assert decode(wire)['records'][1]['attributes'] == attributes(1) and len(wire) > 1300
    for qu in (False, True): assert decode(query(owner, 33, qu))['questions'][0][2] == (0x8001 if qu else 1)
    text = record(owner, 16, raw)
    compressed = struct.pack('!6H', 0, 0x8400, 0, 2, 0, 0) + text + b'\xc0\x0c' + struct.pack('!HHIH', 16, 0x8001, 120, len(raw)) + raw
    assert decode(compressed)['records'][0]['owner'] == decode(compressed)['records'][1]['owner']
    layout = windows_layout()
    if layout['pointer'] == 8:
        assert layout == dict(pointer=8, WSABUF=16, WSAMSG=56, buffersOffset=16, controlOffset=32, flagsOffset=48, GUID=16)
    print(json.dumps(dict(validFixtureChecks=8, attributePolicyBytes=1300, rawTxtBytes=len(raw),
         dnsBytes=len(wire), wireSha256=hashlib.sha256(wire).hexdigest(), windowsLayout=layout, windowsExecution=False), indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--self-test', action='store_true')
    p.add_argument('--control', type=Path); p.add_argument('--type'); p.add_argument('--instance'); p.add_argument('--address')
    p.add_argument('--family', type=int, choices=[4, 6]); p.add_argument('--index', type=int); p.add_argument('--mtu', type=int)
    p.add_argument('--mode', choices=['public', 'reuse', 'death', 'uninstall', 'conflict']); args = p.parse_args()
    if args.self_test: self_test(); return
    if not all([args.control, args.type, args.instance, args.address, args.family, args.index, args.mtu, args.mode]):
        p.error('all owned Windows peer parameters are required')
    if not re.fullmatch(r'_gr[0-9a-f]{12}\._tcp', args.type): p.error('a unique random owned service type is required')
    Peer(args).run()


if __name__ == '__main__': main()
