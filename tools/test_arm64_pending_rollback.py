#!/usr/bin/env python3
"""Run production ARM64 pending-load retirement with explicit host boundaries.

Registry, TLS, host-entry/trampoline recycling, classification, CallSVC,
RunGuest and Attach methods are inserted verbatim from translator.cc. The
Win32/Jit adapter retains instruction bytes until production invalidates them.
Native AArch64 or Windows execution is not established here.
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


def section(source: str, begin: str, end: str) -> str:
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError(f"ARM64 pending rollback source boundary changed: {begin}")
    return source[source.index(begin):source.index(end, source.index(begin))]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if os.name == "nt":
        parser.error("this fixture uses explicit POSIX mappings; use the Windows translator fixture instead")
    source = (ROOT / "compat/arm64/dll/translator.cc").read_text(encoding="utf-8")
    sections = {
        "@FENV@": section(source, "class HostFenvScope {", "// Internal callbacks"),
        "@PROFILE@": section(source, "int ProfilePeriodMs() {", "bool ProfilePreciseEnabled()"),
        "@REGISTRY@": section(source, "struct CodeRegion {", "// --- guest TLS"),
        "@TLS@": section(source, "constexpr size_t kTcbSize =", "// --- the exclusive monitor"),
        "@ENTRIES@": section(source, "std::mutex g_entry_mutex;", "// --- acquire loads"),
        "@RETIRE@": section(source, "void ApiRemoveCode(", "uint64_t ApiCall("),
        "@CLASSIFY@": section(source, "bool InGuestCode(uintptr_t addr) {", "uint32_t GuestFpcr()"),
        "@BINDINGS@": section(source, "void AddVaListFunction(const void* fn, int index) {", "bool InTranslatorCode("),
        "@COUNTERS@": section(source, "struct Counter {", "class Env final"),
        "@ATTACH@": section(source, "  void Attach() {", "  // The thread is exiting."),
        "@CALLSVC@": section(source, "  void CallSVC(std::uint32_t swi) override {", "  void ExceptionRaised("),
        "@DISPATCH@": section(source, "  struct PendingHostCall {", "  static constexpr size_t kStackSize ="),
    }
    template = (ROOT / "tests/native/arm64_pending_rollback_host.cc.in").read_text(encoding="utf-8")
    for marker, production in sections.items():
        if template.count(marker) != 1:
            raise ValueError(f"ARM64 pending rollback fixture marker changed: {marker}")
        template = template.replace(marker, production)
        print(marker + " production SHA256 " + hashlib.sha256(production.encode()).hexdigest(), flush=True)
    # Reuse the same test contract for both implementations, changing only
    # test call names; no production allocator/rollback body is rewritten.
    native_test = (ROOT / "tests/native/arm64_native_rollback_host.cc.in").read_text(encoding="utf-8")
    tls_test = section(native_test, "void TestTls() {", "}  // namespace fixture")
    tls_test = tls_test.replace("NativeTls", "ApiTls").replace("native ARM64 rollback", "ARM64 pending rollback")
    if template.count("@TLS_TEST@") != 1:
        raise ValueError("ARM64 shared TLS test marker changed")
    template = template.replace("@TLS_TEST@", tls_test)
    with tempfile.TemporaryDirectory(prefix="gesher-arm64-pending-rollback-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = folder / "test"
        cpp.write_text(template, encoding="utf-8")
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread",
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "compat/arm64/dll"),
            str(cpp), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
