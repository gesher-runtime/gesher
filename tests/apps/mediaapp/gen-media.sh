#!/usr/bin/env bash
# The test media of mediaapp.apk (docs/media.md), generated with ffmpeg: a tone
# of its own frequency per format, so the test finds each in the audio
# capture (tests/windows/tracks/38-media.ps1). Each is 0.8 s at amplitude 0.5,
# with title, artist and album tags where the format has them.
#
#   tests/apps/mediaapp/gen-media.sh     (writes res/raw/ and assets/ here)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
raw=$here/res/raw
assets=$here/assets
mkdir -p "$raw" "$assets"

tone() {  # tone <hz> <rate> <channels>
  echo "aevalsrc=0.5*sin(2*PI*$1*t):s=$2:c=$3:d=0.8"
}
tags=(-metadata title="Gesher Tone" -metadata artist="Gesher" -metadata album="Media Tests")
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# res/raw (aapt2 keeps these extensions uncompressed: openRawResourceFd works)
ff -f lavfi -i "$(tone 523 44100 stereo)" -c:a libmp3lame -b:a 128k "${tags[@]}" "$raw/tone_mp3.mp3"
ff -f lavfi -i "$(tone 659 44100 stereo)" -c:a libvorbis -q:a 5 "${tags[@]}" "$raw/tone_vorbis.ogg"
ff -f lavfi -i "$(tone 880 22050 mono)" -c:a pcm_s16le "$raw/tone_wav.wav"
# assets (stored uncompressed by build.sh)
ff -f lavfi -i "$(tone 587 44100 stereo)" -c:a aac -b:a 128k "${tags[@]}" "$assets/tone_aac.m4a"
ff -f lavfi -i "$(tone 698 48000 stereo)" -c:a libopus -b:a 96k "$assets/tone_opus.opus"
ff -f lavfi -i "$(tone 784 44100 stereo)" -c:a flac "${tags[@]}" "$assets/tone_flac.flac"
ff -f lavfi -i "$(tone 988 44100 mono)" -c:a aac -b:a 96k -f adts "$assets/tone_adts.aac"
ff -f lavfi -i "$(tone 1047 16000 mono)" -c:a pcm_mulaw -f wav "$assets/tone_ulaw.wav"
# 24-bit WAV, 2 s at 330 Hz: the seek and the loop checks.
ff -f lavfi -i "aevalsrc=0.5*sin(2*PI*330*t):s=48000:c=stereo:d=2" -c:a pcm_s24le "$assets/long_wav24.wav"
# The http check's file (served by the test): Ogg Vorbis, 1175 Hz.
ff -f lavfi -i "$(tone 1175 44100 mono)" -c:a libvorbis -q:a 4 "$assets/tone_http.ogg"
ls -l "$raw" "$assets"
