#!/usr/bin/env python3
"""Exercise production Playback and real Track with a controlled decoder schedule.

The full Playback class and all Track state/ring/resampler methods are compiled
unchanged apart from private visibility and observable condition-variable waits.
Scripted source/decoder/renderer, event delivery and engine attachment boundaries
make startup, seeking and EOF interleavings deterministic. This is a host logic
regression, not a substitute for the Windows codec and captured-output CI gates.
--source-ref compiles the same assertions against old source as a negative control.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import wave

from test_audio_state_contracts import generate as generate_audio, section

ROOT = Path(__file__).resolve().parents[1]


def read(path: str, source_ref: str | None) -> str:
    if source_ref:
        return subprocess.run(["git", "show", source_ref + ":" + path], cwd=ROOT,
                              check=True, capture_output=True, text=True).stdout
    return (ROOT / path).read_text(encoding="utf-8")


def generate(source_ref: str | None, control: str | None) -> str:
    audio = generate_audio("audio", source_ref)
    boundary = "\nnamespace {\nusing namespace gesher::media;"
    if audio.count(boundary) != 1:
        raise ValueError("audio fixture boundary changed")
    audio = audio[:audio.index(boundary)]
    source = read("compat/media/player/player.cc", source_ref)
    header = read("compat/media/player/player.h", source_ref)
    playback = section(source, "class Playback :", "// --- Player: libmedia MediaPlayer")
    playback = playback.replace(" private:", " public:")
    if playback.count("std::condition_variable cv_;") != 1:
        raise ValueError("Playback condition-variable boundary changed")
    playback = playback.replace("std::condition_variable cv_;", "PlayerConditionVariable cv_;")
    # Explicit compiled mutation controls isolate the additional A/V and EOF
    # guards. The old-source control needs none of the new fields or helpers.
    if control:
        before = ("audio_draining_ && track_->stopped()" if control == "eager-video"
                  else "if (audio_draining_) track_->Stop();")
        after = ("track_->stopped()" if control == "eager-video"
                 else "if (audio_draining_ && force) track_->Stop();")
        if playback.count(before) != 1:
            raise ValueError("compiled control boundary changed: " + control)
        playback = playback.replace(before, after)
        print("Compiled mutation control: " + control, flush=True)
    template = (ROOT / "tests/media/player_priming_host.cc.in").read_text(encoding="utf-8")
    for marker, body in {
        "@AUDIO_IMPLEMENTATION@": audio,
        "@PLAYER_EVENTS@": section(header, "constexpr int kMediaErrorIo", "\nclass Playback;\n"),
        "@PLAYBACK@": playback,
    }.items():
        if template.count(marker) != 1:
            raise ValueError("template marker changed: " + marker)
        template = template.replace(marker, body)
    digest = hashlib.sha256((source + header).encode()).hexdigest()
    print("Playback source SHA256 " + digest, flush=True)
    return "// Production source SHA256: " + digest + "\n" + template


def analyze(work: Path, cxx: list[str], source_ref: str | None) -> bool:
    probe = read("compat/media/probe/media_probe.cc", source_ref)
    body = section(probe, "int Analyze(", "int Wasapi()")
    source = work / "analyze.cc"
    source.write_text("#include <algorithm>\n#include <cmath>\n#include <cstdio>\n"
                      "#include <cstdint>\n#include <cstring>\n#include <vector>\n" + body +
                      "\nint main(int argc, char** argv) { return argc == 2 ? Analyze(argv[1]) : 2; }\n")
    binary = work / "analyze"
    subprocess.run(cxx + ["-std=c++20", "-O2", str(source), "-o", str(binary)],
                   check=True, timeout=120)
    wav_path = work / "startup.wav"
    with wave.open(str(wav_path), "wb") as wav:
        wav.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        wav.writeframes((work / "startup.pcm").read_bytes())
    result = subprocess.run([str(binary), str(wav_path)], check=True, capture_output=True,
                            text=True, timeout=10)
    print("Unmodified media_probe Analyze:\n" + result.stdout, end="", flush=True)
    sounds = re.findall(r"sound \d+: start [\d.]+ s, duration ([\d.]+) s, ([\d.]+) Hz, "
                        r"rms ([\d.]+) ([\d.]+)", result.stdout)
    if len(sounds) != 1:
        print("FAIL player priming: expected one captured tone", flush=True)
        return False
    duration, hz, left, right = map(float, sounds[0])
    # The existing Windows AAC tone gates, unchanged: duration +/-60 ms,
    # frequency +/-2%, RMS +/-15%. The source is PCM; AAC decoding is tested
    # by the actual Windows app/capture workflow, not modeled here.
    ok = abs(duration - 0.8) <= 0.06 and abs(hz - 587) <= 587 * 0.02
    ok &= all(abs(rms - 0.354) <= 0.354 * 0.15 for rms in (left, right))
    print(("PASS" if ok else "FAIL") + " unchanged captured-tone tolerances", flush=True)
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--control", choices=["eager-video", "resume-without-drain"])
    args = parser.parse_args()
    cxx = shlex.split(args.cxx)
    with tempfile.TemporaryDirectory(prefix="gesher-player-priming-") as temporary:
        work = Path(temporary)
        source = work / "player.cc"
        source.write_text(generate(args.source_ref, args.control), encoding="utf-8")
        binary = work / "player"
        command = cxx + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                         "-pthread", str(source), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary), str(work / "startup.pcm")], check=False, timeout=60)
        tone_ok = analyze(work, cxx, args.source_ref)
        return int(result.returncode != 0 or not tone_ok)


if __name__ == "__main__":
    raise SystemExit(main())
