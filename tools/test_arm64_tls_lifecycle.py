#!/usr/bin/env python3
"""Exercise the real ARM64 TLS publication/attach/park lifecycle on Windows.

The native fixture copies production methods unchanged into an Env boundary
without a JIT. Optional --dll also runs real guest code and nested callbacks in
the built translator. --compile-only supports the Windows CI artifact path.
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
        raise ValueError("ARM64 TLS source boundary changed")
    return source[source.index(begin):source.index(end, source.index(begin))]


def render(source: str) -> str:
    sections = {
        "@TLS@": section(source, "constexpr size_t kTcbSize =", "// --- the exclusive monitor"),
        "@ATTACH@": section(source, "  void Attach() {", "  // The thread is exiting."),
        "@DETACH@": section(source, "  void Detach() {", "  // Whether another thread may reuse"),
        "@PARK@": section(source, "  void Park() {", "  // One Jit per nesting depth:"),
        "@LEASE@": section(source, "struct EnvRelease {", "uint64_t EnterGuest("),
    }
    template = (ROOT / "tests/native/arm64_tls_lifecycle_windows.cc.in").read_text(encoding="utf-8")
    for marker, body in sections.items():
        if template.count(marker) != 1:
            raise ValueError(f"ARM64 TLS fixture marker changed: {marker}")
        template = template.replace(marker, body)
    return template


def negative_controls(source: str):
    """Each mutant must fail its named assertion, not merely crash or time out."""
    growth = "if (!CommitTlsBlocks(std::max(g_tls_offset, off + reserved))) return -1;"
    loop = "for (TlsBlock& block : g_tls_blocks) {\n    if (required <= block.committed_size) continue;"
    trim = ("if (keep < block.committed_size &&\n"
            "      VirtualFree(block.base + keep, block.committed_size - keep, MEM_DECOMMIT))")
    rollback = "for (TlsBlock& block : g_tls_blocks) TrimTlsBlock(block, g_tls_offset);"
    mutations = [
        ("missing-attachment-trim", "TrimTlsBlock(g_tls_blocks.back(), g_tls_offset);",
         "(void)g_tls_blocks.back();", "active attachment commits only its assigned prefix"),
        ("missing-sleeping-thread-growth", loop, loop.replace("    if (required", "    if (block.owner_thread != GetCurrentThreadId()) continue;\n    if (required"),
         "registration grows every smaller block before exposing its offset"),
        ("partial-range-commit", "required - block.committed_size,\n                     MEM_COMMIT",
         "TlsCommitSize(kTcbSize),\n                     MEM_COMMIT", "entire registered range, including its last page"),
        ("publish-after-allocation-failure", growth,
         "(void)CommitTlsBlocks(std::max(g_tls_offset, off + reserved));", "failure in any live block rejects whole-tail registration"),
        ("lost-failed-decommit-record", trim,
         trim.replace("VirtualFree(", "(VirtualFree(").replace("MEM_DECOMMIT))", "MEM_DECOMMIT), true))"),
         "failed attachment trim keeps accurate bookkeeping"),
        ("missing-rollback-trim", rollback, "(void)g_tls_offset;", "live TLS has the exact page-rounded prefix"),
        ("overlapping-page-reclamation", rollback,
         "for (TlsBlock& block : g_tls_blocks) TrimTlsBlock(block, static_cast<size_t>(offset));",
         "all assigned TLS bytes stay committed"),
        ("missing-failed-growth-cleanup", "for (TlsBlock& prior : g_tls_blocks) TrimTlsBlock(prior, g_tls_offset);",
         "(void)required;", "live TLS has the exact page-rounded prefix"),
    ]
    for name, before, after, expected in mutations:
        if source.count(before) != 1:
            raise ValueError(f"ARM64 TLS mutation boundary changed: {name}")
        yield name, source.replace(before, after), expected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--source-ref")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--host-boundary", action="store_true",
                        help="Use explicit POSIX mapping boundaries; does not establish Windows commitment")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-controls", action="store_true")
    args = parser.parse_args()
    if args.compile_only and args.output is None:
        parser.error("--compile-only requires --output")
    if os.name != "nt" and not args.compile_only and not args.host_boundary:
        parser.error("this fixture exercises actual Windows virtual memory")
    if args.host_boundary and (os.name == "nt" or args.dll or args.compile_only):
        parser.error("--host-boundary is a POSIX component without an actual translator DLL")
    if args.negative_controls and (not args.host_boundary or args.measure or args.output):
        parser.error("--negative-controls requires the POSIX host gate without --measure or --output")
    path = "compat/arm64/dll/translator.cc"
    source = (subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=ROOT, text=True)
              if args.source_ref else (ROOT / path).read_text(encoding="utf-8"))
    print("translator.cc source SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    variants = [("production", source, None)]
    if args.negative_controls:
        variants.extend(negative_controls(source))
    with tempfile.TemporaryDirectory(prefix="gesher-arm64-tls-") as temporary:
        folder = Path(temporary)
        cpp = folder / "test.cc"
        executable = args.output.resolve() if args.output else folder / "test.exe"
        executable.parent.mkdir(parents=True, exist_ok=True)
        command = shlex.split(args.cxx) + ["-std=c++20", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                         "-fno-exceptions", "-pthread", "-I", str(ROOT / "compat/arm64/include"),
                                         "-I", str(ROOT / "compat/arm64/dll"),
                                         str(cpp), "-o", str(executable)]
        if args.host_boundary:
            command += ["-DGESHER_TLS_HOST_BOUNDARY", "-I", str(ROOT / "tests/native")]
        else:
            command += ["-static", "-lpsapi"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        for name, variant, expected in variants:
            cpp.write_text(render(variant), encoding="utf-8")
            subprocess.run(command, check=True, timeout=120)
            if args.compile_only:
                print(f"Compiled ARM64 TLS fixture: {executable}", flush=True)
            else:
                run = [str(executable), "measure" if args.measure else "gate"]
                if args.dll:
                    run.append(str(args.dll.resolve()))
                if expected is None:
                    subprocess.run(run, check=True, timeout=90)
                else:
                    result = subprocess.run(run, capture_output=True, text=True, timeout=90)
                    if result.returncode != 1 or expected not in result.stderr:
                        raise RuntimeError(f"negative control {name} did not fail its assertion:\n"
                                           + result.stdout + result.stderr)
                    print(f"PASS negative control {name}: {expected}", flush=True)


if __name__ == "__main__":
    main()
