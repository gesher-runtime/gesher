#!/usr/bin/env python3
"""Run real ELF graphs through the complete production x86 loader on Linux.

Compiles elf_loader.cc, fs_rewrite.cc, tls.cc and x86_decode.cc unchanged, with
the explicit Win32 primitive boundary in tests/native/elf_loader_host_platform.
The normal graph executes real original ELF DSOs. A separate graph executable
redirects mprotect only in the production loader/rewriter translation units to
the driver's controlled failure boundary; the platform keeps real OS calls.
The ownership fixture includes the complete production rewriter for inspection.

--source-ref snapshots the complete historical compat/elf tree and uses current
fixtures/platform files. Its default suite is graph only, with fail-fast checks
so a historical contract failure cannot lead to execution of a partial handle.
No coordinator or allocator body is extracted, patched or copied into a model.
This runner does not establish Windows, ARM, VEH or TEB-based code execution.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_UNITS = ("elf_loader.cc", "fs_rewrite.cc", "tls.cc", "x86_decode.cc")
FIXTURES = (
    "elf_load_transaction_test.cc",
    "elf_rewrite_ownership_host.cc",
    "elf_loader_resource_ownership_host.cc",
    "elf_loader_host_platform.cc",
    "elf_loader_host_arm64.cc",
    "elf_load_transaction_graph.c",
    "build-load-transaction-graph.sh",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], label: str, *, timeout: int = 120,
        env: dict[str, str] | None = None) -> None:
    print(label, flush=True)
    subprocess.run(command, check=True, timeout=timeout, env=env)


def snapshot(folder: Path, source_ref: str | None,
             fixture_names: tuple[str, ...] = FIXTURES,
             fixture_directories: tuple[str, ...] = ()) -> tuple[Path, Path]:
    source = folder / "source"
    if source_ref:
        revision = subprocess.check_output(
            ["git", "rev-parse", "--verify", "--end-of-options", source_ref + "^{commit}"],
            cwd=ROOT, text=True).strip()
        archive = subprocess.check_output(
            ["git", "archive", "--format=tar", revision, "--", "compat/elf"], cwd=ROOT)
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as tree:
            for member in tree:
                relative = Path(member.name)
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("unexpected path in historical ELF snapshot")
                destination = source / relative
                if member.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    contents = tree.extractfile(member)
                    if contents is None:
                        raise ValueError("missing historical ELF file content")
                    destination.write_bytes(contents.read())
                else:
                    raise ValueError("historical ELF snapshot requires regular files")
        print("SOURCE_REF " + revision, flush=True)
    else:
        shutil.copytree(ROOT / "compat/elf", source / "compat/elf")
        print("SOURCE_REF working-tree snapshot (including uncommitted changes)", flush=True)

    # Snapshot inputs before compilation, so provenance identifies the exact
    # files used even while another contributor edits the shared worktree.
    fixtures = folder / "fixtures"
    fixtures.mkdir()
    for name in fixture_names:
        shutil.copy2(ROOT / "tests/native" / name, fixtures / name)
    for name in fixture_directories:
        shutil.copytree(ROOT / "tests/native" / name, fixtures / name)
    shutil.copytree(ROOT / "tests/native/elf_loader_host_platform",
                    fixtures / "elf_loader_host_platform")
    shutil.copytree(ROOT / "compat/arm64/include", fixtures / "arm64-include")

    elf = source / "compat/elf"
    tree_hashes = {str(path.relative_to(source)): digest(path)
                   for path in sorted(elf.rglob("*")) if path.is_file()}
    tree_digest = hashlib.sha256(json.dumps(tree_hashes, sort_keys=True).encode()).hexdigest()
    print("COMPLETE_ELF_TREE_SHA256 " + tree_digest, flush=True)
    for name in PRODUCTION_UNITS:
        path = elf / name
        print("PRODUCTION_TU_SHA256 " + str(path.relative_to(source)) + " " + digest(path), flush=True)
    for path in sorted(elf.rglob("*.h")):
        print("PRODUCTION_HEADER_SHA256 " + str(path.relative_to(source)) + " " + digest(path), flush=True)
    for path in sorted(fixtures.rglob("*")):
        if path.is_file():
            print("CURRENT_FIXTURE_SHA256 " + str(path.relative_to(fixtures)) + " " + digest(path), flush=True)
    return elf, fixtures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--sanitize", action="store_true", help="ASan/UBSan; LSan is disabled under ptrace")
    parser.add_argument("--source-ref", help="Git commit/ref for the complete historical production ELF tree")
    parser.add_argument("--suite", choices=("all", "graph", "rewrite", "resources"),
                        help="default: all for working tree, graph for --source-ref")
    args = parser.parse_args()
    if sys.platform != "linux" or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("this explicit host boundary requires Linux x86-64")
    suite = args.suite or ("graph" if args.source_ref else "all")
    cxx, cc = shlex.split(args.cxx), shlex.split(args.cc)
    if not cxx or not cc:
        parser.error("compiler commands must not be empty")
    print("BOUNDARY Linux x86-64; complete production ELF sources; explicit Win32 primitives; ARM unavailable",
          flush=True)
    for label, compiler in (("CXX", cxx), ("CC", cc)):
        version = subprocess.check_output(compiler + ["--version"], text=True).splitlines()[0]
        print(label + " " + json.dumps(compiler) + " " + version, flush=True)

    environment = os.environ.copy()
    flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-pthread"]
    link_flags = ["-pthread"]
    if args.sanitize:
        sanitizer = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        flags += sanitizer + ["-fno-pie"]
        link_flags += sanitizer + ["-no-pie"]
        options = environment.get("ASAN_OPTIONS", "")
        environment["ASAN_OPTIONS"] = options + (":" if options else "") + "detect_leaks=0"
        print("SANITIZERS address,undefined; fatal findings; detect_leaks=0 (ptrace boundary)", flush=True)

    with tempfile.TemporaryDirectory(prefix="gesher-elf-atomic-") as temporary:
        folder = Path(temporary)
        elf, fixtures = snapshot(folder, args.source_ref)
        includes = ["-I", str(fixtures / "elf_loader_host_platform"),
                    "-I", str(elf / "include"), "-I", str(elf),
                    "-I", str(fixtures / "arm64-include")]
        print("COMPILE_FLAGS " + json.dumps(flags + includes), flush=True)
        print("LINK_FLAGS " + json.dumps(link_flags), flush=True)
        objects = folder / "objects"
        objects.mkdir()

        def compile_unit(path: Path, name: str, defines: tuple[str, ...] = ()) -> Path:
            target = objects / (name + ".o")
            command = cxx + flags + includes + ["-D" + define for define in defines]
            run(command + ["-c", str(path), "-o", str(target)], "COMPILE " + name)
            return target

        def link(name: str, inputs: list[Path]) -> Path:
            target = folder / name
            run(cxx + [str(path) for path in inputs] + link_flags + ["-o", str(target)], "LINK " + name)
            return target

        common = [compile_unit(elf / "tls.cc", "tls"),
                  compile_unit(elf / "x86_decode.cc", "x86_decode"),
                  compile_unit(fixtures / "elf_loader_host_platform.cc", "host_platform")]
        if suite in ("all", "graph"):
            driver = fixtures / "elf_load_transaction_test.cc"
            driver_defines: tuple[str, ...] = ()
            if args.source_ref:
                if "GESHER_ELF_TRANSACTION_FAIL_FAST" not in driver.read_text(encoding="utf-8"):
                    raise ValueError("historical graph testing requires the driver's test-only fail-fast option")
                driver_defines = ("GESHER_ELF_TRANSACTION_FAIL_FAST",)
            graph = folder / "graph"
            compiler = folder / "cc-wrapper"
            compiler.write_text("#!/bin/sh\nexec " + shlex.join(cc) + ' "$@"\n', encoding="utf-8")
            compiler.chmod(0o700)
            run(["bash", str(fixtures / "build-load-transaction-graph.sh"), str(compiler), str(graph)],
                "BUILD original real ELF dependency graphs")
            graph_objects = common + [compile_unit(fixtures / "elf_loader_host_arm64.cc", "arm_unavailable")]
            normal = graph_objects + [compile_unit(elf / "elf_loader.cc", "elf_loader"),
                                      compile_unit(elf / "fs_rewrite.cc", "fs_rewrite"),
                                      compile_unit(driver, "graph_driver", driver_defines)]
            executable = link("elf-transaction", normal)
            run([str(executable), str(graph)], "RUN real graph with production protection calls", env=environment)

            # Macro substitution is confined to these two production TUs.
            # Driver/platform/TLS retain their real OS mprotect declarations.
            redirected = ("mprotect=gesher_elf_test_mprotect",)
            failure = graph_objects + [compile_unit(elf / "elf_loader.cc", "elf_loader_failure", redirected),
                                       compile_unit(elf / "fs_rewrite.cc", "fs_rewrite_failure", redirected),
                                       compile_unit(driver, "graph_failure_driver", driver_defines +
                                                    ("GESHER_ELF_TRANSACTION_FAILURE",))]
            executable = link("elf-transaction-protection-failure", failure)
            run([str(executable), str(graph)], "RUN real graph with controlled protection failure/retry", env=environment)

        if suite in ("all", "rewrite"):
            # This fixture includes fs_rewrite.cc itself for private-registry
            # inspection. Linking its ordinary object too would duplicate it.
            ownership = compile_unit(fixtures / "elf_rewrite_ownership_host.cc", "rewrite_ownership")
            executable = link("elf-rewrite-ownership", common + [ownership])
            run([str(executable)], "RUN complete production rewriter ownership contracts", env=environment)
        if suite in ("all", "resources"):
            # Explicit owner units exercise the complete production discard
            # path and its raw metadata. ELF parsing/transactions are above.
            ownership = compile_unit(fixtures / "elf_loader_resource_ownership_host.cc", "loader_ownership")
            resource_objects = common + [ownership,
                compile_unit(elf / "fs_rewrite.cc", "fs_resource_ownership"),
                compile_unit(fixtures / "elf_loader_host_arm64.cc", "arm_resource_ownership")]
            executable = link("elf-loader-ownership", resource_objects)
            run([str(executable)], "RUN complete production loader resource ownership", env=environment)
    print("ELF atomic-load host suite passed: " + suite + " (no Windows/ARM execution)", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print("ELF atomic-load command failed: " + shlex.join(error.cmd) +
              " (exit " + str(error.returncode) + ")", file=sys.stderr, flush=True)
        raise SystemExit(error.returncode if error.returncode > 0 else 1)
    except subprocess.TimeoutExpired as error:
        print("ELF atomic-load command timed out: " + shlex.join(error.cmd), file=sys.stderr, flush=True)
        raise SystemExit(124)
    except (OSError, ValueError) as error:
        print("ELF atomic-load runner: " + str(error), file=sys.stderr, flush=True)
        raise SystemExit(2)
