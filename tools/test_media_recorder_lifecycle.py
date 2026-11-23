#!/usr/bin/env python3
"""Run the production MediaRecorder worker and MP4 writer on deterministic host inputs."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/media/recorder-host"


def run(args):
    return subprocess.run(args, check=True, timeout=60)


def inspect(path):
    output = subprocess.check_output([
        "ffprobe", "-v", "error", "-count_packets", "-show_streams", "-of", "json", path,
    ], text=True, timeout=30)
    run(["ffmpeg", "-v", "error", "-xerror", "-i", path, "-f", "null", "-"])
    return {track["codec_type"]: track for track in json.loads(output)["streams"]}


def audio_result(track):
    assert (track["codec_name"], track["sample_rate"], track["channels"]) == ("aac", "48000", 1)
    return int(track["nb_read_packets"]), float(track["duration"])


with tempfile.TemporaryDirectory(prefix="gesher-recorder-") as temporary:
    work = Path(temporary)
    binary = work / "recorder-lifecycle"
    audio = work / "tone.aac"
    video = work / "frame.h264"
    run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=750:sample_rate=48000",
         "-t", "1", "-c:a", "aac", "-b:a", "96000", "-f", "adts", audio])
    run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=red:size=32x32",
         "-frames:v", "1", "-c:v", "libx264", "-f", "h264", video])
    media = ROOT / "compat/media"
    run([os.getenv("CXX", "c++"), "-std=c++20", "-O1", "-g", "-pthread", "-fno-exceptions",
         "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-DGESHER_MEDIA_STANDALONE",
         "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", ROOT, "-I", FIXTURE / "include",
         FIXTURE / "lifecycle.cc", media / "record/recorder.cc", media / "mux/mp4_muxer.cc",
         media / "mux/ogg_muxer.cc", media / "encode/codec_config.cc", "-o", binary])
    run([binary, audio, video, work])
    packets, duration = audio_result(inspect(work / "duration.m4a.ready")["audio"])
    assert packets == 6 and 0.11 < duration < 0.14, (packets, duration)
    packets, duration = audio_result(inspect(work / "size.m4a")["audio"])
    assert 2 <= packets <= 8 and 0.03 < duration < 0.18, (packets, duration)
    tracks = inspect(work / "mixed.mp4")
    assert audio_result(tracks["audio"])[0] == 3
    video = tracks["video"]
    assert (video["codec_name"], int(video["nb_read_packets"])) == ("h264", 2)
    print("Finalized duration, size and mixed-source outputs independently demuxed and decoded")
