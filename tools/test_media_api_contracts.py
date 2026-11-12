#!/usr/bin/env python3
"""Run production AAudio I/O, extractor setup and YUV crop/row preparation code.

The host supplies deterministic clocks, PCM transfers, extractor factories and
a YUV encoder observer. Raw JPEG row preparation is checked separately. It does
not emulate WASAPI, AOSP parsing, JPEG encoding,
JNI or Windows descriptors. The public
MediaDataContractTest and mediaapp NDK fixtures separately cover those routes.
--source-ref runs the same behavioral assertions against a prior revision.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(text: str, start: str, end: str) -> str:
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("production section boundaries changed")
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    def read(relative: str) -> str:
        if args.source_ref:
            return subprocess.run(["git", "show", args.source_ref + ":" + relative],
                                  cwd=ROOT, check=True, capture_output=True, text=True).stdout
        return (ROOT / relative).read_text(encoding="utf-8")

    audio = read("compat/media/ndk/aaudio.cc")
    extractor = read("compat/media/jni/media_extractor_jni.cc")
    yuv = read("compat/hwui/surfaces/yuv_image.cc")
    audio_start = ("int64_t StreamDeadline(" if "int64_t StreamDeadline(" in audio
                   else "aaudio_result_t SYSV WaitForStateChange(")
    extractor_start = ("JniExtractor* GetForDataSource(" if "JniExtractor* GetForDataSource(" in extractor
                       else "void Open(JNIEnv*")
    yuv_start = ("bool PlaneFits(" if "bool PlaneFits(" in yuv
                 else "jboolean JNICALL YuvImage_compressToJpeg(")
    yuv_rows_start = ("size_t JpegRowBytes(" if "size_t JpegRowBytes(" in yuv
                      else "void Nv21Rows(")
    sources = {
        "aaudio": section(audio, audio_start, "aaudio_result_t SYSV SetBufferSizeInFrames("),
        "extractor": section(extractor, extractor_start, "void JNICALL ME_setDataSourcePath("),
        "yuv": section(yuv, yuv_start, "jboolean JNICALL YuvImage_compressToJpegR("),
        "yuv_rows": section(yuv, yuv_rows_start, "bool Encode("),
    }
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-media-contracts-") as temporary:
        work = Path(temporary)
        for name, implementation in sources.items():
            template = (ROOT / "tests/media" / (name + "_contract_host.cc.in")).read_text()
            if template.count("@PRODUCTION_IMPLEMENTATION@") != 1:
                raise ValueError("invalid host template")
            generated = work / (name + ".cc")
            generated.write_text(template.replace("@PRODUCTION_IMPLEMENTATION@", implementation))
            executable = work / name
            command = shlex.split(args.cxx) + [
                "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(generated), "-o", str(executable),
            ]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-fno-omit-frame-pointer"]
            print(name + " production section SHA256 " + hashlib.sha256(implementation.encode()).hexdigest(),
                  flush=True)
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(executable)], check=False, timeout=30)
            failed |= result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
