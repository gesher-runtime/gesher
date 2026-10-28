"""Independent CI-only Ogg packet boundaries versus ffprobe source hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def page_crc(data):
    value = 0
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def packets(data):
    cursor = 0
    pending = bytearray()
    result = []
    sequence = 0
    serial = None
    saw_eos = False
    granules = []
    while cursor < len(data):
        if saw_eos or len(data) - cursor < 27 or data[cursor:cursor + 5] != b'OggS\0':
            raise ValueError('trailing/truncated/invalid Ogg page')
        flags = data[cursor + 5]
        page_granule = struct.unpack_from('<Q', data, cursor + 6)[0]
        page_serial, page_sequence = struct.unpack_from('<II', data, cursor + 14)
        if page_sequence != sequence or (serial is not None and serial != page_serial):
            raise ValueError('page sequence or stream identity mismatch')
        if bool(flags & 1) != bool(pending):
            raise ValueError('packet continuation flag mismatch')
        if sequence == 0 and not flags & 2:
            raise ValueError('first page has no BOS flag')
        serial = page_serial
        sequence += 1
        segment_count = data[cursor + 26]
        header_end = cursor + 27 + segment_count
        if header_end > len(data):
            raise ValueError('truncated lacing table')
        lacing = data[cursor + 27:header_end]
        page_end = header_end + sum(lacing)
        if page_end > len(data):
            raise ValueError('truncated page payload')
        stored_crc = struct.unpack_from('<I', data, cursor + 22)[0]
        page_bytes = bytearray(data[cursor:page_end])
        page_bytes[22:26] = bytes(4)
        if page_crc(page_bytes) != stored_crc:
            raise ValueError('independent Ogg CRC mismatch')
        payload_cursor = header_end
        for size in lacing:
            pending.extend(data[payload_cursor:payload_cursor + size])
            payload_cursor += size
            if size < 255:
                result.append(bytes(pending))
                pending.clear()
        saw_eos = bool(flags & 4)
        if saw_eos and pending:
            raise ValueError('EOS leaves an incomplete packet')
        cursor = page_end
        granules.append((page_granule, len(result)))
    if not saw_eos or pending:
        raise ValueError('missing EOS or unfinished packet')
    return result, sequence, granules


def opus_samples(packet):
    if not packet:
        raise ValueError('zero-octet Opus audio packet')
    toc = packet[0]
    if toc & 0x80:
        frame = (48000 << ((toc >> 3) & 3)) // 400
    elif toc & 0x60 == 0x60:
        frame = 960 if toc & 8 else 480
    else:
        duration = (toc >> 3) & 3
        frame = 2880 if duration == 3 else 480 << duration
    code = toc & 3
    if code == 3 and len(packet) < 2:
        raise ValueError('truncated Opus frame count')
    count = 1 if code == 0 else (packet[1] & 63 if code == 3 else 2)
    if count == 0 or frame * count > 5760:
        raise ValueError('invalid Opus packet duration')
    return frame * count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--limit', type=int)
    parser.add_argument('--decoded-frames', type=int)
    args = parser.parse_args()
    try:
        expected = json.loads(args.fixture.read_text(encoding='utf-8'))['packets']
        if args.limit is not None:
            if not 0 <= args.limit <= len(expected):
                raise ValueError('invalid source packet prefix limit')
            expected = expected[:args.limit]
        actual, pages, granules = packets(args.output.read_bytes())
        if len(actual) < 2 or len(actual[0]) < 19 or not actual[0].startswith(b'OpusHead') or not actual[1].startswith(b'OpusTags'):
            raise ValueError('missing independent Opus header packet boundaries')
        audio = actual[2:]
        empty = sum(not packet for packet in audio)
        matching = sum(len(packet) == source['size'] and hashlib.sha256(packet).hexdigest() == source['sha256']
                       for packet, source in zip(audio, expected))
        ok = len(audio) == len(expected) and empty == 0 and matching == len(expected)
        # Nonempty streams end on the page completing their final real packet.
        if audio and len(granules) > 1 and granules[-1][1] <= granules[-2][1]:
            ok = False
        print(f'media4 muxer Ogg packet trace: pages {pages}; audio {len(audio)} expected {len(expected)}; '
              f'empty {empty}; matching payload size/hash/order {matching}/{len(expected)}')
        if empty == 0:
            cumulative = [0]
            for packet in audio:
                cumulative.append(cumulative[-1] + opus_samples(packet))
            valid_granules = all(granule == cumulative[max(0, completed - 2)] for granule, completed in granules)
            pre_skip = struct.unpack_from('<H', actual[0], 10)[0]
            decoded_frames = max(0, cumulative[-1] - pre_skip)
            ok = ok and valid_granules and (args.decoded_frames is None or args.decoded_frames == decoded_frames)
            print(f'media4 muxer Ogg granule trace: encoded {cumulative[-1]} pre-skip {pre_skip} '
                  f'expected decoded {decoded_frames} actual {args.decoded_frames} valid={valid_granules}; all page CRCs valid')
        print(f'media4 muxer Ogg packet verification: {0 if ok else 1} failure(s)')
        return 0 if ok else 1
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'media4 muxer Ogg packet verification: 1 failure(s): {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
