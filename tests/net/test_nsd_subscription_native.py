#!/usr/bin/env python3
"""Finite continuity checks and an optional complete Windows NSD link gate.

The Windows gate compiles the real production transport and the production
POSIX source list, then links the existing guest bind/sendto/setsockopt/select
fixture. It never executes the Windows binary. Supply pinned, already fetched
AOSP inputs; this runner does not download or substitute any dependency.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--host-cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--llvm-mingw", type=Path)
    parser.add_argument("--aosp-dir", type=Path)
    parser.add_argument("--support-aosp-dir", type=Path,
                        help="Optional separate pinned bionic/ICU inputs")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 32:
        parser.error("--jobs must be between 1 and 32")
    if args.llvm_mingw and not args.aosp_dir:
        parser.error("--aosp-dir is required for the Windows cross-build")
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    records: list[dict] = []
    report: dict = {
        "scope": "ordinary host lifetime observations and optional Windows cross-build/link",
        "windows_runtime_executed": False,
        "asan_options": os.environ.get("ASAN_OPTIONS"),
        "commands": records,
        "source_root": str(ROOT),
    }

    def run(name: str, command: list[str], timeout: int = 180) -> dict:
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=timeout)
        log = out / (name + ".log")
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(result.stdout + result.stderr)
        record = {"name": name, "argv": command, "exit": result.returncode,
                  "seconds": round(time.monotonic() - started, 6), "log": str(log),
                  "log_sha256": digest(log)}
        records.append(record)
        if result.returncode:
            raise RuntimeError(f"{name} failed; see {log}")
        return record

    native = ROOT / "compat/net/windows_nsd_resolve.cc"
    header = ROOT / "compat/net/include/gesher/nsd_subscription_lifetimes.h"
    host_source = ROOT / "tests/net/nsd_subscription_lifetimes_test.cc"
    inputs = [native, header, host_source, Path(__file__).resolve()]
    initial = {str(path): digest(path) for path in inputs}
    try:
        host = out / "lifetimes-host"
        host_flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
        if args.sanitize:
            host_flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        run("host-compile", [args.host_cxx, *host_flags,
                            "-I" + str(ROOT / "compat/net/include"), str(host_source), "-o", str(host)])
        host_run = run("host-run", [str(host)], timeout=30)
        text = Path(host_run["log"]).read_text()
        match = re.fullmatch(r"NSD_SUBSCRIPTION_LIFETIMES_OK checks=(\d+)\n", text)
        if not match:
            raise RuntimeError("host test did not produce its complete success marker")
        report["host_checks"] = int(match.group(1))
        report["host_sanitizers"] = bool(args.sanitize)
        if args.llvm_mingw:
            toolchain = args.llvm_mingw.resolve()
            aosp = args.aosp_dir.resolve()
            support = (args.support_aosp_dir or args.aosp_dir).resolve()
            posix_cmake = ROOT / "compat/posix/posix.cmake"
            posix_sources = sorted(set(re.findall(r"\$\{CMAKE_CURRENT_LIST_DIR\}/src/([\w]+\.cc)",
                                                 posix_cmake.read_text())))
            if not posix_sources:
                raise RuntimeError("production POSIX source list is empty")
            gen = out / "generated"
            gen.mkdir(exist_ok=True)
            zones = support / "external/icu/icu4c/source/data/misc/windowsZones.txt"
            run("windows-zones", [sys.executable, str(ROOT / "compat/posix/gen_windows_zones.py"),
                                  str(zones), str(gen / "windows_zones_table.inc")])
            initial[str(posix_cmake)] = digest(posix_cmake)
            report["posix_source_count"] = len(posix_sources)
            report["cross_architectures"] = {}
            definitions = ["GESHER", "USE_MINGW", "WIN32_LEAN_AND_MEAN", "NOMINMAX", "NOGDI",
                           "_USE_MATH_DEFINES", "__STDC_FORMAT_MACROS", "__STDC_CONSTANT_MACROS",
                           "__STDC_LIMIT_MACROS", "_WIN32_WINNT=0x0A00", "WINVER=0x0A00",
                           "_FILE_OFFSET_BITS=64", "_POSIX_THREAD_SAFE_FUNCTIONS"]
            includes = [ROOT / "compat/posix/include", ROOT / "compat/net/include", gen]
            includes += [aosp / "libnativehelper" / name for name in
                         ("header_only_include", "include", "include_jni", "include_platform",
                          "include_platform_header_only")]
            includes += [aosp / "system/logging/liblog/include"]
            common = ["-std=gnu++20", "-O2", "-g", "-DNDEBUG", "-mno-ms-bitfields",
                      "-fno-exceptions", "-fsized-deallocation", "-Wall", "-Wextra",
                      "-Wno-unused-parameter", *["-D" + value for value in definitions],
                      *["-I" + str(path) for path in includes]]
            for architecture, triple in (("x64", "x86_64"), ("arm64", "aarch64")):
                build = out / architecture
                build.mkdir(exist_ok=True)
                compiler = str(toolchain / "bin" / (triple + "-w64-mingw32-clang++"))
                compilation_inputs: dict[str, str] = {}

                def compile_source(source: Path, name: str, extra: list[str] = []) -> Path:
                    obj, dep = build / (name + ".obj"), build / (name + ".d")
                    before = digest(source)
                    run(architecture + "/" + name, [compiler, *common, *extra, "-MD", "-MF", str(dep),
                                                     "-c", str(source), "-o", str(obj)])
                    if digest(source) != before:
                        raise RuntimeError(f"source changed during compilation: {source}")
                    dependencies = shlex.split(dep.read_text().replace("\\\n", " ").split(":", 1)[1])
                    for value in dependencies:
                        path = Path(value).resolve()
                        compilation_inputs[str(path)] = digest(path)
                    return obj

                def compile_posix(name: str) -> Path:
                    source = ROOT / "compat/posix/src" / name
                    extra = ["-Werror"]
                    if name == "binder.cc":
                        extra += ["-idirafter", str(support / "bionic/libc/kernel/uapi")]
                    if name == "mman_alias_prepare.cc":
                        extra += ["-fexceptions"]
                    return compile_source(source, "posix-" + source.stem, extra)

                with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                    objects = list(pool.map(compile_posix, posix_sources))
                archive = build / "libgesher_posix.a"
                run(architecture + "/archive", [str(toolchain / "bin/llvm-ar"), "rcs", str(archive),
                                                 *map(str, objects)])
                transport = compile_source(native, "subscription-transport")
                fixture = compile_source(ROOT / "tests/net/nsd_windows_link_test.cc", "guest-link-fixture")
                executable = build / "nsd_windows_link_test.exe"
                linked = run(architecture + "/link", [compiler, "-static", str(fixture), str(transport),
                    "-Wl,-u,gesher_crash_reporter_anchor", "-Wl,-u,gesher_signal_mailbox_anchor",
                    "-Wl,-u,gesher_futex_owner_anchor_v1", "-Wl,--wrap=free", "-Wl,--wrap=realloc",
                    "-Wl,-Map," + str(build / "link.map"), "-Wl,--verbose", str(archive),
                    "-lpowrprof", "-lsynchronization", "-lpsapi", "-lws2_32", "-liphlpapi",
                    "-lbcrypt", "-lshell32", "-ladvapi32", "-lole32", "-luuid", "-lpthread",
                    "-lkernel32", "-lmincore", "-o", str(executable)])
                link_inputs = {}
                for line in Path(linked["log"]).read_text().splitlines():
                    if not line.startswith("ld.lld: Reading "):
                        continue
                    path = Path(line.removeprefix("ld.lld: Reading ").strip())
                    if path.is_absolute() and path.is_file():
                        link_inputs[str(path)] = digest(path)
                if not all(str(path) in link_inputs for path in (transport, fixture, archive)):
                    raise RuntimeError("linker did not expose the actual production/fixture/archive input set")
                undefined = run(architecture + "/transport-undefined",
                                [str(toolchain / "bin/llvm-nm"), "-u", str(transport)])
                symbols = Path(undefined["log"]).read_text()
                forbidden = [name for name in ("bind", "sendto", "setsockopt", "select")
                             if re.search(r"\b__imp_" + name + r"\b", symbols)]
                if forbidden:
                    raise RuntimeError("guest-colliding Winsock imports: " + ", ".join(forbidden))
                report["cross_architectures"][architecture] = {
                    "exe_sha256": digest(executable), "archive_sha256": digest(archive),
                    "transport_sha256": digest(transport), "compiler_sha256": digest(Path(compiler)),
                    "input_hashes": compilation_inputs, "conflicting_winsock_imports": forbidden,
                    "link_input_hashes": link_inputs,
                    "runtime_executed": False,
                }
        if any(digest(Path(path)) != value for path, value in initial.items()):
            raise RuntimeError("an initial source input changed during validation")
        report["initial_source_hashes"] = initial
        report["success"] = True
    except Exception as error:
        report["success"] = False
        report["error"] = str(error)
    finally:
        (out / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: report.get(key) for key in
                      ("success", "host_checks", "posix_source_count", "error")}, sort_keys=True))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
