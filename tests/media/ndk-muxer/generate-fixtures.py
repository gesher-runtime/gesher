#!/usr/bin/env python3
"""CI only: independent encoded packets for the NDK muxer contract fixtures."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def dump_bytes(value):
    result = bytearray()
    for line in value.splitlines():
        if not line.strip():
            continue
        address, payload = line.split(': ', 1)
        int(address, 16)
        # ffprobe prints sixteen bytes as eight groups of four hex digits,
        # then a separate ASCII column. Never interpret ASCII as packet data.
        result.extend(bytes.fromhex(payload[:39]))
    return bytes(result)


def avc_parameters(data):
    if len(data) < 7 or data[0] != 1 or data[4] & 3 != 3:
        raise ValueError('Expected avcC with four-byte packet NAL lengths')
    at = 6
    result = bytearray()
    counts = [data[5] & 31]
    for group in range(2):
        for _ in range(counts[group]):
            size = struct.unpack_from('>H', data, at)[0]
            at += 2
            if size == 0 or at + size > len(data):
                raise ValueError('Truncated avcC parameter set')
            result.extend(b'\0\0\0\1' + data[at:at + size])
            at += size
        if group == 0:
            counts.append(data[at])
            at += 1
    return bytes(result)


def vorbis_headers(data):
    if not data or data[0] != 2:
        raise ValueError('Expected three Xiph-laced Vorbis headers')
    at = 1
    lengths = []
    for _ in range(2):
        size = 0
        while True:
            value = data[at]
            at += 1
            size += value
            if value < 255:
                break
        lengths.append(size)
    lengths.append(len(data) - at - sum(lengths))
    result = []
    for size, prefix in zip(lengths, [b'\x01vorbis', b'\x03vorbis', b'\x05vorbis']):
        header = data[at:at + size]
        if size <= 0 or len(header) != size or not header.startswith(prefix):
            raise ValueError('Invalid Vorbis codec header')
        result.append(header)
        at += size
    return result


def fields(stream):
    codec = stream['codec_name']
    mime = {'h264': 'video/avc', 'aac': 'audio/mp4a-latm', 'vp9': 'video/x-vnd.on2.vp9',
            'vp8': 'video/x-vnd.on2.vp8', 'opus': 'audio/opus', 'vorbis': 'audio/vorbis'}[codec]
    result = [('mime', 3, mime.encode())]
    for source, key in [('width', 'width'), ('height', 'height'),
                        ('sample_rate', 'sample-rate'), ('channels', 'channel-count')]:
        if source in stream:
            result.append((key, 1, struct.pack('<i', int(stream[source]))))
    extra = dump_bytes(stream.get('extradata', ''))
    if codec == 'h264':
        csd = [avc_parameters(extra)]
    elif codec == 'vorbis':
        csd = vorbis_headers(extra)
    elif codec == 'opus':
        if len(extra) < 19 or not extra.startswith(b'OpusHead'):
            raise ValueError('Missing OpusHead')
        pre_skip = struct.unpack_from('<H', extra, 10)[0]
        csd = [extra, struct.pack('<q', pre_skip * 1_000_000_000 // 48000),
               struct.pack('<q', 80_000_000)]
    elif codec == 'aac':
        if not extra:
            raise ValueError('Missing AAC AudioSpecificConfig')
        csd = [extra]
    else:
        csd = []
    result.extend((f'csd-{i}', 4, data) for i, data in enumerate(csd))
    return result


def blob(data):
    return struct.pack('<I', len(data)) + data


def encode(name, source, out, audio_only=False):
    raw = subprocess.check_output(['ffprobe', '-v', 'error', '-show_streams', '-show_packets',
                                   '-show_data', '-of', 'json', str(source)], timeout=60)
    info = json.loads(raw)
    streams = [s for s in info['streams'] if not audio_only or s['codec_type'] == 'audio']
    indexes = {s['index']: i for i, s in enumerate(streams)}
    packets = [p for p in info['packets'] if p['stream_index'] in indexes]
    times = [int(Decimal(p['pts_time']) * 1_000_000) for p in packets]
    shift = -min(0, min(times))
    result = bytearray(b'GSMUX001' + struct.pack('<I', len(streams)))
    track_audit = []
    for stream in streams:
        attributes = fields(stream)
        result.extend(struct.pack('<I', len(attributes)))
        for key, kind, value in attributes:
            result.extend(blob(key.encode()) + struct.pack('<I', kind) + blob(value))
        track_audit.append({'codec': stream['codec_name'], 'original_index': stream['index'],
                            'fields': [{'key': key, 'kind': kind, 'size': len(value),
                                        'sha256': hashlib.sha256(value).hexdigest()}
                                       for key, kind, value in attributes]})
    result.extend(struct.pack('<I', len(packets)))
    packet_audit = []
    previous = {}
    for packet, time in zip(packets, times):
        track = indexes[packet['stream_index']]
        time += shift
        if time < previous.get(track, 0):
            raise ValueError('Fixture must use presentation-ordered packets without B frames')
        previous[track] = time
        data = dump_bytes(packet['data'])
        if len(data) != int(packet['size']):
            raise ValueError('Packet dump length does not match ffprobe size')
        flags = 1 if 'K' in packet['flags'] else 0
        result.extend(struct.pack('<IqII', track, time, flags, len(data)) + data)
        packet_audit.append({'track': track, 'time_us': time, 'flags': flags, 'size': len(data),
                             'sha256': hashlib.sha256(data).hexdigest()})
    target = out / f'{name}.bin'
    target.write_bytes(result)
    audit = {'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
             'fixture_sha256': hashlib.sha256(result).hexdigest(), 'timestamp_shift_us': shift,
             'tracks': track_audit, 'packets': packet_audit}
    (out / f'{name}.json').write_text(json.dumps(audit, indent=2) + '\n', encoding='utf-8')
    print(f'muxer fixture {name}: {len(streams)} tracks, {len(packets)} packets, '
          f'{len(result)} bytes, shift {shift} us, sha256 {audit["fixture_sha256"]}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--video-assets', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    colors = ';'.join(f'color=c={color}:s=320x240:r=30:d=0.5[{letter}]'
                      for color, letter in zip(['red', 'lime', 'blue', 'white'], 'abcd'))
    colors += ';[a][b][c][d]concat=n=4:v=1:a=0,format=yuv420p[v]'
    source = args.out / 'source-h264-aac.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-filter_complex',
                    colors + ';aevalsrc=0.5*sin(2*PI*1661*t):s=44100:c=stereo:d=2[s]',
                    '-map', '[v]', '-map', '[s]', '-c:v', 'libx264', '-preset', 'veryfast',
                    '-profile:v', 'baseline', '-g', '15', '-bf', '0', '-c:a', 'aac', '-b:a', '96k',
                    str(source)], check=True, timeout=120)
    encode('h264-aac', source, args.out)
    encode('vp9-opus', args.video_assets / 'video_vp9_opus.webm', args.out)
    encode('vp8-vorbis', args.video_assets / 'video_vp8_vorbis.webm', args.out)
    encode('opus', args.video_assets / 'video_vp9_opus.webm', args.out, audio_only=True)


if __name__ == '__main__':
    main()
