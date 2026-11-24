#!/usr/bin/env python3
"""Exercise exact production local signal routing with real Windows thread DACLs.

Registry population, final handler/frame dispatch, UID, futex/translator notice
and remote broker authorization are explicit fixture boundaries. Windows thread
security, retained handles, waits, duplication, retirement and the production
pending container are real. This does not run ART or an AppContainer.
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


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError("missing or ambiguous production body: " + signature)
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def generate(directory: Path) -> Path:
    paths = ["compat/bionic/signal_bionic.cc", "compat/bionic/pthread_bionic.cc",
             "compat/bionic/shim_internal.h"]
    signal, pthread, shim = [(ROOT / path).read_text() for path in paths]
    for path in paths:
        print(path + " SHA256 " + hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), flush=True)
    constants = []
    for key in ("kNsig", "kSigDfl", "kSaSiginfo", "kSaResethand", "kSiUser"):
        found = re.findall(r"^constexpr [^\n]*\b" + key + r"\b[^\n]*;[^\n]*", signal, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous signal constant: " + key)
        constants.extend(found)
    for key in ("kEPERM", "kESRCH", "kEAGAIN", "kEINVAL", "kENOSYS"):
        found = re.findall(r"^constexpr int " + key + r"[^\n]+", shim, re.M)
        if len(found) != 1:
            raise ValueError("missing or ambiguous errno constant: " + key)
        constants.extend(found)
    types = [function(signal, "struct " + name + " {") + ";" for name in
             ("BionicSigaction", "BionicSiginfo", "BionicStack", "ThreadState", "State")]
    state = [function(signal, name) for name in (
        "State& S()", "void CALLBACK RetireThread(", "ThreadState* CreateThreadState(",
        "ThreadState* LiveThreadLocked(", "ThreadState* Self()")]
    delivery = [function(signal, name) for name in (
        "uint64_t Bit(", "bool Valid(", "void Pop(", "bool DefaultIgnores(",
        "bool DefaultStops(", "bool ActionSupported(", "bool ClaimActionLocked(",
        "int Run(", "int TakeDeliverable(", "int DeliverAll(", "BionicSiginfo MakeInfo(",
        "int SendFromSelf(", "int SendToOtherThread(")]
    routing = [function(signal, name) for name in (
        "int RegisteredSignalTarget(", "int SendToThread(",
        "int BIONIC_SYSV Tgkill(", "int BIONIC_SYSV Tkill(")]
    replacements = {"@CONSTANTS@": "\n".join(constants), "@TYPES@": "\n".join(types),
                    "@STATE@": "\n".join(state), "@DELIVERY@": "\n".join(delivery),
                    "@ROUTING@": "\n".join(routing),
                    "@PTHREAD_LOOKUP@": function(pthread, "int PthreadSignalTargetByTid(")}
    fixture = (ROOT / "tests/native/protected_thread_signal_windows.cc.in").read_text()
    for marker, body in replacements.items():
        if fixture.count(marker) != 1:
            raise ValueError("missing or ambiguous fixture marker: " + marker)
        fixture = fixture.replace(marker, body)
    print("extracted bodies SHA256 " + hashlib.sha256("\n".join(replacements.values()).encode()).hexdigest(), flush=True)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "gesher").mkdir(exist_ok=True)
    for name in ("pending_signals.h", "futex_interrupt.h"):
        data = (ROOT / "compat/posix/include/gesher" / name).read_bytes()
        (directory / "gesher" / name).write_bytes(data)
        print(name + " SHA256 " + hashlib.sha256(data).hexdigest(), flush=True)
    output = directory / "protected_thread_signal_windows.cc"
    output.write_text(fixture, encoding="utf-8", newline="\n")
    print("generated source SHA256 " + hashlib.sha256(output.read_bytes()).hexdigest(), flush=True)
    return output


def run(directory: Path, args: argparse.Namespace) -> int:
    source = generate(directory)
    executable = directory / "protected_thread_signal_windows.exe"
    compiler = shlex.split(args.cxx, posix=os.name != "nt")
    if Path(compiler[0]).name.lower() in ("cl", "cl.exe"):
        command = compiler + ["/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
            "/D_WIN32_WINNT=0x0A00", "/DNOMINMAX", "/I" + str(directory), str(source),
            "/Fe:" + str(executable), "advapi32.lib"]
    else:
        command = compiler + ["-std=gnu++20", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wshorten-64-to-32", "-mno-ms-bitfields", "-fexceptions", "-D_WIN32_WINNT=0x0A00",
            "-DNOMINMAX", "-I", str(directory), str(source), "-static", "-ladvapi32", "-o", str(executable)]
    print("compile: " + subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=directory, check=True, timeout=120)
    if args.compile_only:
        print("cross-link only; no Windows execution claimed", flush=True)
        return 0
    if os.name != "nt":
        raise RuntimeError("actual Windows required; --compile-only permits cross-link validation")
    return subprocess.run([str(executable)], cwd=directory, timeout=90).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="cl")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    if args.build_dir:
        return run(args.build_dir.resolve(), args)
    with tempfile.TemporaryDirectory(prefix="gesher-protected-thread-signals-") as temporary:
        return run(Path(temporary), args)


if __name__ == "__main__":
    raise SystemExit(main())
