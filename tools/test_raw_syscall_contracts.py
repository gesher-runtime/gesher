#!/usr/bin/env python3
"""Compile the production raw-syscall/regex adapter and exercise its public ABI."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "compat/bionic/syscall_bionic.cc")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source_dir = args.source.resolve().parent
    sources = [args.source, source_dir / "posix_regex.h"]
    sources += sorted((source_dir / "regex").glob("*.h"))
    sources += sorted((source_dir / "regex").glob("*.inc"))
    if (source_dir / "posix_regex_cnames.h").exists():
        sources.append(source_dir / "posix_regex_cnames.h")
    for source in sources:
        print(f"Production source {source.name} SHA256 " + hashlib.sha256(source.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix="gesher-raw-syscall-") as folder:
        executable = Path(folder) / "raw-syscall-contracts"
        # These fixtures check production ABI/ownership behavior, not a product throughput claim.
        command = shlex.split(args.cxx) + ["-std=c++20", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "compat/bionic/include"),
            "-I", str(ROOT / "compat/arm64/include"), "-I", str(ROOT / "compat/elf/include"),
            str(args.source), str(ROOT / "tests/native/raw_syscall_host_test.cc"), "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)], cwd=folder, timeout=30).returncode
        if result:
            return result
        budget = Path(folder) / "regex-budget-contracts"
        budget_command = shlex.split(args.cxx) + ["-std=c++20", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
            "-pthread", "-I", str(ROOT / "compat/bionic"),
            str(ROOT / "tests/native/regex_budget_host_test.cc"),
            "-Wl,--wrap=malloc", "-Wl,--wrap=free", "-o", str(budget)]
        if args.sanitize:
            budget_command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(budget_command, check=True, timeout=120)
        return subprocess.run([str(budget)], cwd=folder, timeout=30).returncode


if __name__ == "__main__":
    raise SystemExit(main())
