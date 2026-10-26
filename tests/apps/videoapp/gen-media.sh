#!/usr/bin/env bash
# The clips of videoapp.apk (docs/media.md "Video"), generated with ffmpeg: 2 s at
# 320x240, 30 frames a second, four colors of 0.5 s each (red, lime, blue, white), so
# a decoded frame's color says which frame it is; the MP4 also has a 2 s AAC tone of
# 1661 Hz (stereo, 44100 Hz). One clip per codec; the test decodes those this
# machine's MediaCodecList has a decoder for.
#
#   tests/apps/videoapp/gen-media.sh     (writes assets/ here)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
assets=$here/assets
mkdir -p "$assets"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

colors="color=c=red:s=320x240:r=30:d=0.5[a];color=c=lime:s=320x240:r=30:d=0.5[b];"
colors+="color=c=blue:s=320x240:r=30:d=0.5[c];color=c=white:s=320x240:r=30:d=0.5[d];"
colors+="[a][b][c][d]concat=n=4:v=1:a=0,format=yuv420p[v]"
tone="aevalsrc=0.5*sin(2*PI*1661*t):s=44100:c=stereo:d=2[s]"

# H.264 (Main, B-frames, a key frame every 15 frames) and AAC in MP4.
ff -filter_complex "$colors;$tone" -map "[v]" -map "[s]" \
  -c:v libx264 -preset veryfast -profile:v main -g 15 -bf 2 -c:a aac -b:a 96k \
  -movflags +faststart "$assets/video_h264.mp4"
# The other codecs, video only.
ff -filter_complex "$colors" -map "[v]" -c:v libx265 -preset veryfast -x265-params log-level=error \
  -g 15 -tag:v hvc1 "$assets/video_hevc.mp4"
ff -filter_complex "$colors" -map "[v]" -c:v libvpx -b:v 500k -g 15 "$assets/video_vp8.webm"
ff -filter_complex "$colors" -map "[v]" -c:v libvpx-vp9 -b:v 500k -g 15 -row-mt 1 "$assets/video_vp9.webm"
ff -filter_complex "$colors" -map "[v]" -c:v libaom-av1 -cpu-used 8 -b:v 300k -g 15 "$assets/video_av1.mp4"
ls -l "$assets"
