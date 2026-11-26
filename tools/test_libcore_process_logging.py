#!/usr/bin/env python3
"""Check actual UNIXProcess exit logging with a competing app-output writer.

Extract the complete production waiter and exit-status mapper. The host fixture
replaces JNI/Win32/CRT boundaries, deterministically schedules an app writer
after the first sink write, and models fragmented stdio emission. It does not
replace the producer, its format, child cleanup, or exit-code mapping. The
production argument summary of the start and failure records (bounded, quoted,
one line) is extracted and checked as well.

--source-root runs this same fixture against another source tree (the original
fprintf producer is the negative control). Real Windows E2E retains the exact
exit marker and its existing deadline.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def between(source, begin, end):
    if source.count(begin) != 1 or source.count(end) != 1:
        raise ValueError("ambiguous production boundary: " + begin)
    start = source.index(begin)
    return source[start:source.index(end, start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    process = (args.source_root / "compat/libcore/process.cc").read_text()
    spawn = (args.source_root / "compat/elf/exec/exec_spawn.cc").read_text()
    pieces = {
        "@WAIT_FOR_PROCESS_EXIT@": between(
            process, "jint JNICALL WaitForProcessExit(", "// Process.destroy():"),
        "@EXIT_STATUS@": between(spawn, "int ExitStatus(DWORD code)", "int WaitStatus(DWORD code)"),
        "@EXEC_LOG_ARGUMENTS@": between(
            process, "constexpr size_t kExecLogArgumentBytes", "void ThrowIOException("),
    }
    for name, source in [("process.cc", process), ("exec_spawn.cc", spawn)]:
        print(name, hashlib.sha256(source.encode()).hexdigest(), flush=True)
    source = (ROOT / "tests/exec/process_logging_host_test.cc.in").read_text()
    for marker, body in pieces.items():
        if source.count(marker) != 1:
            raise ValueError("ambiguous fixture marker: " + marker)
        source = source.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix="gesher-process-logging-") as folder:
        folder = Path(folder)
        unit = folder / "process_logging.cc"
        output = folder / "process_logging"
        unit.write_text(source)
        command = shlex.split(args.cxx) + [
            "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
            str(unit), "-o", str(output)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-fno-sanitize-recover=all"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
        print(result.stdout, end="", flush=True)
        if result.returncode:
            print(result.stderr[:12000], end="", flush=True)
        return int(result.returncode != 0)


if __name__ == "__main__":
    raise SystemExit(main())
