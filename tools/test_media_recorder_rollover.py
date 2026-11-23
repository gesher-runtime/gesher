#!/usr/bin/env python3
"""Exercise production Recorder workers/MP4 rollover and independently decode outputs.

The existing capture/codec/Surface host controls use explicit pre-encoded FFmpeg
packets. This validates the actual Recorder, muxer, fd ownership and callbacks;
it is not a Windows hardware encoder, EGL or ART execution claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'tests/media/recorder-host'


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, timeout=120, **kwargs)


def aac_units(data: bytes) -> list[bytes]:
    units = []
    at = 0
    while at < len(data):
        length = ((data[at + 3] & 3) << 11) | (data[at + 4] << 3) | (data[at + 5] >> 5)
        assert length > 7 and at + length <= len(data)
        units.append(data[at + 7:at + length])
        at += length
    return units


def decode(path: Path) -> None:
    # Retain the demuxer's actual clock in the null output. The mixed-worker
    # fixture intentionally returns two packets 1 ms apart in one batch;
    # rounding those to a guessed display frame rate would duplicate DTS.
    result = run(['ffmpeg', '-v', 'error', '-xerror', '-i', path, '-vsync', '0',
                  '-enc_time_base', '-1', '-f', 'null', '-'],
                 capture_output=True, text=True)
    assert not result.stderr, (path, result.stderr)


def inspect(path: Path) -> tuple[dict, list[dict]]:
    data = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-show_streams', '-show_packets', '-show_data_hash', 'sha256',
        '-of', 'json', str(path)], text=True, timeout=30))
    decode(path)
    assert len(data['streams']) == 1, path
    stream, packets = data['streams'][0], data['packets']
    assert packets and float(packets[0]['pts_time']) == 0, (path, packets[:1])
    pts = [int(p['pts']) for p in packets]
    assert all(a < b for a, b in zip(pts, pts[1:])), (path, pts)
    assert path.stat().st_size < 8192, (path, path.stat().st_size)
    return stream, packets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--output-dir', type=Path, help='Retain outputs and the compiled host probe')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gesher-rollover-') as temporary:
        work = args.output_dir.resolve() if args.output_dir else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        audio, avc = work / 'tone.aac', work / 'two-frames.h264'
        silence = work / 'silence.aac'
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=750:sample_rate=48000',
             '-t', '1', '-c:a', 'aac', '-b:a', '96000', '-f', 'adts', audio])
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'anullsrc=sample_rate=48000:channel_layout=mono',
             '-t', '0.4', '-c:a', 'aac', '-b:a', '96000', '-f', 'adts', silence])
        run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'color=red:size=32x32:rate=30',
             '-frames:v', '2', '-c:v', 'libx264', '-g', '30', '-keyint_min', '30',
             '-x264-params', 'aud=1:repeat-headers=1:bframes=0', '-f', 'h264', avc])
        encoded = avc.read_bytes()
        starts = [match.start() for match in re.finditer(b'\x00\x00(?:\x00)?\x01\x09', encoded)]
        assert len(starts) == 2 and starts[0] == 0, starts
        key, dependent = work / 'key.h264', work / 'dependent.h264'
        key.write_bytes(encoded[:starts[1]])
        dependent.write_bytes(encoded[starts[1]:])

        # Reuse exactly the declared backend controls already used by the
        # lifecycle/JNI state gates, without compiling their separate main.
        host = (FIXTURE / 'lifecycle.cc').read_text().split('struct Events {')[0]
        source = work / 'rollover-host.cc'
        source.write_text('#include <dirent.h>\n#include <cerrno>\n#include <cstring>\n'
                          + host + (FIXTURE / 'rollover.cc').read_text())
        media = ROOT / 'compat/media'
        production = [media / 'record/recorder.cc', media / 'mux/mp4_muxer.cc',
                      media / 'mux/ogg_muxer.cc', media / 'encode/codec_config.cc']
        for path in production[:2]:
            print(str(path.relative_to(ROOT)) + ' SHA256 ' + hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
        command = shlex.split(args.cxx) + [
            '-std=c++20', '-O1', '-g', '-pthread', '-fno-exceptions', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-DGESHER_MEDIA_STANDALONE', '-I', ROOT, '-I', FIXTURE / 'include',
            source, *production, '-Wl,--wrap=pthread_cond_clockwait', '-Wl,--wrap=pthread_mutex_lock',
            '-Wl,--wrap=write', '-o', work / 'rollover']
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        run(command)
        result = subprocess.run([str(work / 'rollover'), str(audio), str(key), str(dependent), str(silence), str(work)],
                                capture_output=True, text=True, timeout=120)
        print(result.stdout, end='')
        print(result.stderr, end='')
        (work / 'host.log').write_text(result.stdout + result.stderr)
        result.check_returncode()

        units = aac_units(audio.read_bytes())
        kept = 0
        position = 0
        for i in range(3):
            stream, packets = inspect(work / f'audio-{i}.mp4')
            assert (stream['codec_name'], stream['sample_rate'], stream['channels']) == ('aac', '48000', 1)
            for packet in packets:
                expected = hashlib.sha256(units[position % len(units)]).hexdigest()
                assert packet['data_hash'].lower() == 'sha256:' + expected, (i, position, packet)
                position += 1
            kept += len(packets)
            position += 1  # Android discards the sample which crosses a segment's limit.
        encoded_count = int(re.search(r'AUDIO_ROLLOVER encoded=(\d+)', result.stdout)[1])
        assert encoded_count == kept + 3, (encoded_count, kept)

        video_kept = 0
        for i in range(3):
            stream, packets = inspect(work / f'video-{i}.mp4')
            assert (stream['codec_name'], stream['width'], stream['height']) == ('h264', 32, 32)
            assert 'K' in packets[0]['flags'] and all('K' in packet['flags'] for packet in packets)
            assert any(abs(side.get('rotation', 0)) == 90 for side in stream.get('side_data_list', [])), stream
            pixels = run(['ffmpeg', '-v', 'error', '-xerror', '-i', work / f'video-{i}.mp4',
                          '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], capture_output=True)
            assert not pixels.stderr, pixels.stderr
            raw = pixels.stdout
            assert len(raw) == len(packets) * 32 * 32 * 3
            assert all(raw[j] > 200 and raw[j + 1] < 30 and raw[j + 2] < 30 for j in range(0, len(raw), 3))
            video_kept += len(packets)
        encoded_count = int(re.search(r'VIDEO_ROLLOVER encoded=(\d+)', result.stdout)[1])
        assert encoded_count == video_kept + 4 + 3, (encoded_count, video_kept)
        for name in ('grace-0.mp4', 'grace-1.mp4', 'grace-stop.mp4', 'failure-old.mp4', 'callback-stop.mp4'):
            inspect(work / name)
        for i in range(2):
            path = work / f'mixed-{i}.mp4'
            details = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_packets',
                '-show_streams', '-show_packets', '-of', 'json', str(path)], text=True, timeout=30))
            streams = details['streams']
            assert {s['codec_name'] for s in streams} == {'aac', 'h264'}
            assert all(int(s['nb_read_packets']) > 0 for s in streams)
            for stream in streams:
                pts = [int(p['pts']) for p in details['packets'] if p['stream_index'] == stream['index']]
                assert pts[0] >= 0 and all(a < b for a, b in zip(pts, pts[1:])), (path, pts)
            assert path.stat().st_size < 8192
            decode(path)
        quiet = aac_units(silence.read_bytes())
        for i, expected_units in enumerate((units[:5], quiet[:6])):
            path = work / f'duration-rollover-{i}.mp4'
            stream, packets = inspect(path)
            assert len(packets) == len(expected_units)
            for packet, source_unit in zip(packets, expected_units):
                assert packet['data_hash'].lower() == 'sha256:' + hashlib.sha256(source_unit).hexdigest()
            assert path.stat().st_size < 2048
        print(f'Independent FFmpeg: 15 finalized MP4 files decoded; {kept + 11} AAC packets match source bytes; '
              f'{video_kept} H.264 frames preserve real key boundaries, pixels and orientation')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
