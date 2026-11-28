#!/usr/bin/env python3
"""Run the complete Bionic search implementation and test-only Android supplier."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "compat/bionic/search_bionic.cc"
FIXTURE = ROOT / "tests/native/search_delete_contract_host.cc"
REFERENCE = ROOT / "tests/native/reference/search"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--negative-controls", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory(prefix="search-delete-") if not args.output_dir else None
    out = Path(temporary.name) if temporary else args.output_dir.resolve()
    if not temporary:
        out.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which(args.compiler)
    if not compiler:
        raise RuntimeError("C++ compiler is unavailable")
    compiler_name = Path(compiler).name
    c_compiler = (str(Path(compiler).with_name(compiler_name[:-3] + "gcc"))
                  if compiler_name == "g++" or compiler_name.endswith("-g++")
                  else compiler.removesuffix("++"))
    (out / "driver-used.py").write_bytes(Path(__file__).read_bytes())
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "complete": False,
              "source_sha256": digest(SOURCE), "fixture_sha256": digest(FIXTURE),
              "shared_sha256": digest(ROOT / "tests/native/search_delete_contract.inc"),
              "commands": [], "positive": {}, "negative": {}, "hashes": {}}
    options = ["-O2" if args.sanitize else "-O3", "-g", "-Wall", "-Wextra", "-Werror", "-pthread"]
    if args.sanitize:
        options += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    includes = ["-I", str(ROOT / "compat/bionic"), "-I", str(ROOT / "tests/native")]
    wrap = ["-DTREE_DELETE_WRAP_ALLOCATOR", "-Wl,--wrap=malloc", "-Wl,--wrap=free"]

    def run(label, command, expected_failure=False):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                                timeout=120, env=os.environ.copy())
        row = {"label": label, "command": [str(x) for x in command], "exit": result.returncode,
               "stdout": result.stdout, "stderr": result.stderr, "expected_failure": expected_failure}
        report["commands"].append(row)
        (out / (label + ".stdout")).write_text(result.stdout)
        (out / (label + ".stderr")).write_text(result.stderr)
        if expected_failure:
            if result.returncode == 0 or not re.search(
                    r"SEARCH_DELETE_CONTRACT_FAILURE|AddressSanitizer|runtime error:", result.stderr):
                raise RuntimeError("Compiled negative control did not fail for its runtime contract: " + label)
        elif result.returncode != 0 or result.stderr:
            raise RuntimeError(label + " failed: " + result.stderr[-4000:])
        return result.stdout

    def positive(label, source, reference=False):
        binary = out / (label + ".bin")
        if reference:
            manifest = json.loads((REFERENCE / "manifest.json").read_text())
            objects = []
            for entry in manifest["files"]:
                path = REFERENCE / entry["file"]
                if digest(path) != entry["sha256"]:
                    raise RuntimeError("Reference source identity mismatch: " + str(path))
                obj = out / (entry["file"] + ".o")
                is_c = path.suffix == ".c"
                run("reference-" + entry["file"], [c_compiler if is_c else compiler,
                    "-std=c11" if is_c else "-std=c++20", "-D_GNU_SOURCE", *options,
                    "-c", path, "-o", obj])
                objects.append(obj)
            run(label + "-compile", [compiler, "-std=c++20", *options, *includes,
                "-DTREE_DELETE_ANDROID_REFERENCE", *wrap, FIXTURE, *objects, "-o", binary])
            report["reference"] = manifest
        else:
            run(label + "-compile", [compiler, "-std=c++20", *options, *includes, *wrap,
                                    FIXTURE, source, "-o", binary])
        text = run(label + "-contracts", [binary])
        match = re.fullmatch(r"SEARCH_DELETE_CONTRACT_PASS host_checks=(\d+) shared_checks=(\d+) trace=([0-9a-f]+)\n", text)
        if not match:
            raise RuntimeError("Incomplete contract receipt: " + label)
        report["positive"][label] = {"host_checks": int(match[1]), "shared_checks": int(match[2]),
                                     "trace": match[3], "binary_sha256": digest(binary)}

    try:
        report["compiler"] = run("compiler-version", [compiler, "--version"])
        candidate = out / "candidate.cc"
        candidate.write_bytes(SOURCE.read_bytes())
        positive("candidate", candidate)
        if args.baseline:
            baseline = out / "baseline.cc"
            baseline.write_bytes(args.baseline.read_bytes())
            positive("baseline", baseline)
            for field in ("shared_checks", "trace"):
                if report["positive"]["candidate"][field] != report["positive"]["baseline"][field]:
                    raise RuntimeError("Candidate changed complete public tree trace: " + field)
        positive("android-reference", None, reference=True)
        text = candidate.read_text()
        capacity = "std::array<Node**, 128> ancestors;"
        if text.count(capacity) != 1:
            raise RuntimeError("Reviewed route capacity changed")
        small = out / "small-route.cc"
        small.write_text(text.replace(capacity, "std::array<Node**, 2> ancestors;"))
        positive("small-route", small)
        for field in ("shared_checks", "trace"):
            if report["positive"]["small-route"][field] != report["positive"]["candidate"][field]:
                raise RuntimeError("Bounded route fallback changed the public tree trace: " + field)
        if args.negative_controls:
            start = text.index("void* BIONIC_SYSV Tdelete(")
            end = text.index("\nvoid BIONIC_SYSV Twalk(", start)
            body = text[start:end]
            mutations = {
                "no-ancestor-balance": ("*ancestor = Balance(*ancestor);", "(void)ancestor;", False),
                "wrong-parent": ("void* result = parent ? parent : node;", "void* result = parent ? tree_root : node;", False),
                "missing-success": ("if (node == nullptr) return nullptr;", "if (node == nullptr) return tree_root;", False),
                "lost-successor-key": ("node->right = RemoveMinimum(node->right, &node->key);",
                                       "const void* discarded = nullptr;\n    node->right = RemoveMinimum(node->right, &discarded);", False),
                "overflow-fallback-omitted": ("if (overflow) {", "if (false && overflow) {", True),
            }
            if args.sanitize:
                mutations["route-off-by-one"] = ("depth < ancestors.size()", "depth <= ancestors.size()", True)
            for label, (before, after, reduce_capacity) in mutations.items():
                if body.count(before) != 1:
                    raise RuntimeError("Negative-control source target changed: " + label)
                changed = text[:start] + body.replace(before, after) + text[end:]
                if reduce_capacity:
                    changed = changed.replace(capacity, "std::array<Node**, 2> ancestors;")
                mutant = out / (label + ".cc")
                mutant.write_text(changed)
                binary = out / (label + ".bin")
                run(label + "-compile", [compiler, "-std=c++20", *options, *includes, *wrap,
                                        FIXTURE, mutant, "-o", binary])
                run(label + "-contracts", [binary], expected_failure=True)
                report["negative"][label] = {"source_sha256": digest(mutant), "compiled": True,
                                            "failed_runtime_contract": True, "test_capacity": 2 if reduce_capacity else 128}
        report["complete"] = True
    except Exception as error:
        report["error"] = repr(error)
        raise
    finally:
        report["ended_utc"] = datetime.now(timezone.utc).isoformat()
        report["hashes"] = {path.name: digest(path) for path in sorted(out.iterdir()) if path.is_file()}
        (out / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"complete": report["complete"], "positive": report["positive"],
                      "negative_controls": len(report["negative"])}))
    if temporary:
        temporary.cleanup()


if __name__ == "__main__":
    main()
