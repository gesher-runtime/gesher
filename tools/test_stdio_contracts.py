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
    args = parser.parse_args()
    adapter = (ROOT / "compat/posix/src/stdio_output.cc").read_text()
    fsops = (ROOT / "compat/posix/src/fsops.cc").read_text()
    sockets = (ROOT / "compat/posix/src/fd.cc").read_text()
    adapter = adapter[adapter.index('extern "C" int __mingw_pformat'):]
    writer = 'return ::write(fd, bytes, count);'
    if adapter.count(writer) != 1:
        raise ValueError("production descriptor writer boundary changed")
    adapter = adapter.replace(writer, 'return HostDescriptorWrite(fd, bytes, count);')
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
        "@SOCKET_REGISTRY@": section(sockets, "struct SocketRegistry {", "// Takes the socket out"),
        "@SOCKET_INSERT@": section(sockets, "void Insert(", "// Publish a successful CRT replacement"),
        "@SOCKET_LOOKUP@": section(sockets, "bool LookupSocket(", "void RegisterDevice("),
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
        (include / "stdio.h").write_bytes((ROOT / "compat/posix/include/gesher/stdio.h").read_bytes())
        (include / "cancellation.h").write_bytes((ROOT / "compat/posix/include/gesher/cancellation.h").read_bytes())
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
                socket_destination = folder / "exit-socket.txt"
                completed = subprocess.run([str(executable), "--exit-socket", str(socket_destination)],
                                           cwd=folder, capture_output=True, text=True, timeout=45)
                if completed.returncode or socket_destination.read_bytes() != b"earlylate" or \
                        "SOCKET_EXIT_RETAINED after_ordinary_destructors=1" not in completed.stderr:
                    raise AssertionError("late stdio lost the live socket registry: " + completed.stderr)
                # Keep stderr occupied by a real read-only descriptor so socket
                # creation cannot reuse it. A failed diagnostic write must fail
                # the probe instead of silently claiming success.
                with open(os.devnull, "rb") as readonly_stderr:
                    unreported = subprocess.run(
                        [str(executable), "--exit-socket", str(folder / "unreported-socket.txt")],
                        cwd=folder, stdout=subprocess.PIPE, stderr=readonly_stderr,
                        text=True, timeout=45)
                if unreported.returncode != 89:
                    raise AssertionError(
                        "socket shutdown diagnostic I/O failure was not rejected: exit " +
                        str(unreported.returncode))
                print("stdio shutdown diagnostic I/O failure: rejected", flush=True)
                # Only the registry's lifetime changes in this negative. The
                # actual late stdio flush, map insertion/lookup and real files
                # remain identical; diagnose retirement before undefined access.
                lifetime = section(sockets, "SocketRegistry& Sockets() {", "// Lets the common case")
                negative = "SocketRegistry& Sockets() {\n  static SocketRegistry registry;\n  return registry;\n}\n"
                if template.count(lifetime) != 1:
                    raise ValueError("socket registry lifetime control boundary changed")
                negative_source = folder / "retired-socket.cc"
                negative_source.write_text(template.replace(lifetime, negative))
                negative_executable = folder / "retired-socket"
                subprocess.run(cxx + extra + [str(negative_source), "-o", str(negative_executable)], check=True, timeout=120)
                rejected = subprocess.run([str(negative_executable), "--exit-socket", str(folder / "negative-socket.txt")],
                                          cwd=folder, capture_output=True, text=True, timeout=45)
                if rejected.returncode != 87 or "SOCKET_EXIT_RETIRED before_final_stdio_write=1" not in rejected.stderr:
                    raise AssertionError("destructible socket-registry negative was not diagnosed: " + rejected.stderr)
                print("late stdio socket registry: live map PASS, actual shutdown-retirement negative rejected", flush=True)
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
