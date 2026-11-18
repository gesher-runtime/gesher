#!/usr/bin/env python3
"""Package exact component executables with source, compiler, and input hashes."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"], cwd=root))
    if os.environ.get("GITHUB_ACTIONS") == "true" and (dirty or head != os.environ.get("GITHUB_SHA")):
        raise ValueError("Component producer must build its clean workflow commit")
    args.output.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for name in ("pipe_job_runner", "pipe_kernel_test", "pipe_native_process_test",
                 "ofd_native_child", "file_flags_ofd_test", "stdio_ofd_test"):
        path = args.output / (name + ".exe")
        shutil.copyfile(args.build / path.name, path)
        binaries[path.name] = digest(path)
        imports = subprocess.check_output([str(args.toolchain / "bin/llvm-readobj"),
                                           "--coff-imports", str(path)], text=True)
        (args.output / (path.name + ".imports.txt")).write_text(imports)
    selected = ["compat/posix", "compat/elf/exec", "tests/native", "tests/exec/ofd-native-child.cc",
                "tools/pipe-component", "cmake/toolchain-llvm-mingw.cmake", "aosp.lock",
                ".github/workflows/pipe-descriptors-component.yml"]
    files = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *selected], cwd=root)
    sources = {name: digest(root / name) for name in sorted(set(files.decode().strip("\0").split("\0"))) if name}
    compiler = args.toolchain / "bin/x86_64-w64-mingw32-clang++"
    receipt = {"schema": 1, "head": head, "dirty": dirty,
               "run_id": os.environ.get("GITHUB_RUN_ID"), "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
               "compiler": {"sha256": digest(compiler.resolve()), "version": subprocess.check_output([str(compiler), "--version"], text=True)},
               "sources": sources, "binaries": binaries,
               "aosp": json.loads((args.inputs / "component-inputs.json").read_text())}
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    shutil.copyfile(args.build / "compile_commands.json", args.output / "compile_commands.json")
    print(json.dumps({"head": head, "dirty": dirty, "sources": len(sources), "binaries": binaries}, indent=2))


if __name__ == "__main__":
    main()
