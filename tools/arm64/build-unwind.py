#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Build exact native thunk probes and source-derived controls; no Dynarmic."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = "e80cd1854163bd18e4ea5392c25f423184048804"
NAME = "llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
ARCHIVE_SHA = "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/{NAME}.tar.xz"
PRODUCTION = "compat/arm64/dll/thunks.S"
SOURCES = [
    PRODUCTION, "compat/arm64/dll/arm64_unwind_test.S",
    "compat/arm64/dll/arm64_unwind_test.cc", "tools/arm64/build-unwind.py",
    "tools/arm64/run-unwind.ps1", ".github/workflows/arm64-unwind.yml",
    "docs/research/arm64-thunk-unwind-2026-11-17.md",
]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def boundaries(disassembly, unwind):
    info = {}
    for block in unwind.split("  RuntimeFunction {")[1:]:
        name = re.search(r"StartAddress: (\w+) ", block)[1]
        end = re.search(r"EndAddress: \w+ \+0x([0-9A-Fa-f]+)", block)
        prolog = re.search(r"PrologSize: (\d+)", block)
        info[name] = {"size": int(end[1], 16), "prolog": int(prolog[1]),
                      "instructions": []}
    assert set(info) == {"gesher_call_host_sysv", "gesher_guest_entry_shim"}
    name = None
    for line in disassembly.splitlines():
        label = re.fullmatch(r"([0-9a-f]+) <([^>]+)>:", line)
        if label:
            start, name = int(label[1], 16), label[2]
            continue
        insn = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{2}\s+)+)([a-z].*)", line)
        if name in info and insn:
            offset = int(insn[1], 16) - start
            if offset < info[name]["size"]:
                info[name]["instructions"].append(
                    {"offset": offset, "bytes": insn[2].split(), "asm": insn[3]})
    for item in info.values():
        insns = item.pop("instructions")
        calls = [i for i, row in enumerate(insns) if row["asm"].startswith("call")]
        assert len(calls) == 1 and insns[-1]["asm"] == "ret"
        selected = set()
        for i, row in enumerate(insns):
            if row["offset"] <= item["prolog"] or i >= calls[0]:
                selected.add(i)
            # Before/after the body stack adjustments, including guest shadow.
            if re.match(r"(and|sub)\s+rsp,", row["asm"]):
                selected.update((i, i + 1))
        item["boundaries"] = [insns[i] for i in sorted(selected)]
        assert item["boundaries"][0]["offset"] == 0
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    toolchain, out = args.toolchain.resolve(), args.out.resolve()
    assert toolchain.name == NAME, "unexpected compiler release"
    assert sha(args.archive) == ARCHIVE_SHA, "compiler archive checksum mismatch"
    assert not out.exists() or not any(out.iterdir()), "use an empty output directory"
    out.mkdir(parents=True, exist_ok=True)
    ci = {k: os.environ.get(k) for k in
          ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_REPOSITORY",
           "GITHUB_REPOSITORY_ID", "GITHUB_WORKFLOW", "GITHUB_JOB")}
    commit, tree, dirty = git("rev-parse", "HEAD"), git("rev-parse", "HEAD^{tree}"), bool(git("status", "--porcelain"))
    if os.environ.get("GITHUB_ACTIONS") == "true":
        assert ci["GITHUB_SHA"] == commit and not dirty, "CI source must be exact and clean"
    compiler = toolchain / "bin/x86_64-w64-mingw32-clang++"
    commands = []

    def run(argv, log):
        argv = [str(a) for a in argv]
        process = subprocess.run(argv, cwd=ROOT, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / log).write_text(process.stdout)
        commands.append({"argv": argv, "cwd": str(ROOT), "exit_code": process.returncode,
                         "output": log})
        if process.returncode:
            raise RuntimeError(f"{log}: {process.stdout}")
        return process.stdout

    version = run([compiler, "--version"], "compiler-version.txt")
    assert "clang version 23.1.3" in version
    run([compiler, "-c", ROOT / PRODUCTION, "-o", out / "thunks.obj"], "compile-thunks.txt")
    unwind = run([toolchain / "bin/llvm-readobj", "--unwind", out / "thunks.obj"], "thunks-unwind.txt")
    disasm = run([toolchain / "bin/llvm-objdump", "-d", "--x86-asm-syntax=intel",
                  out / "thunks.obj"], "thunks-disassembly.txt")
    assert "SET_FPREG reg=RBP, offset=0xC0" in unwind
    assert "SET_FPREG reg=RBP, offset=0xA0" in unwind
    assert unwind.count("SAVE_XMM128") == 10
    for register in range(6, 16):
        assert f"SAVE_XMM128 reg=XMM{register}, offset=0x{(register-6)*16:X}" in unwind
    sites = boundaries(disasm, unwind)
    header = "// Generated from the compiled production object. Do not hand-edit.\n"
    for label, symbol in (("Host", "gesher_call_host_sysv"), ("Guest", "gesher_guest_entry_shim")):
        item = sites[symbol]
        offsets = ", ".join(hex(row["offset"]) for row in item["boundaries"])
        header += f"static constexpr unsigned k{label}CodeSize = {item['size']};\n"
        header += f"static constexpr unsigned k{label}Boundaries[] = {{{offsets}}};\n"
    (out / "arm64_unwind_offsets.h").write_text(header)
    (out / "instruction-boundaries.json").write_text(json.dumps(sites, indent=2) + "\n")
    run([compiler, "-c", ROOT / "compat/arm64/dll/arm64_unwind_test.S",
         "-o", out / "callers.obj"], "compile-callers.txt")
    run([compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I", out, "-c", ROOT / "compat/arm64/dll/arm64_unwind_test.cc",
         "-o", out / "probe.obj"], "compile-probe.txt")
    source = (ROOT / PRODUCTION).read_text()
    before = subprocess.check_output(
        ["git", "show", f"{BASE}:{PRODUCTION}"], cwd=ROOT).decode("utf-8")
    assert ".seh_proc" not in before, "baseline is no longer the missing-metadata control"
    (out / "thunks-baseline.S").write_text(before)
    needle = "\t.seh_savexmm xmm15, 144"
    assert source.count(needle) == 1
    (out / "thunks-wrong-xmm.S").write_text(source.replace(needle, "\t.seh_savexmm xmm15, 128"))
    for variant in ("baseline", "wrong-xmm"):
        run([compiler, "-c", out / f"thunks-{variant}.S", "-o",
             out / f"thunks-{variant}.obj"], f"compile-{variant}.txt")
        run([toolchain / "bin/llvm-readobj", "--unwind", out / f"thunks-{variant}.obj"],
            f"{variant}-unwind.txt")
    assert "RuntimeFunction" not in (out / "baseline-unwind.txt").read_text()
    assert "SAVE_XMM128 reg=XMM15, offset=0x80" in (out / "wrong-xmm-unwind.txt").read_text()
    for label, obj in (("production", "thunks.obj"), ("wrong-xmm", "thunks-wrong-xmm.obj")):
        run([toolchain / "bin/llvm-objcopy", "--dump-section",
             f".text={out / (label + '-text.bin')}", out / obj], f"extract-{label}-text.txt")
    assert (out / "production-text.bin").read_bytes() == (out / "wrong-xmm-text.bin").read_bytes(), \
        "the wrong-XMM control must change metadata only, not executable instructions"
    for name, obj in (("arm64_unwind_test", "thunks.obj"),
                      ("baseline_no_unwind", "thunks-baseline.obj"),
                      ("wrong_xmm_unwind", "thunks-wrong-xmm.obj")):
        run([compiler, "-static", "-Wl,--no-insert-timestamp", out / "probe.obj",
             out / "callers.obj", out / obj, "-o", out / f"{name}.exe"],
            f"link-{name}.txt")
    imports = run([toolchain / "bin/llvm-readobj", "--coff-imports",
                   out / "arm64_unwind_test.exe"], "executable-imports.txt")
    for name in ("RtlLookupFunctionEntry", "RtlVirtualUnwind", "RtlCaptureContext"):
        assert name in imports
    receipt = {
        "schema": "gesher-arm64-native-unwind-v1",
        "source": {"commit": commit, "tree": tree, "dirty": dirty,
                   "files": {p: sha(ROOT / p) for p in SOURCES}},
        "ci": ci,
        "compiler": {"archive_url": URL, "archive_sha256": ARCHIVE_SHA,
                     "driver_sha256": sha(compiler), "version": version},
        "controls": {"baseline_commit": BASE, "baseline_path": PRODUCTION,
                     "wrong_xmm": "Only XMM15 unwind slot changes from 144 to 128; machine code unchanged"},
        "commands": commands,
        "outputs": {p.name: sha(p) for p in sorted(out.iterdir()) if p.is_file()},
        "runtime": "not executed by this cross-compile recipe",
    }
    (out / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Compiled exact production thunks and two controls: {out}")
    print(f"Instruction boundaries: host={len(sites['gesher_call_host_sysv']['boundaries'])}, "
          f"guest={len(sites['gesher_guest_entry_shim']['boundaries'])}")


if __name__ == "__main__":
    main()
