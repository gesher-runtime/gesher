"""Independent CI-only Ogg packet boundaries versus ffprobe source hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def packets(data):
    cursor = 0
    pending = bytearray()
    result = []
    sequence = 0
    serial = None
    saw_eos = False
    while cursor < len(data):
        if saw_eos or len(data) - cursor < 27 or data[cursor:cursor + 5] != b'OggS\0':
            raise ValueError('trailing/truncated/invalid Ogg page')
        flags = data[cursor + 5]
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
    if not saw_eos or pending:
        raise ValueError('missing EOS or unfinished packet')
    return result, sequence


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        expected = json.loads(args.fixture.read_text(encoding='utf-8'))['packets']
        actual, pages = packets(args.output.read_bytes())
        if len(actual) < 2 or not actual[0].startswith(b'OpusHead') or not actual[1].startswith(b'OpusTags'):
            raise ValueError('missing independent Opus header packet boundaries')
        audio = actual[2:]
        empty = sum(not packet for packet in audio)
        matching = sum(len(packet) == source['size'] and hashlib.sha256(packet).hexdigest() == source['sha256']
                       for packet, source in zip(audio, expected))
        ok = len(audio) == len(expected) and empty == 0 and matching == len(expected)
        print(f'media4 muxer Ogg packet trace: pages {pages}; audio {len(audio)} expected {len(expected)}; '
              f'empty {empty}; matching payload size/hash/order {matching}/{len(expected)}')
        print(f'media4 muxer Ogg packet verification: {0 if ok else 1} failure(s)')
        return 0 if ok else 1
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'media4 muxer Ogg packet verification: 1 failure(s): {error}')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
