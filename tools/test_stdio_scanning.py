#!/usr/bin/env python3
"""Execute complete production stream ownership with the pinned MinGW parsers.

The C parser algorithms are unchanged. One exact host va_list initialization
uses va_copy/va_end because SysV's type is an array. Linux supplies FILE locks,
scalar ABI, locale and numeric conversion functions; descriptor I/O is real. Actual
Windows UCRT/LLP64, Winpthreads cancellation and the Bionic ABI remain CI gates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import urllib.request

from test_stdio_contracts import ROOT, adapter_source

# build-mingw-w64.sh in llvm-mingw's immutable 20261006 release selects this
# revision. Retain the original downloaded ZPL notices; no third-party source
# is copied into Gesher. The generated host copy has the ABI-only change below.
PARSER_REVISION = "a3d93999ef0521681d45e445c39964a7af0f593f"
PARSER_HASHES = {
    "mingw_sformat.c": "201c8a3f865757e68def9cb35328ce45cce7995f0359f33e969fbca6ce0cd137",
    "mingw_sformat.h": "3bbfce7c097c95e405162b99d76b60d8cd6105630e4f94dca726edc74c8ddb10",
    "mingw_swformat.c": "dcae5b6b1affaf81026498e55ea905ffa82181916123200f54fe98c9ad93356d",
    "mingw_swformat.h": "9257cee49d49bcfd3be3f64d68dc931fd9bf82dca89e963b7f94f24dde61985d",
    "mingw_vfscanf.c": "e79464dc1b414c723aede89abbb646bd8f990f7619d2f2a590452cbdd42c095b",
    "mingw_vfwscanf.c": "5ea43659ecbc0994ad185e905b546e566f5f5a1abe98dbc4e24f38879f825a26",
}


def replace_once(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError("source boundary changed: " + before[:100])
    return text.replace(before, after)


def parser_sources(folder: Path, supplied: Path | None) -> Path:
    source = supplied.resolve() if supplied else folder / "mingw-scanner"
    if not supplied:
        source.mkdir(exist_ok=True)
    for name, digest in PARSER_HASHES.items():
        path = source / name
        if not supplied:
            url = ("https://raw.githubusercontent.com/mingw-w64/mingw-w64/" +
                   PARSER_REVISION + "/mingw-w64-crt/stdio/" + name)
            with urllib.request.urlopen(url, timeout=60) as response:
                path.write_bytes(response.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError("pinned MinGW parser source differs: " + name)
    return source


def build(args, folder: Path, read_source, parser_objects=None):
    include = folder / "include"
    (include / "gesher").mkdir(parents=True, exist_ok=True)
    for name in ("stdio.h", "cancellation.h"):
        (include / "gesher" / name).write_bytes(read_source("compat/posix/include/gesher/" + name))
    for name in ("stdio_internal.h", "stdio_stream_buffer.h", "stdio_output_buffer.h"):
        (include / name).write_bytes(read_source("compat/posix/src/" + name))
    template = adapter_source(read_source)
    for name in ("stdio_internal.h", "stdio_stream_buffer.h"):
        template = replace_once(template, str(ROOT / "compat/posix/src" / name), str(include / name))
    # Replace only the declared glibc parser envelope, retaining the complete
    # production adapter and its actual public callback entry points.
    first = template.index("static ssize_t HostScanRead(")
    after = template.index("#define GESHER_STDIO_HOST_ENVELOPE", first)
    template = template[:first] + template[after:]
    for name in ("HostVfscanf", "HostSetvbuf"):
        template = replace_once(template, "static int " + name + "(", "[[maybe_unused]] static int " + name + "(")
    template = replace_once(template, "int main(int argc, char** argv) {", "int ExistingAdapterMain(int argc, char** argv) {")
    counters = """
static thread_local uint64_t scan_registry_visits = 0;
static thread_local uint64_t scan_state_locks = 0;
"""
    template = replace_once(template, "static int checks;", counters + "\nstatic int checks;")
    if not args.measure:
        template = replace_once(template, "StatePtr Find(FILE* stream, bool create) {",
                                "StatePtr Find(FILE* stream, bool create) {\n  ++scan_registry_visits;")
        template = replace_once(template,
                                "if (state_ && !unlocked) lock_ = std::unique_lock<std::recursive_mutex>(state_->mutex);",
                                "if (state_ && !unlocked) { ++scan_state_locks; lock_ = std::unique_lock<std::recursive_mutex>(state_->mutex); }")
    template += '\n#include "stdio_scan_host.inc"\n'
    generated = folder / "scan-adapter.cc"
    generated.write_text(template)
    flags = ["-O2", "-g", "-fno-builtin", "-fexceptions", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    # Name routing and declared host numeric/locale ABI boundaries only. The
    # same immutable parser control flow executes in both compared versions.
    parser_flags = ["-std=c11", "-D_GNU_SOURCE", "-D_WIN32", "-D_WIN64", "-D__cdecl=", "-D__MINGW_NOTHROW=", "-include", "wchar.h",
                    "-Dgetc=gesher_test_scan_getc", "-Dungetc=gesher_test_scan_ungetc",
                    "-Dgetwc=gesher_test_scan_getwc", "-Dungetwc=gesher_test_scan_ungetwc",
                    "-D__mingw_vfscanf=__real___mingw_vfscanf", "-D__mingw_vfwscanf=__real___mingw_vfwscanf",
                    "-D__mingw_strtof=strtof", "-D__mingw_strtold=strtold",
                    "-D__mingw_wcstof=wcstof", "-D__mingw_wcstod=wcstod", "-D__mingw_wcstold=wcstold"]
    source = None if parser_objects else parser_sources(folder, args.mingw_scanner_root)
    objects = parser_objects or []
    for name in (() if parser_objects else ("mingw_sformat.c", "mingw_swformat.c", "mingw_vfscanf.c", "mingw_vfwscanf.c")):
        obj = folder / (name + ".o")
        compilation_source = source / name
        if name in ("mingw_sformat.c", "mingw_swformat.c"):
            content = compilation_source.read_text()
            header = '#include "' + name.replace(".c", ".h") + '"'
            content = replace_once(content, header, header + "\nstatic void end_host_args(va_list* ap) { va_end(*ap); }\n")
            content = replace_once(content, "  va_list arg;", "  va_list arg __attribute__((cleanup(end_host_args)));")
            content = replace_once(content, "  arg = argp;", "  va_copy(arg, argp);")
            compilation_source = folder / (name + ".host.c")
            compilation_source.write_text("/* Gesher host test adaptation, 2026-11-23: SysV va_list copy/end only. */\n" + content)
        subprocess.run(shlex.split(args.cc) + flags + parser_flags + ["-I", str(source), "-c", str(compilation_source), "-o", str(obj)],
                       check=True, timeout=120)
        objects.append(str(obj))
    executable = folder / "stdio-scanning"
    subprocess.run(shlex.split(args.cxx) + flags + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-DGESHER_SCAN_MEASURE=" + ("1" if args.measure else "0"), "-I", str(include),
                   "-I", str(ROOT / "tests/native"), str(generated), *objects, "-o", str(executable)],
                   check=True, timeout=120)
    return executable, objects


def negative_controls(source: bytes):
    text = source.decode()
    cases = [
        ("narrow-scope", "nested", "  ScanScope callbacks(state.Get());\n  return __real___mingw_vfscanf",
         "  return __real___mingw_vfscanf", "nested scopes restore the outer pinned owner"),
        ("wide-scope", "nested", "  ScanScope callbacks(state.Get());\n  return __real___mingw_vfwscanf",
         "  return __real___mingw_vfwscanf", "nested scopes restore the outer pinned owner"),
        ("restore-scope", "restore", "static void Restore(void* raw) { scanning_stream = static_cast<ScanScope*>(raw)->previous_; }",
         "static void Restore(void*) {}", "after scan each public read finds its owner"),
        ("stream-identity", "other", "scanning_stream->stream == stream && scanning_stream->live",
         "(static_cast<void>(stream), scanning_stream->live)", "scanner callback keeps other-stream identity"),
        ("retired-owner", "retired", "scanning_stream->stream == stream && scanning_stream->live",
         "scanning_stream->stream == stream", "retired scanner owner cannot supply stale buffered bytes"),
        ("thread-isolation", "cancel", "thread_local State* scanning_stream = nullptr;",
         "State* scanning_stream = nullptr;", "another thread cannot borrow a scanner owner"),
    ]
    for name, case, before, after, expected in cases:
        yield name, case, replace_once(text, before, after).encode(), expected


def linux_reference(args, folder: Path):
    source = folder / "linux-reference.c"
    source.write_text('''#include <stdio.h>
static int checks, failures;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL Linux scanning: %s\\n", name); }
}
#include "stdio_scan_contract.inc"
int main(void) {
  test_stdio_scan_contracts(); test_stdio_scan_wide_contracts();
  printf("stdio scanning Linux reference: %d checks, %d failures\\n", checks, failures);
  return failures ? 1 : 0;
}
''')
    executable = folder / "linux-reference"
    subprocess.run(shlex.split(args.cc) + ["-std=c11", "-D_GNU_SOURCE", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-fno-builtin", "-I", str(ROOT / "tests/native"), str(source), "-o", str(executable)],
                   check=True, timeout=60)
    subprocess.run([str(executable)], cwd=folder, check=True, timeout=30)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--mingw-scanner-root", type=Path, help="Directory containing the six exact unmodified pinned parser files")
    parser.add_argument("--source-ref")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--measure", action="store_true", help="Uninstrumented timings; correctness corpus still runs")
    parser.add_argument("--measure-only", action="store_true", help="Report old-source work without enforcing the candidate work budget")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    if args.measure and args.sanitize:
        parser.error("timings must use an unsanitized build")
    if args.negative_controls and (args.source_ref or args.measure or args.measure_only or args.compile_only):
        parser.error("negative controls require the current instrumented gate")
    hashes = {}
    def read_source(path: str) -> bytes:
        content = (subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT)
                   if args.source_ref else (ROOT / path).read_bytes())
        hashes[path] = hashlib.sha256(content).hexdigest()
        return content
    with tempfile.TemporaryDirectory(prefix="gesher-stdio-scanning-") as temporary:
        folder = args.output_dir.resolve() if args.output_dir else Path(temporary)
        folder.mkdir(parents=True, exist_ok=True)
        executable, objects = build(args, folder, read_source)
        receipt = {"source_ref": args.source_ref, "sha256": hashes,
                   "parser_revision": PARSER_REVISION, "parser_sha256": PARSER_HASHES,
                   "instrumented_work": not args.measure, "sanitized": args.sanitize,
                   "host_parser_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                          for p in sorted(folder.glob("*.host.c"))}}
        (folder / "source.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if args.compile_only:
            print("Compiled actual adapter and pinned MinGW parser at " + str(executable), flush=True)
            return
        mode = "timing" if args.measure else "measure" if args.measure_only else "gate"
        result = subprocess.run([str(executable), mode], cwd=folder, check=False,
                                capture_output=True, text=True, timeout=120)
        (folder / "probe.log").write_text(result.stdout + result.stderr)
        print(result.stdout, end="")
        print(result.stderr, end="")
        result.check_returncode()
        linux_reference(args, folder)
        controls = []
        if args.negative_controls:
            path = "compat/posix/src/stdio_output.cc"
            for name, case, source, expected in negative_controls(read_source(path)):
                destination = folder / ("negative-" + name)
                destination.mkdir(exist_ok=True)
                def variant_read(relative):
                    return source if relative == path else read_source(relative)
                variant, _ = build(args, destination, variant_read, objects)
                failed = subprocess.run([str(variant), "gate", case], cwd=destination, check=False,
                                        capture_output=True, text=True, timeout=120)
                (destination / "probe.log").write_text(failed.stdout + failed.stderr)
                if failed.returncode != 1 or "FAIL: " + expected not in failed.stderr:
                    raise AssertionError("scanner control did not fail its intended assertion: " + name +
                                         "\n" + failed.stdout + failed.stderr)
                controls.append({"name": name, "source_sha256": hashlib.sha256(source).hexdigest(),
                                 "returncode": failed.returncode, "assertion": expected})
                print("PASS scanner negative control " + name + ": " + expected, flush=True)
        (folder / "controls.json").write_text(json.dumps(controls, indent=2) + "\n")


if __name__ == "__main__":
    main()
