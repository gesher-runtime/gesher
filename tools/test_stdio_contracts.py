#!/usr/bin/env python3
"""Execute production stream buffering, adapter lifecycle and public stdio I/O.

The core and adapters use real Linux descriptors/files/pipes. The adapter
envelope replaces Win32 owner lookup and FILE allocation with declared Linux
boundaries; its formatter/scanner boundary is glibc, not MinGW. Actual Windows
linkage, MinGW formatting and the ELF ABI remain mandatory runtime CI gates.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def section(source: str, first: str, after: str) -> str:
    if source.count(first) != 1 or source.count(after) != 1:
        raise ValueError("ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(after, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-ref", help="Run the unchanged current fixture against exact historical adapter/fsops/header sources")
    args = parser.parse_args()
    def read_source(path: str) -> bytes:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT)
        return (ROOT / path).read_bytes()
    adapter = read_source("compat/posix/src/stdio_output.cc").decode()
    fsops = read_source("compat/posix/src/fsops.cc").decode()
    adapter = adapter[adapter.index('extern "C" int __mingw_pformat'):]
    # These are symbol-name changes for the host envelope, not altered bodies.
    for name in ("fseeko64", "ftello64"):
        marker = '__asm__("' + name + '")'
        if adapter.count(marker) != 1:
            raise ValueError("64-bit ABI alias changed: " + name)
        adapter = adapter.replace(marker, '__asm__("adapter_' + name + '")')
    names = """fwrite fread fputc putc putchar fgetc getc getchar fgets ungetc
      fputs puts fflush ferror feof clearerr setvbuf setbuf fseek ftell fgetpos
      fsetpos fgetpos64 fsetpos64 rewind perror vfprintf fprintf vprintf printf
      fputwc putwc putwchar fgetwc getwc getwchar ungetwc fputws fgetws fwide
      vfwprintf fwprintf vwprintf wprintf vfscanf fscanf vscanf scanf
      vfwscanf fwscanf vwscanf wscanf flockfile funlockfile ftrylockfile
      fread_unlocked fwrite_unlocked fgetc_unlocked getc_unlocked getchar_unlocked
      fputc_unlocked putc_unlocked putchar_unlocked fgets_unlocked fputs_unlocked
      fflush_unlocked ferror_unlocked feof_unlocked clearerr_unlocked
      fopen fdopen fopen64 fclose""".split()
    renames = "\n".join("#define " + name + " adapter_" + name for name in names)
    renames += "\n#define fseeko gesher_fseeko64\n#define ftello gesher_ftello64\n"
    sections = {
        "@RENAME_PUBLIC_SYMBOLS@": renames,
        "@STDIO_ADAPTER@": adapter,
        "@PARSE_MODE@": section(fsops, "bool ParseMode(", "// Random characters"),
        "@OPEN_CLOSE@": section(fsops, "FILE* fopen(", "// The stream is reopened"),
        "@STDIO_INTERNAL_HEADER@": str(ROOT / "compat/posix/src/stdio_internal.h"),
        "@STREAM_BUFFER_HEADER@": str(ROOT / "compat/posix/src/stdio_stream_buffer.h"),
    }
    template = (ROOT / "tests/native/stdio_adapter_host.cc.in").read_text()
    for marker, source in sections.items():
        if template.count(marker) != 1:
            raise ValueError("missing template boundary: " + marker)
        template = template.replace(marker, source)
        if marker in ("@STDIO_ADAPTER@", "@PARSE_MODE@", "@OPEN_CLOSE@"):
            print(marker.strip("@") + " SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-stdio-") as temporary:
        folder = Path(temporary)
        include = folder / "include/gesher"
        include.mkdir(parents=True)
        (include / "stdio.h").write_bytes(read_source("compat/posix/include/gesher/stdio.h"))
        (include / "cancellation.h").write_bytes(read_source("compat/posix/include/gesher/cancellation.h"))
        generated = folder / "adapter.cc"
        generated.write_text(template)
        cxx = shlex.split(args.cxx) + ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin", "-pthread"]
        if args.sanitize:
            cxx += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        cases = [
            ("core", ROOT / "tests/native/stdio_stream_buffer_test.cc", []),
            ("adapter", generated, ["-I", str(folder / "include"), "-I", str(ROOT / "tests/native")]),
        ]
        for name, source, extra in cases:
            executable = folder / name
            subprocess.run(cxx + extra + [str(source), "-o", str(executable)], check=True, timeout=120)
            subprocess.run([str(executable)], cwd=folder, check=True, timeout=45)
            if name == "adapter":
                destination = folder / "exit-output.txt"
                subprocess.run([str(executable), "--exit-output", str(destination)], cwd=folder, check=True, timeout=45)
                if destination.read_bytes() != b"earlylate":
                    raise AssertionError("stdio output after an older atexit callback was lost")
                print("stdio exit-handler ordering: passed", flush=True)
        reference = folder / "reference.c"
        reference.write_text('''
#include <stdio.h>
static int checks, failures;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL Linux stdio: %s\\n", name); }
}
#include "stdio_ofd_contract.inc"
int main(void) {
  test_stdio_ofd_contracts();
  printf("stdio Linux reference: %d checks, %d failures\\n", checks, failures);
  return failures ? 1 : 0;
}
''')
        executable = folder / "reference"
        subprocess.run(shlex.split(args.cc) + ["-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin", "-pthread", "-I", str(ROOT / "tests/native"), str(reference), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], cwd=folder, check=True, timeout=45)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
