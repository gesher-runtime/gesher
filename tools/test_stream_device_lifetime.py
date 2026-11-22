#!/usr/bin/env python3
"""Exercise production stream/device ownership with real Linux FILE reuse.

Extract actual OFD device adapters, descriptor retirement, fclose/freopen and
Bionic stream readers. The shared OFD/memfd registries, Windows CRT/path/record locks,
stdio backend and device data are declared boundaries; their complete owners
run separately in the OFD/stdio corpora and actual Windows fixtures. The shared
public fixture also runs without adapters against Linux. This is not Windows
or Android ELF runtime qualification.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def section(source: str, first: str, following: str) -> str:
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-source", type=Path, default=ROOT / "compat/posix/src/fsops.cc")
    parser.add_argument("--descriptor-source", type=Path, default=ROOT / "compat/posix/src/fd.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    host = args.host_source.read_text()
    devices = args.descriptor_source.read_text()
    shim = (ROOT / "compat/bionic/stdio_bionic.cc").read_text()
    sections = {
        "@DEVICE_MAP@": section(devices, "// Stateless logical devices live", "int ReadDevice("),
        "@DEVICE_REGISTRATION@": section(devices, "void RegisterDevice(", "}  // namespace gesher"),
        "@RETIRE_DESCRIPTOR@": section(devices, "void RetiredDescriptor::Retire(", "int RetiredDescriptor::Finish("),
        "@PARSE_MODE@": section(host, "bool ParseMode(", "// Random characters"),
        "@FCLOSE@": section(host, "int gesher_fclose_state(", "// The stream is reopened"),
        "@FREOPEN@": section(host, "FILE* gesher_freopen_state(", "char* mkdtemp("),
        "@STREAM_STATE@": section(shim, "constexpr uint32_t kWeof", "class StreamLock"),
        "@READ_STREAM@": section(shim, "size_t ReadStream(", "// fopen through"),
        "@GETC_UNLOCKED@": section(shim, "int BIONIC_SYSV GetcUnlocked(", "int BIONIC_SYSV PutcUnlocked("),
        "@FREAD_UNLOCKED@": section(shim, "size_t BIONIC_SYSV FreadUnlocked(", "size_t BIONIC_SYSV FwriteUnlocked("),
        "@FGETC@": section(shim, "int BIONIC_SYSV Fgetc(", "int BIONIC_SYSV Getchar("),
        "@FREAD@": section(shim, "size_t BIONIC_SYSV Fread(", "size_t BIONIC_SYSV Fwrite("),
    }
    sections["@FREOPEN@"] = sections["@FREOPEN@"].replace(
        "return freopen(path, mode, stream);", "return host::freopen(path, mode, stream);")
    # Both public and unlocked guest readers now call the declared shared-stdio
    # boundary directly. Keep their production bodies unchanged.
    template = (ROOT / "tests/native/stream_device_host_test.cc.in").read_text()
    for marker, contents in sections.items():
        if template.count(marker) != 1:
            raise ValueError("invalid template marker: " + marker)
        template = template.replace(marker, contents)
    with tempfile.TemporaryDirectory(prefix="gesher-stream-devices-") as temporary:
        folder = Path(temporary)
        generated = folder / "stream_device_test.cc"
        executable = folder / "stream_device_test"
        generated.write_text(template)
        command = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-builtin",
            "-I", str(ROOT / "tests/native"), str(generated), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
        adapter = subprocess.run([str(executable)], cwd=folder, env=env, timeout=30)
        reference = folder / "stream_device_reference.c"
        reference.write_text('''
#include <stdio.h>
static int checks, failures;
static void check(const char* name, int ok) {
  ++checks;
  if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\\n", name); }
}
#include "stream_device_contract_test.inc"
int main(void) {
  test_stream_device_contracts();
  printf("stream devices Linux: %d checks, %d failures\\n", checks, failures);
  return failures != 0;
}
''')
        reference_exe = folder / "stream_device_reference"
        subprocess.run(shlex.split(args.cc) + [
            "-std=c11", "-D_GNU_SOURCE", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "tests/native"), str(reference), "-o", str(reference_exe),
        ], check=True, timeout=60)
        independent = subprocess.run([str(reference_exe)], cwd=folder, timeout=30)
        return int(adapter.returncode != 0 or independent.returncode != 0)


if __name__ == "__main__":
    raise SystemExit(main())
