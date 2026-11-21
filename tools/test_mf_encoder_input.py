#!/usr/bin/env python3
"""Run the complete production MF encoder at a declared Win32/MF host boundary.

The adapter supplies owned, reference-counted MF buffers and samples. The real
encoder, crop copying, sample construction, retry, output parsing and teardown
execute unchanged. This does not execute Windows Media Foundation or a codec.
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--measure-only", action="store_true",
                        help="Run valid-input compatibility and report measurements without budgets")
    parser.add_argument("--valid-only", action="store_true",
                        help="Keep allocation/copy budgets but skip failure/invalid cases unsafe on the baseline")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    relative = "compat/media/encode/mf_encoder.cc"
    source = ((ROOT / relative).read_bytes() if not args.source_ref else subprocess.check_output(
        ["git", "show", args.source_ref + ":" + relative], cwd=ROOT))
    print("MF encoder source SHA256 " + hashlib.sha256(source).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-mf-encoder-") as temporary:
        work = Path(temporary)
        production = work / "mf_encoder.cc"
        production.write_bytes(source)
        for name in ("windows.h", "mfapi.h", "mferror.h", "mfidl.h", "mftransform.h",
                     "objbase.h", "oleauto.h"):
            (work / name).write_text('#include "tests/media/mf_encoder_host.h"\n')
        binary = work / "mf_encoder_input"
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-DGESHER_MEDIA_STANDALONE", '-DGESHER_MF_ENCODER_SOURCE="' + str(production) + '"',
            "-I", str(work), "-I", str(ROOT), "-I", str(ROOT / "compat/media/encode"),
            str(ROOT / "tests/media/mf_encoder_input_host.cc"),
            str(ROOT / "compat/media/encode/codec_config.cc"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        mode = "measure" if args.measure_only else "valid-gate" if args.valid_only else "gate"
        return subprocess.run([str(binary), mode], check=False, timeout=120).returncode


if __name__ == "__main__":
    raise SystemExit(main())
