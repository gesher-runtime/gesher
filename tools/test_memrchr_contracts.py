#!/usr/bin/env python3
"""Exercise the actual Bionic reverse-byte search, checked export and fatal path.

The same C cases run in the imported Android libc probe. This host gate extracts
unmodified production functions, not a search/FORTIFY model. Guard allocation
uses the host OS; host execution does not prove Windows/ART or ARM64 execution.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "compat/bionic/posix_bionic.cc"
BASELINE_REF = "e505ad227523aaca566d05b8af6e466495c819cc"
BASELINE = """void* BIONIC_SYSV Memrchr(const void* s, int c, size_t n) {
  auto p = static_cast<const uint8_t*>(s);
  while (n-- > 0) {
    if (p[n] == static_cast<uint8_t>(c)) return const_cast<uint8_t*>(p + n);
  }
  return nullptr;
}"""

def function(source: str, name: str) -> str:
    pattern = (r"(?ms)^(?:__attribute__\(\(noinline\)\)\n)?(?:\[\[noreturn\]\] )?"
               r"(?:void\* BIONIC_SYSV |void )" + name + r"\([^)]*\) \{.*?^\}")
    matches = re.findall(pattern, source)
    if len(matches) != 1:
        raise ValueError("production function boundary changed: " + name)
    return matches[0]

def production(source: str, search: str | None = None) -> str:
    for export in ['E("memrchr", Memrchr);', 'E("__memrchr_chk", MemrchrChk);']:
        if source.count(export) != 1:
            raise ValueError("the actual export binding changed: " + export)
    selected = search or function(source, "Memrchr")
    helper = function(source, "MemrchrWords") + "\n" if "MemrchrWords(" in selected else ""
    return ("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
            "#include <cstdlib>\n#include <cstring>\n#define BIONIC_SYSV\nnamespace production {\n" +
            function(source, "FortifyFailure") + "\n" +
            helper + selected + "\n" +
            function(source, "MemrchrChk") + "\n}\n")

def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()

def run(command: list[str], output: Path, label: str, expected: int | None = 0) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    (output / (label + ".stdout")).write_text(result.stdout, encoding="utf-8")
    (output / (label + ".stderr")).write_text(result.stderr, encoding="utf-8")
    (output / (label + ".command.json")).write_text(json.dumps({
        "command": command, "returncode": result.returncode}, indent=2) + "\n", encoding="utf-8")
    if expected is not None and result.returncode != expected:
        raise RuntimeError(f"{label}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result

def build(compiler: list[str], flags: list[str], body: str, folder: Path, name: str,
          extra_objects: list[str] | None = None) -> Path:
    source = folder / (name + ".cc")
    source.write_text(body, encoding="utf-8")
    obj = folder / (name + ".o")
    executable = folder / (name + (".exe" if os.name == "nt" else ".bin"))
    run(compiler + flags + ["-fstack-usage", "-Wframe-larger-than=128", "-c", str(source), "-o", str(obj)],
        folder, name + "-compile")
    run(compiler + flags + [str(folder / "contracts.cc"), str(obj)] + (extra_objects or []) + ["-o", str(executable)],
        folder, name + "-link")
    return executable

def validate(args: argparse.Namespace, folder: Path) -> None:
    source = (ROOT / SOURCE).read_text(encoding="utf-8")
    if args.compare_ref:
        original = subprocess.check_output(["git", "show", f"{args.compare_ref}:{SOURCE}"], cwd=ROOT, text=True)
        if function(original, "Memrchr") != BASELINE:
            raise ValueError("the Git baseline differs from the frozen production function")
        for name in ("MemrchrChk", "FortifyFailure"):
            if function(original, name) != function(source, name):
                raise ValueError("comparison requires the unchanged actual " + name)
    template = (ROOT / "tests/native/memrchr_host.cc.in").read_text(encoding="utf-8")
    (folder / "contracts.cc").write_text(template, encoding="utf-8")
    compiler = shlex.split(args.cxx)
    flags = ["-std=c++20", "-O" + args.optimization, "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
             "-I", str(ROOT / "tests/native")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    elif os.name == "nt":
        flags += ["-static"]
    receipt = {"source_sha256": sha(source.encode()), "functions_sha256": sha(production(source).encode()),
               "baseline_ref": args.compare_ref or BASELINE_REF, "git_verified": bool(args.compare_ref),
               "sanitizers": args.sanitize, "optimization": args.optimization,
               "asan_options": os.environ.get("ASAN_OPTIONS"), "runs": []}
    for name, body in (("baseline", production(source, BASELINE)), ("candidate", production(source))):
        executable = build(compiler, flags, body, folder, name)
        result = run([str(executable)], folder, name + "-contracts")
        print(name + ": " + result.stdout.strip(), flush=True)
        fatal = run([str(executable), "fortify"], folder, name + "-fortify", expected=None)
        message = "gesher bionic: FORTIFY: memrchr: prevented buffer overflow"
        if message not in fatal.stderr or (os.name != "nt" and fatal.returncode != -signal.SIGABRT):
            raise RuntimeError("actual FORTIFY did not abort before search: " + fatal.stderr)
        if os.name == "nt" and fatal.returncode == 0:
            raise RuntimeError("actual Windows FORTIFY returned successfully")
        receipt["runs"].append({"name": name, "stdout": result.stdout,
                                "fortify_returncode": fatal.returncode, "source_sha256": sha(body.encode())})
    if args.negative_controls:
        negative_controls(compiler, flags, source, folder, args.sanitize, receipt)
    if args.reference_root:
        reference_contracts(compiler, flags, source, folder, args.reference_root.resolve(), receipt)
    (folder / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print("memrchr actual-source contract gate passed", flush=True)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--optimization", choices=("0", "1", "2", "3"), default="2")
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--compare-ref")
    parser.add_argument("--reference-root", type=Path,
                        help="optional pinned Android x86_64 supplier checkout; all nine source/header hashes are verified")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if os.name != "nt":
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=False)
        validate(args, args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="gesher-memrchr-") as temporary:
            validate(args, Path(temporary))

def negative_controls(compiler: list[str], flags: list[str], source: str,
                      folder: Path, sanitize: bool, receipt: dict) -> None:
    current = production(source)
    variants = [
        ("wrong_block_order", "for (size_t i = sizeof(word); i != 0; --i)",
         "for (size_t i = 1; i <= sizeof(word); ++i)", [], "FAIL memrchr returns the last occurrence within a word"),
        ("borrow_position", "if (((different - ones) & ~different & highs) != 0) {",
         "if (((different - ones) & ~different & highs) != 0) { return const_cast<uint8_t*>(p + n + "
         "(63 - __builtin_clzll((different - ones) & ~different & highs)) / 8);",
         [], "FAIL memrchr borrow flags do not select a different byte"),
        ("missing_tail", "while (n-- > 0)", "while (n-- > 1)", ["tail-boundary"],
         "FAIL word search preserves the remaining first byte"),
        ("wrong_int_conversion", "const uint8_t byte = static_cast<uint8_t>(c);", "const int byte = c;", [],
         "FAIL memrchr all bytes, alignments and match positions"),
        ("underread", "std::memcpy(&word, p + n, sizeof(word));", "std::memcpy(&word, p + n - 1, sizeof(word));",
         ["guard-boundary"], None),
        ("missing_fortify", 'if (n > buf_size) FortifyFailure("memrchr");',
         'if (n > buf_size && false) FortifyFailure("memrchr");', ["fortify"],
         "FAIL FORTIFY did not reject the oversized range"),
    ]
    for name, before, after, arguments, message in variants:
        if current.count(before) != (2 if name == "missing_tail" else 1):
            raise ValueError("negative control boundary changed: " + name)
        executable = build(compiler, flags, current.replace(before, after, 1), folder, name)
        result = run([str(executable)] + arguments, folder, name + "-negative", expected=None)
        if result.returncode == 0 or (message and message not in result.stderr):
            raise RuntimeError(f"{name} did not fail its intended assertion: {result.returncode}\n{result.stderr}")
        if name == "underread":
            expected_fault = ("AddressSanitizer" in result.stderr if sanitize else
                              result.returncode == (0xC0000005 if os.name == "nt" else -signal.SIGSEGV))
            if not expected_fault:
                raise RuntimeError("underread control failed for an unrelated reason: " + result.stderr)
        receipt["runs"].append({"name": name, "compiled": True, "returncode": result.returncode,
                                "expected_assertion": message, "stderr": result.stderr})
        print("negative " + name + ": rejected by its intended contract", flush=True)

def reference_contracts(compiler: list[str], flags: list[str], source: str,
                        folder: Path, reference: Path, receipt: dict) -> None:
    manifest = json.loads((ROOT / "tests/native/memrchr_android_reference.json").read_text(encoding="utf-8"))
    for path, expected in manifest["files"].items():
        if sha((reference / path).read_bytes()) != expected:
            raise ValueError("pinned Android supplier differs: " + path)
    # These are the actual namespace/public-packaging flags in the pinned
    # supplier's Android.bp. Its source and all support headers stay intact.
    reference_flags = flags + ["-I", str(reference), "-DLIBC_NAMESPACE=llvmlibc",
                              "-DLIBC_COPT_PUBLIC_PACKAGING",
                              "-DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_SYSTEM_INLINE"]
    obj = folder / "android-supplier.o"
    run(compiler + reference_flags + ["-MMD", "-MF", str(folder / "android-reference.d"),
        "-c", str(reference / "src/string/memrchr.cpp"), "-o", str(obj)], folder, "android-reference-compile")
    facade = """void* BIONIC_SYSV Memrchr(const void* s, int c, size_t n) {
  return llvmlibc::memrchr(s, c, n);
}"""
    body = '#include "src/string/memrchr.h"\n' + production(source, facade)
    executable = build(compiler, reference_flags, body, folder, "android-reference", [str(obj)])
    result = run([str(executable)], folder, "android-reference-contracts")
    receipt["reference"] = {"manifest": manifest, "stdout": result.stdout,
                            "scope": "actual pinned search supplier; checked-call facade retains Gesher FORTIFY"}
    print("Android x86_64 supplier: " + result.stdout.strip(), flush=True)

if __name__ == "__main__":
    main()
