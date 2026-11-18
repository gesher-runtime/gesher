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
import tempfile


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
    for name in ("signal_job_runner", "realtime_signals_test", "realtime_signal_contract_test"):
        path = args.output / (name + ".exe")
        shutil.copyfile(args.build / path.name, path)
        binaries[path.name] = digest(path)
        imports = subprocess.check_output([str(args.toolchain / "bin/llvm-readobj"),
                                           "--coff-imports", str(path)], text=True)
        (args.output / (path.name + ".imports.txt")).write_text(imports)
    # Remove only our six diagnostic additions in an isolated temporary index.
    # The remaining complete tree must be exactly the frozen repair candidate.
    reviewed_tree = "8685910c14b598ea3addf957a63066fed9472767"
    diagnostic_paths = [".github/workflows/realtime-signal-component.yml", *[
        "tools/signal-component/" + name for name in
        ("CMakeLists.txt", "artifact.py", "job_runner.cc", "prepare-inputs.py", "run.ps1")]]
    if os.environ.get("GITHUB_ACTIONS") == "true":
        with tempfile.TemporaryDirectory(prefix="gesher-signal-tree-") as temp:
            env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / "index"))
            subprocess.run(["git", "read-tree", "HEAD"], cwd=root, env=env, check=True)
            subprocess.run(["git", "update-index", "--force-remove", "--", *diagnostic_paths],
                           cwd=root, env=env, check=True)
            actual = subprocess.check_output(["git", "write-tree"], cwd=root, env=env, text=True).strip()
            if actual != reviewed_tree:
                raise ValueError("Diagnostic branch changed the frozen signal repair")
    selected = ["compat/posix", "compat/bionic", "compat/common", "tests/native",
                "tests/exec/realtime_signals_contract.c", "tests/windows/realtime-signals-contracts.ps1",
                "tools/signal-component", "cmake/toolchain-llvm-mingw.cmake", "aosp.lock",
                ".github/workflows/realtime-signal-component.yml"]
    files = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *selected], cwd=root)
    sources = {name: digest(root / name) for name in sorted(set(files.decode().strip("\0").split("\0"))) if name}
    compiler = args.toolchain / "bin/x86_64-w64-mingw32-clang++"
    receipt = {"schema": 1, "head": head, "dirty": dirty, "reviewed_tree": reviewed_tree,
               "run_id": os.environ.get("GITHUB_RUN_ID"), "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
               "compiler": {"sha256": digest(compiler.resolve()), "version": subprocess.check_output([str(compiler), "--version"], text=True)},
               "sources": sources, "binaries": binaries,
               "aosp": json.loads((args.inputs / "component-inputs.json").read_text())}
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    shutil.copyfile(args.build / "compile_commands.json", args.output / "compile_commands.json")
    print(json.dumps({"head": head, "dirty": dirty, "sources": len(sources), "binaries": binaries}, indent=2))


if __name__ == "__main__":
    main()
