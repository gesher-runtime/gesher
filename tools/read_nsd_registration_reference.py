#!/usr/bin/env python3
"""Decode captured owned-service reference observations offline; never use a device."""
import argparse
import base64
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import struct


def sha(data):
    return hashlib.sha256(data).hexdigest()


def lower(labels):
    return [label.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ',
                                        'abcdefghijklmnopqrstuvwxyz')) for label in labels]


def decode(data):
    assert len(data) >= 12
    ident, flags, qn, an, nn, rn = struct.unpack_from('!6H', data)
    assert qn + an + nn + rn <= 4096

    def name(start):
        position, extent, labels, seen = start, None, [], set()
        while True:
            assert position < len(data) and position not in seen
            seen.add(position)
            size = data[position]
            position += 1
            if size & 0xc0 == 0xc0:
                assert position < len(data)
                if extent is None:
                    extent = position + 1
                position = ((size & 63) << 8) | data[position]
                continue
            assert size <= 63 and position + size <= len(data)
            if not size:
                return labels, extent if extent is not None else position
            labels.append(data[position:position + size].decode('utf-8'))
            position += size
            assert len(labels) <= 255

    at, questions, records = 12, [], []
    for _ in range(qn):
        owner, at = name(at)
        kind, clazz = struct.unpack_from('!HH', data, at)
        at += 4
        questions.append({'owner': owner, 'type': kind, 'class': clazz})
    for section, count in [('answer', an), ('authority', nn), ('additional', rn)]:
        for _ in range(count):
            owner, at = name(at)
            kind, clazz, ttl, length = struct.unpack_from('!HHIH', data, at)
            at += 10
            end = at + length
            assert end <= len(data)
            raw = data[at:end]
            record = {'owner': owner, 'type': kind, 'class': clazz,
                      'cache_flush': bool(clazz & 0x8000), 'ttl': ttl,
                      'rdata_bytes': length, 'rdata_hex': raw.hex(), 'section': section}
            if kind == 33:
                assert length >= 7
                priority, weight, port = struct.unpack_from('!HHH', data, at)
                target, finish = name(at + 6)
                assert finish == end
                record.update(priority=priority, weight=weight, port=port, target=target)
            elif kind == 12:
                target, finish = name(at)
                assert finish == end
                record['target'] = target
            elif kind == 16:
                cursor, values, entries = 0, {}, []
                while cursor < len(raw):
                    size = raw[cursor]
                    cursor += 1
                    entry = raw[cursor:cursor + size]
                    assert len(entry) == size
                    cursor += size
                    key, separator, value = entry.partition(b'=')
                    key = key.decode('ascii')
                    encoded = value.hex() if separator else None
                    entries.append({'key': key, 'value_hex': encoded, 'bytes': size})
                    if key not in values:
                        values[key] = encoded
                record['txt_entries'] = entries
                record['txt_map'] = values
            elif kind in (1, 28):
                assert length == (4 if kind == 1 else 16)
                record['address'] = str(ipaddress.ip_address(raw))
            records.append(record)
            at = end
    assert at == len(data), 'Unparsed DNS trailing data'
    return {'id': ident, 'flags': flags, 'response': bool(flags & 0x8000),
            'truncated': bool(flags & 0x0200), 'questions': questions,
            'records': records, 'bytes': len(data), 'sha256': sha(data)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capsule', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads((args.capsule / 'manifest.json').read_text())
    assert sha(Path(manifest['source']['snapshot']).read_bytes()) == manifest['source']['sha256']
    assert sha(Path(manifest['payload']['path']).read_bytes()) == manifest['payload']['sha256']
    receipt = json.loads(args.receipt.read_text())
    assert receipt['requested_steps'] == 1 and len(receipt['results']) == 1
    result = receipt['results'][0]
    output = result['output']
    observations = []
    for line in output.splitlines():
        match = re.fullmatch(r'NSD_REGISTRATION_REFERENCE_([A-Z]+) (\{.*\})', line)
        if match:
            observations.append({'kind': match[1], 'value': json.loads(match[2])})
    grouped = {}
    for event in observations:
        grouped.setdefault(event['kind'], []).append(event['value'])
    profile, = grouped['PROFILE']
    scope, = grouped['SCOPE']
    multicast_profile = profile.get('peerMode') == 'multicast-5353-fresh-owned-wire'
    if multicast_profile:
        assert manifest['peerProfile'] == profile['peerMode']
        assert profile['periodicAnnouncementsCanSupplyRecords'] and not profile['legacyUnicastQualified']
    assert profile['sdk'] == 36 and profile['uid'] == 2000
    assert profile['context'] == 'app_process-system-context' and not profile['ordinaryApkPermissions']
    assert profile['nsdLoader'] != profile['referenceLoader']
    assert re.fullmatch(r'_gr[0-9a-f]{10}\._tcp', scope['type'])
    nonce = scope['type'][3:-5]
    expected_txt = {'flag': None, 'empty': '',
                    'binary': 'nul\0equals=שלום😀'.encode('utf-8').hex(),
                    'owned': nonce.encode('ascii').hex()}
    requests = {v['registration']: v for v in grouped.get('REQUEST', [])}
    for request in requests.values():
        assert request['attributeHex'] == expected_txt
        assert request['network'] == scope['network']
        assert 0 < request['port'] <= 65535
    wires = []
    for raw in grouped.get('WIRE', []):
        data = base64.b64decode(raw['base64'], validate=True)
        assert len(data) == raw['bytes'] and sha(data) == raw['sha256']
        assert raw['source'] == scope['address'] and raw['sourcePort'] == 5353
        packet = decode(data)
        assert (packet['id'], packet['flags'], len(packet['questions'])) == (
            raw['id'], raw['flags'], raw['questions'])
        packet.update(sequence=raw['sequence'], channel=raw['channel'], elapsed_ms=raw['elapsedMs'])
        if multicast_profile:
            packet['received_elapsed_ms'] = raw['receivedElapsedMs']
        wires.append(packet)
    queries = []
    for raw in grouped.get('QUERY', []):
        packet = decode(base64.b64decode(raw['base64'], validate=True))
        assert not packet['response'] and not packet['records']
        assert packet['id'] == raw['id'] and 0 < raw['sourcePort'] <= 65535
        if multicast_profile:
            assert raw['sourcePort'] == 5353 and packet['id'] == 0
            assert raw['responseCausalityEstablished'] is False
            packet.update(scenario=raw['scenario'], after_sequence=raw['afterSequence'],
                          freshness_boundary_ms=raw['freshnessBoundaryMs'])
        else:
            assert raw['sourcePort'] != 5353
        assert [q['type'] for q in packet['questions']] == raw['types']
        assert all(q['owner'] == raw['ownerLabels'] and q['class'] == 1 for q in packet['questions'])
        packet['source_port'] = raw['sourcePort']
        queries.append(packet)
    callbacks = grouped.get('CALLBACK', [])
    registered = {v['registration']: v for v in callbacks if v['event'] == 'registered'}
    checks = [(int(n), text) for n, text in re.findall(
        r'^NSD_REGISTRATION_REFERENCE_CHECK (\d+) (.*)$', output, re.M)]
    assert [n for n, _ in checks] == list(range(1, len(checks) + 1))
    successful = result['exit_code'] == 0
    endpoint_results = []
    if successful:
        assert set(requests) == set(registered) == {'first-default-executor', 'second-explicit-executor', 'replacement'}
        assert registered['first-default-executor']['name'] != registered['second-explicit-executor']['name']
        assert registered['second-explicit-executor']['thread'] == 'nsd-reference-registration-executor'
        for label in requests:
            assert len([c for c in callbacks if c['registration'] == label and c['event'] == 'registered']) == 1
            assert len([c for c in callbacks if c['registration'] == label and c['event'] == 'unregistered']) == 1
        assert all(c['error'] is None for c in callbacks)
        assert len(grouped['ENDPOINT']) == 4
        for endpoint in grouped['ENDPOINT']:
            label = endpoint['registration']
            owner = [registered[label]['name'], scope['type'].split('.')[0], '_tcp', 'local']
            if multicast_profile:
                packet_ids = endpoint['packetSequences']
                assert packet_ids and len(set(packet_ids)) == len(packet_ids)
                selected = [p for p in wires if p['sequence'] in packet_ids]
                assert len(selected) == len(packet_ids)
                windows = [q for q in queries if q['scenario'] in (endpoint['scenario'], endpoint['scenario'] + 0x100)]
                assert windows and endpoint['responseCausalityEstablished'] is False
                for packet in selected:
                    assert packet['channel'] == 'multicast-observer' and packet['response']
                    assert packet['elapsed_ms'] <= endpoint['elapsedMs']
                    assert any(packet['sequence'] > q['after_sequence'] and
                               packet['received_elapsed_ms'] >= q['freshness_boundary_ms'] for q in windows)
            else:
                selected = [p for p in wires if p['channel'] == 'legacy-unicast-peer']
            answers = [r for p in selected if p['response']
                       for r in p['records'] if r['ttl'] > 0 and lower(r['owner']) == lower(owner)]
            srvs = [r for r in answers if r['type'] == 33]
            txts = [r for r in answers if r['type'] == 16]
            assert any(r['port'] == requests[label]['port'] and r['target'] == endpoint['observedHostLabels'] for r in srvs)
            assert any(r['txt_map'] == expected_txt for r in txts)
            assert endpoint['observedAddress'] == scope['address']
            assert endpoint['endpointAddressSource'] == 'observed-owned-service-DNS-A-record'
            assert any(r['type'] == 1 and r['ttl'] > 0 and lower(r['owner']) == lower(endpoint['observedHostLabels'])
                       and r['address'] == scope['address'] for p in selected for r in p['records'])
            endpoint_results.append({'registration': label, 'observed_port': requests[label]['port'],
                                     'observed_address': scope['address'], 'host_labels': endpoint['observedHostLabels'],
                                     'srv_txt_address_from_decoded_wire': True})
        assert f'NsdRegistrationAndroidReference all passed: {len(checks)}; stockReference=true; windowsExecution=false; ordinaryApkPermissions=false' in output
    report = {'manifest_sha256': sha((args.capsule / 'manifest.json').read_bytes()),
              'source_sha256': manifest['source']['sha256'], 'payload_sha256': manifest['payload']['sha256'],
              'reader_sha256': sha(Path(__file__).read_bytes()), 'raw_receipt': str(args.receipt),
              'raw_receipt_sha256': sha(args.receipt.read_bytes()), 'exit_code': result['exit_code'],
              'seconds': result['seconds'], 'successful': successful, 'checks': checks,
              'profile': profile, 'scope': scope, 'requests': requests, 'callbacks': callbacks,
              'queries': queries, 'wire_packets': wires, 'endpoint_checks': endpoint_results,
              'peer_profile': profile.get('peerMode', 'legacy-unicast'),
              'causal_query_response_qualified': False,
              'legacy_unicast_qualified': successful and not multicast_profile,
              'withdrawal_observation': grouped.get('WITHDRAWAL', []),
              'source_provider_replaced': False, 'ordinary_apk_permission_qualification': False,
              'windows_execution': False, 'source_profile_36_1_execution': False,
              'offline_decoder_device_operations': False}
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({'successful': successful, 'checks': len(checks), 'wire_packets': len(wires),
                      'queries': len(queries), 'endpoint_checks': len(endpoint_results),
                      'output': str(args.output), 'output_sha256': sha(args.output.read_bytes())}))


if __name__ == '__main__':
    main()
