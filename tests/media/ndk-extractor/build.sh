#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1/media/ndk-extractor" && cd "$1/media/ndk-extractor" && pwd)
ndk=$(cd "$2" && pwd)
llvm=$ndk/toolchains/llvm/prebuilt/linux-x86_64
ffmpeg -v error -f lavfi -i 'testsrc2=duration=1:size=48x32:rate=4' \
  -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=1' \
  -c:v libx264 -g 2 -bf 0 -sc_threshold 0 -c:a aac -y "$out/multitrack.mp4"
python3 - "$out" <<'PY'
import pathlib
import json
import struct
import subprocess
import sys
import wave

out = pathlib.Path(sys.argv[1])
with wave.open(str(out / 'source.wav'), 'wb') as wav:
    wav.setparams((1, 2, 8000, 0, 'NONE', 'not compressed'))
    wav.writeframes(b''.join(struct.pack('<h', i % 251) for i in range(8000)))
(out / 'embedded.bin').write_bytes(b'Z' * 37 + (out / 'source.wav').read_bytes() + b'Q' * 128)
mp4 = out / 'multitrack.mp4'
data = bytearray(mp4.read_bytes())
offset = 0
while offset < len(data):
    length, kind = struct.unpack_from('>I4s', data, offset)
    assert length >= 8 and offset + length <= len(data)
    if kind == b'moov':
        assert offset + length == len(data)
        payload = bytes(4) + bytes(range(16)) + struct.pack('>I', 3) + b'xyz'
        atom = struct.pack('>I4s', 8 + len(payload), b'pssh') + payload
        struct.pack_into('>I', data, offset, length + len(atom))
        mp4.write_bytes(data + atom)
        break
    offset += length
else:
    raise SystemExit("No moov box found")

# Pin the guest assertion to the configuration in this actual generated fixture.
# FFprobe is an independent container reader; no expected CSD is made up by the
# extractor implementation or copied from its output.
streams = json.loads(subprocess.check_output([
    'ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name,extradata',
    '-show_data', '-of', 'json', str(mp4)], text=True))['streams']
dump = next(stream['extradata'] for stream in streams if stream['codec_name'] == 'aac')
csd = bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0].replace(' ', '')
                          for line in dump.splitlines() if ':' in line))
assert csd
(out / 'fixture_csd.h').write_text('#pragma once\nstatic const unsigned char kFixtureAacCsd[] = {' +
                                 ','.join(str(value) for value in csd) + '};\n')
PY
for abi in x86_64 aarch64; do
  "$llvm/bin/$abi-linux-android29-clang" -std=c17 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
    -fPIE -pie -I"$out" "$here/extractor_test.c" -lmediandk -ldl -o "$out/ndk-extractor-$abi"
done
