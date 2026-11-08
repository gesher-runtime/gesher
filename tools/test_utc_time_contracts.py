#!/usr/bin/env python3
"""Run production UTC functions against Linux and optional pinned AOSP tzcode.

The source-ref negative control models only the documented UCRT range boundary.
Real optimized Android imports are also exercised by libc_test.c on Windows.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
PINNED_TZCODE = {
    "localtime.c": "94734ca974ae7205dccef40dfcface2c3520ae67c932fb3af7aec1fd47e35b92",
    "private.h": "b6e4dbc3b2b275f222d0fe9ce8761fdf7ccad5102515a30323e2e7ef34d13238",
    "tzfile.h": "87192a964c733091b397860bfb42a2a03dc2c342f9e495ebc8c4d79a1e187a35",
}


def extract(source, begin, end):
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("production UTC boundaries changed")
    return source[source.index(begin):source.index(end, source.index(begin))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Compile a prior production revision as a negative control")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--tzcode-dir", type=Path,
                        help="Directory with unchanged Android16.0.0_r4 tzcode files (hash verified)")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    path = "compat/bionic/time_bionic.cc"
    source = (subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / path).read_text())
    print(path + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    static = re.search(r"^BionicTm\* BIONIC_SYSV Gmtime\(.*\}$", source, re.MULTILINE)
    if static is None:
        raise ValueError("gmtime wrapper boundary changed")
    sections = {
        "@UTC_TYPES@": extract(source, "struct BionicTm {", "\n// tzname/timezone/daylight"),
        "@UTC_CONSTANT@": 'const char kUtc[] = "UTC";',
        "@UTC_FORWARD@": extract(source, "void FromCrt(", "\nBionicTm* BIONIC_SYSV LocaltimeR("),
        "@UTC_STATIC@": "thread_local BionicTm t_tm;\n" + static.group(0),
        "@UTC_INVERSE@": extract(source, ("bool UtcNormalizeField(" if "bool UtcNormalizeField(" in source
                                            else "int64_t BIONIC_SYSV Timegm("), "\n// --- strftime"),
    }
    if source.count(sections["@UTC_CONSTANT@"]) != 1 or source.count("thread_local BionicTm t_tm;") != 1:
        raise ValueError("UTC constant or thread storage boundary changed")
    template = (ROOT / "tests/native/utc_time_host_test.cc.in").read_text()
    for marker, value in sections.items():
        if template.count(marker) != 1:
            raise ValueError("host template boundary changed: " + marker)
        template = template.replace(marker, value)
    if args.tzcode_dir:
        for name, expected in PINNED_TZCODE.items():
            actual = hashlib.sha256((args.tzcode_dir / name).read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError("pinned Android tzcode identity mismatch: " + name)
            print(name + " SHA256 " + actual, flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-utc-") as temporary:
        directory = Path(temporary)
        cpp, executable = directory / "utc.cc", directory / "utc"
        cpp.write_text(template)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-pthread", "-I", str(ROOT / "tests/native"), str(cpp), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if args.tzcode_dir:
            oracle = directory / "tzcode.o"
            # Calendar code is unchanged. Renamed symbols and the host UTC
            # zone-file loader let it coexist with Linux libc as an oracle.
            compile_oracle = shlex.split(args.cc) + ["-std=gnu11", "-O2", "-D_GNU_SOURCE",
                "-DPROP_VALUE_MAX=92", "-Dtime_tz=time_t", "-DSTD_INSPIRED=1", "-DTHREAD_SAFE=1",
                "-DTM_GMTOFF=tm_gmtoff", "-DTM_ZONE=tm_zone", '-DTZDIR="/usr/share/zoneinfo"',
                "-c", str(args.tzcode_dir / "localtime.c"), "-o", str(oracle)]
            subprocess.run(compile_oracle, check=True, timeout=120)
            command += ["-DHAVE_PINNED_TZCODE=1", str(oracle)]
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        if args.sanitize:
            environment["UBSAN_OPTIONS"] = "halt_on_error=1:" + environment.get("UBSAN_OPTIONS", "")
        result = subprocess.run([str(executable)], env=environment, text=True,
                                capture_output=True, timeout=120)
        print(result.stdout, end="", flush=True)
        if result.stderr:
            print(result.stderr, end="", flush=True)
        return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
