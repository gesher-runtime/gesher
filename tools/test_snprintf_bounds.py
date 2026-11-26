#!/usr/bin/env python3
"""Execute the actual guest snprintf parser and va-list entry on Linux x64.

glibc supplies the declared host CRT conversion boundary; native SysV calls
provide real register/stack arguments. Production parser/accessor/entry bodies
are not rewritten. Actual Windows MinGW and guest ABI remain mandatory CI gates.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def section(source, first, after):
    if source.count(first) != 1 or source.count(after) != 1:
        raise ValueError("Ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(after, start)]


def compile_probe(folder, source, executable, cxx, sanitize=False, observe_allocations=False):
    flags = ["-std=c++17", "-O2" if sanitize else "-O3", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-fno-builtin", "-ffunction-sections", "-fdata-sections"]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if observe_allocations:
        flags += ["-DSNPRINTF_OBSERVE_ALLOCATIONS"]
    command = shlex.split(cxx) + flags + ["-I", str(folder / "include"),
               "-I", str(ROOT / "tests/native"), str(source), "-Wl,--gc-sections", "-o", str(executable)]
    subprocess.run(command, check=True, timeout=120)
    return command


def build(folder, source_ref=None, cxx="g++", sanitize=False, observe_allocations=False):
    def read(path):
        if source_ref:
            return subprocess.check_output(["git", "show", source_ref + ":" + path], cwd=ROOT)
        return (ROOT / path).read_bytes()
    paths = ("compat/bionic/stdio_bionic.cc", "compat/bionic/shim_internal.h",
             "compat/arm64/include/gesher/arm64_va_list.h")
    sources = {p: read(p) for p in paths}
    stdio, shim = (sources[p].decode() for p in paths[:2])
    markers = {
        "@VARARGS@": section(shim, "struct SysvVaList {", "// A trampoline for a variadic function"),
        "@FORMATTING@": section(stdio, "double NextFp128(", "// --- FILE ---"),
        "@SNPRINTF@": section(stdio, "int BIONIC_SYSV gesher_bionic_vsnprintf(",
                              "int BIONIC_SYSV gesher_bionic_vsprintf("),
        "@CHECKED_SNPRINTF@": section(stdio, "int BIONIC_SYSV gesher_bionic_vsnprintf_chk(",
                                      "int BIONIC_SYSV gesher_bionic_vsprintf_chk("),
    }
    template = (ROOT / "tests/native/snprintf_bounds_host.cc.in").read_text()
    for marker, value in markers.items():
        if template.count(marker) != 1:
            raise ValueError("Missing template boundary: " + marker)
        template = template.replace(marker, value)
    include = folder / "include/gesher"
    include.mkdir(parents=True)
    (include / "arm64_va_list.h").write_bytes(sources[paths[2]])
    source = folder / "probe.cc"
    source.write_text(template)
    executable = folder / "snprintf-bounds"
    command = compile_probe(folder, source, executable, cxx, sanitize, observe_allocations)
    hashes = {p: hashlib.sha256(v).hexdigest() for p, v in sources.items()}
    hashes.update({str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
                   (ROOT / "tests/native/snprintf_bounds_host.cc.in", ROOT / "tests/native/snprintf_bounds_contract.inc",
                    ROOT / "tests/native/snprintf_bounds_benchmark.inc")})
    (folder / "build.json").write_text(json.dumps({"command": command,
        "compiler": subprocess.check_output(shlex.split(cxx) + ["--version"], text=True),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}, indent=2) + "\n")
    return executable, hashes


def contracts(executable):
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=90)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise AssertionError(result.stderr or "snprintf contract process failed")
    for fmt in ("%n", "prefix%n", "%1$n", "%hhn", "%lln", "%w32n"):
        result = subprocess.run([str(executable), "--fatal", fmt], text=True, capture_output=True, timeout=15)
        if result.returncode != -signal.SIGABRT or "FORTIFY: %n not allowed on Android" not in result.stderr:
            raise AssertionError("bounded %n lost precise fatal behavior: " + fmt + "\n" + result.stderr)
    result = subprocess.run([str(executable), "--fortify"], text=True, capture_output=True, timeout=15)
    if result.returncode != -signal.SIGABRT or "FORTIFY: vsnprintf: buffer overflow detected" not in result.stderr:
        raise AssertionError("checked entry lost fatal object-size guard\n" + result.stderr)
    print("snprintf bounds: six precise %n fatal controls and checked object-size guard passed", flush=True)


def negative_controls(folder, cxx, sanitize):
    original = (folder / "probe.cc").read_text()
    changes = [
        ("retained_count", "return static_cast<int>(s.Count());",
         "return static_cast<int>(s.bytes.size());", "snprintf complete count through truncation"),
        ("retained_byte", "out->bytes.resize(offset + kept);",
         "out->bytes.resize(offset + kept); out->bytes[offset] = '!';", "snprintf exact retained bytes"),
        ("terminator", "buf[s.bytes.size()] = 0;",
         "buf[s.bytes.size() + 1] = 0;", "snprintf terminator"),
        ("early_publish", "if (!valid) return -1;\n  if (n != 0) {",
         "if (!valid) { if (n) buf[0] = 0; return -1; }\n  if (n != 0) {",
         "invalid format leaves destination intact"),
    ]
    for name, before, after, assertion in changes:
        if original.count(before) != 1:
            raise AssertionError("Ambiguous negative-control boundary: " + name)
        source, executable = folder / (name + ".cc"), folder / name
        source.write_text(original.replace(before, after, 1))
        compile_probe(folder, source, executable, cxx, sanitize)
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=90)
        expected = "FAIL snprintf bounds: " + assertion
        if result.returncode != 1 or expected not in result.stderr:
            raise AssertionError("Mutant missed its contract assertion: " + name + "\n" + result.stderr)
        print("snprintf negative control detected:", name, assertion, flush=True)


def allocation_contracts(executable):
    # Output-storage bounds, not machine-dependent timing thresholds. glibc's
    # internal malloc and existing UTF-32-to-UTF-8 temporaries are outside this
    # narrow-string corpus and are not advertised as bounded by these receipts.
    budgets = {"tiny": 0, "short": 128, "truncated": 96, "sizing": 0,
               "literal": 96, "padding": 96, "medium": 1088,
               "complete": 1048640, "mixed": 192}
    for case, budget in budgets.items():
        result = subprocess.run([str(executable), "--benchmark", case, "3"],
                                text=True, capture_output=True, check=True, timeout=30)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        if len(rows) != 2 or [r["phase"] for r in rows] != ["first", "steady"]:
            raise AssertionError("Missing allocation receipts: " + case)
        for row in rows:
            if row["cpp_allocations"] is None:
                raise AssertionError("Allocation observation is required")
            if (row["cpp_peak_live_bytes"] > budget or
                    row["cpp_requested_bytes"] > budget * row["iterations"]):
                raise AssertionError("snprintf output-storage budget exceeded: " + case + " " + json.dumps(row))
    print("snprintf bounds: nine first-call/steady output-storage budgets passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--observe-allocations", action="store_true",
                        help="Separate C++ allocation receipts; do not use this build for CPU claims")
    parser.add_argument("--allocation-contracts", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--build-output", type=Path, help="Empty output directory retained for separate measurement")
    args = parser.parse_args()
    if args.allocation_contracts and not args.observe_allocations:
        parser.error("--allocation-contracts requires --observe-allocations")
    if platform.machine() not in ("x86_64", "amd64") or platform.system() != "Linux":
        raise SystemExit("This declared host ABI boundary requires Linux x64")
    if args.build_output:
        args.build_output.mkdir(parents=True, exist_ok=True)
        if any(args.build_output.iterdir()):
            raise SystemExit("--build-output must be empty")
        executable, hashes = build(args.build_output, args.source_ref, args.cxx, args.sanitize, args.observe_allocations)
        contracts(executable)
        if args.negative_controls:
            negative_controls(args.build_output, args.cxx, args.sanitize)
        if args.allocation_contracts:
            allocation_contracts(executable)
        (args.build_output / "source-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-snprintf-") as temporary:
            executable, hashes = build(Path(temporary), args.source_ref, args.cxx, args.sanitize, args.observe_allocations)
            contracts(executable)
            if args.negative_controls:
                negative_controls(Path(temporary), args.cxx, args.sanitize)
            if args.allocation_contracts:
                allocation_contracts(executable)
    print("SNPRINTF_SOURCE_HASHES " + json.dumps(hashes, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
