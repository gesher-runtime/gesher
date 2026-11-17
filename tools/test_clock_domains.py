#!/usr/bin/env python3
"""Compile actual Android clock adapters and patched, pinned AOSP clock bodies.

Win32 clocks, host clock_gettime, signal waits and JNI/video transport are controlled. The
model distinguishes awake time, suspended time, QPC epoch, UTC and CPU clocks.
It is not evidence of actual Windows suspend/hibernate or device timestamps.
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

ROOT = Path(__file__).resolve().parents[1]
PINNED = {
    "SystemClock.cpp": "2e4d1d550d9bcabf52ea11a3aef4bb05928cdc0875266fdb715afb23e7803da7",
    "time_utils.cc": "57d9da8802ff4313e65aec616af020bee27f788f4a6f9104b68033217cc0bb6b",
    "Timers.cpp": "43657a80d3cac5edc1d88a7933ed8970a0c3f6a2542cb9892cd904003704f745",
}


def section(text: str, begin: str, end: str) -> str:
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError("production source boundaries changed: " + begin)
    start = text.index(begin)
    return text[start:text.index(end, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="Old production code as a negative control")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path],
                                           cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    if "tag android-16.0.0_r4" not in (ROOT / "aosp.lock").read_text():
        raise ValueError("Refresh the pinned AOSP clock sources for the new release")
    with tempfile.TemporaryDirectory(prefix="gesher-clock-domains-") as temporary:
        work = Path(temporary)
        for original, destination in (("time_utils.cc", "libartbase/base/time_utils.cc"),
                                      ("Timers.cpp", "libutils/Timers.cpp"),
                                      ("SystemClock.cpp", "libutils/SystemClock.cpp")):
            data = (ROOT / "tests/clock-domains/aosp" / original).read_bytes()
            if hashlib.sha256(data).hexdigest() != PINNED[original]:
                raise ValueError("Pinned AOSP clock source changed: " + original)
            target = work / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for path in ("patches/art/0572-native3-runtime-cpu-clocks.patch",
                     "patches/art/0573-native3-runtime-monotonic-clocks.patch",
                     "patches/system/core/0006-libutils-monotonic-clocks-on-gesher.patch"):
            subprocess.run(["git", "apply", "--whitespace=error-all", "-"], cwd=work,
                           input=read(path), text=True, check=True, timeout=15)
        system_clock = (work / "libutils/SystemClock.cpp").read_text()
        art = (work / "libartbase/base/time_utils.cc").read_text()
        utils = (work / "libutils/Timers.cpp").read_text()
        shim = read("compat/bionic/shim.cc")
        extra = read("compat/bionic/extra_sys_bionic.cc")
        posix = read("compat/posix/src/internal.cc")
        futex = read("compat/posix/src/syscall.cc")
        helper_start = "int64_t QpcNanos() {" if "int64_t QpcNanos() {" in shim else "int64_t MonotonicNanos() {"
        capture = read("compat/media/audio/capture.cc")
        codec = read("compat/media/jni/media_codec_jni.cc")
        player = read("compat/media/player/player.cc")
        anchors = re.findall(r"^  (?:std::chrono::steady_clock::time_point|int64_t) clock_anchor_[^;]*;", player, re.M)
        if len(anchors) != 1:
            raise ValueError("player clock anchor changed")
        parts = {
            "@CODEC_RENDER@": section(codec, "  void PostFrameRendered(", "  // With mu_ held: the decoder"),
            "@CODEC_INPUT@": section(codec, "  void OnSurfaceFrame(", "  // With mu_ held: the format of what comes out."),
            "@PLAYER_CLOCK@": section(player, "  int64_t WallClockLocked()", "  // The timestamp exposes"),
            "@PLAYER_ANCHOR@": anchors[0],
            "@PLAYER_START@": re.search(r"clock_anchor_ = [^;]+;", player).group(),
            "@AUDIO_API@": section(read("compat/media/audio/capture.h"), "class CaptureSource {", "// WASAPI shared mode"),
            "@AUDIO_CLOCK@": section(capture, "int64_t MonotonicNs() {", "// --- Sources paced"),
            "@AUDIO_PACING@": section(capture, "class PacedSource :", "class SilentSource :"),
            "@AUDIO_SINK@": section(read("compat/media/audio/sink_file.cc"), "class Clock {", "class ClockedSink :"),
            "@BIONIC_READER@": section(shim, helper_start, "int64_t ThreadCpuNanos() {") +
                section(shim, "int BIONIC_SYSV ClockGettime(", "int BIONIC_SYSV ClockGetres("),
            "@BIONIC_DEADLINE@": section(shim, "uint32_t MillisUntil(", "void BIONIC_SYSV GcovStub("),
            "@SLEEP_HELPERS@": section(extra, "int64_t NowRealtimeNs() {", "// Milliseconds until an absolute time"),
            "@CLOCK_SLEEP@": section(extra, "int BIONIC_SYSV ClockNanosleep(", "int BIONIC_SYSV ClockGetcpuclockid("),
            "@ART_CLOCKS@": section(art, "uint64_t MilliTime() {", "uint64_t ThreadCpuNanoTime() {") +
                section(art, "void InitTimeSpec(", "}  // namespace art"),
            "@SYSTEM_CLOCK@": system_clock[system_clock.index("namespace android {"):],
            "@UTILS_CLOCKS@": section(utils, "static constexpr size_t clock_id_max", "int toMillisecondTimeoutDelay("),
            "@POSIX_DEADLINES@": section(posix, "ULONGLONG MonotonicMicros() {", "}  // namespace gesher"),
            "@FUTEX_DEADLINES@": section(futex, "struct Deadline {", "long Wait("),
        }
        template = (ROOT / "tests/clock-domains/host.cc.in").read_text()
        for marker, content in parts.items():
            if template.count(marker) != 1:
                raise ValueError("fixture marker changed: " + marker)
            # Old-source controls use the same controlled host clock boundary
            # for chrono as for QPC; production arithmetic remains unchanged.
            body = content.replace("std::chrono::steady_clock", "HostSteadyClock")
            template = template.replace(marker, body)
            print(marker.strip("@"), hashlib.sha256(content.encode()).hexdigest(), flush=True)
        (work / "host.cc").write_text(template)
        (work / "windows.h").write_text("// Win32 boundary types are supplied by host.cc.\n")
        (work / "gesher").mkdir()
        (work / "gesher/clocks.h").write_text((ROOT / "compat/posix/include/gesher/clocks.h").read_text())
        (work / "gesher/awake_clock.h").write_text((ROOT / "compat/posix/include/gesher/awake_clock.h").read_text())
        binary = work / "clock-domains"
        command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-Wno-sign-compare", "-fno-exceptions", "-DGESHER", "-I", str(work),
            str(work / "host.cc"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary)], check=False, timeout=15).returncode


if __name__ == "__main__":
    raise SystemExit(main())
