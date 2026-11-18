#!/usr/bin/env python3
"""Check the production APS2 decoder/relocation dispatcher and its temporary storage.

The actual parser and Relocate are compiled unchanged. Relocation application,
symbol/IFUNC resolution and Win32 mapping are explicit observing host boundaries;
they do not constitute Windows/ART execution. An independent encoder supplies
expected field values, and optional exact-baseline execution checks compatibility.
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


def production(ref: str | None) -> str:
    path = "compat/elf/elf_loader.cc"
    source = ((ROOT / path).read_text() if ref is None else
              subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True))
    start, end = '// DT_ANDROID_RELA: "APS2",', '// What init functions get (SetInitArguments):'
    if source.count(start) != 1 or source.count(end) != 1:
        raise ValueError("packed relocation extraction boundaries changed")
    return source[source.index(start):source.index(end)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Run an older source with the same assertions/budgets")
    parser.add_argument("--compare-ref", help="Also run the compatible-behavior oracle on this source")
    parser.add_argument("--measure-only", action="store_true", help="Disable only resource budgets")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    candidate = production(args.source_ref)
    reference = production(args.compare_ref) if args.compare_ref else ""
    fixture = (ROOT / "tests/native/elf_packed_relocations_host.cc.in").read_text()
    fixture = fixture.replace("@CANDIDATE@", candidate).replace("@REFERENCE@", reference)
    fixture = fixture.replace("@CORRUPTION@", "Corruption(candidate::Relocate);" if
                              "template <bool ValidateOnly" in candidate else "(void)Corruption;")
    fixture = fixture.replace("@RANGE_CHECKS@", "MappedRanges(candidate::Relocate);" if
                              "bool ReadablePackedRange" in candidate else "(void)MappedRanges;")
    print("packed relocation source SHA256 " + hashlib.sha256(candidate.encode()).hexdigest(), flush=True)
    if reference:
        print("reference source SHA256 " + hashlib.sha256(reference.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-elf-packed-") as temporary:
        work = Path(temporary)
        source = work / "packed.cc"
        source.write_text(fixture)
        binary = work / ("packed.exe" if os.name == "nt" else "packed")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-pthread", f"-DHAS_REFERENCE={int(bool(reference))}", str(source), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        return subprocess.run([str(binary), "measure" if args.measure_only else "gate"],
                              check=False, timeout=120).returncode


if __name__ == "__main__":
    raise SystemExit(main())
