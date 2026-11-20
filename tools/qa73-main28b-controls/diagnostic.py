#!/usr/bin/env python3
"""Current-main PR73 old/fixed controls; never a product acceptance substitute."""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
MAIN = "28b2617e35df63a1cb0fb7c9678b5d3f41b90a9a"
ORIGINAL = "de1e43c391275137c7b7b25dc170892684d55b5e"
BASE_TREE = "fd4cdc929de120a54e01551054712dfa0c4aefee"
FIXED_TREE = "a390e5cc97fce146df9fec99f451b73a60e48f06"
COMPILER_SHA = "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
REPAIRS = {
    "compat/posix/src/stdio_output.cc", "compat/posix/src/ofd.cc",
    "tests/native/ofd_transfer_host_test.cc.in",
    "tests/native/stdio_ofd_contract.inc", "tests/native/pipe_native_process_test.cc",
}
OVERLAY = {"tests/native/stdio_ofd_contract.inc", "tests/native/pipe_native_process_test.cc"}
DIAGNOSTIC = {
    "tools/qa73-main28b-controls/CMakeLists.txt",
    "tools/qa73-main28b-controls/diagnostic.py",
    "tools/qa73-main28b-controls/empty_stdio_options.cc",
    ".github/workflows/qa73-main28b-controls.yml",
}
SCOPES = ["compat/posix", "compat/elf/exec", "tests/native", "tests/exec",
          "tools/pipe-component", "tools/qa73-main28b-controls", "tools/ci",
          "cmake/toolchain-llvm-mingw.cmake", "aosp.lock", ".gitattributes",
          ".github/workflows/qa73-main28b-controls.yml"]
NAMES = ["pipe_job_runner", "stdio_ofd_test", "pipe_native_process_test",
         "ofd_native_child", "native_child_observed", "empty_stdio_options"]

# The bounded observation inserted below is generated from the exact original
# native child. Its original main/predicates remain byte-identical after the
# single call and this declared observation block are removed.
CHILD_HELPERS = '\n#include <cerrno>\n#include <cstdio>\n#include <cstdlib>\nstatic void qa73_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}\nstatic void qa73_child_handles(const char* mode) {\n  const int saved_errno = errno; const DWORD saved_error = GetLastError();\n  struct Errors { int error; DWORD win32; ~Errors() { errno = error; SetLastError(win32); } } restore{saved_errno, saved_error};\n  wchar_t path[32768];\n  DWORD length = GetEnvironmentVariableW(L"GESHER_QA73_CHILD_LOG", path, 32768);\n  if (!length || length >= 32768) return;\n  HANDLE log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,\n                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);\n  if (log == INVALID_HANDLE_VALUE) return;\n  const DWORD std_ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};\n  auto old_handler = _set_invalid_parameter_handler(qa73_invalid_parameter);\n  for (int fd = 0; fd < 3; ++fd) {\n    HANDLE handle = GetStdHandle(std_ids[fd]); DWORD flags = 0;\n    SetLastError(0); BOOL valid = GetHandleInformation(handle, &flags); DWORD validity_error = GetLastError();\n    SetLastError(0); DWORD type = GetFileType(handle), type_error = GetLastError();\n    errno = 0; intptr_t crt = _get_osfhandle(fd); int crt_errno = errno;\n    char text[384];\n    int n = snprintf(text, sizeof(text), "QA73 CHILD pid=%lu mode=%s fd=%d std=%p valid=%d validity_error=%lu type=%lu type_error=%lu crt=%lld crt_errno=%d original_reject=%d\\n",\n      GetCurrentProcessId(), mode, fd, handle, !!valid, validity_error, type, type_error,\n      (long long)crt, crt_errno, handle != nullptr && handle != INVALID_HANDLE_VALUE);\n    DWORD written = 0; if (n > 0 && (size_t)n < sizeof(text)) WriteFile(log, text, (DWORD)n, &written, nullptr);\n  }\n  _set_invalid_parameter_handler(old_handler);\n  CloseHandle(log); errno = saved_errno; SetLastError(saved_error);\n}\n'

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(*args: str, raw=False):
    result = subprocess.check_output(["git", *args], cwd=ROOT)
    return result if raw else result.decode().strip()

def source_proof() -> dict:
    history = [line.split() for line in git("log", "--first-parent", "-8", "--format=%H %T").splitlines()]
    base = [h for h, tree in history if tree == BASE_TREE]
    fixed = [h for h, tree in history if tree == FIXED_TREE]
    if len(base) != 1 or len(fixed) != 1:
        raise ValueError("Expected unique reviewed source trees in preserved first-parent history")
    base, fixed = base[0], fixed[0]
    if git("show", "-s", "--format=%P", base).split() != [MAIN, ORIGINAL]:
        raise ValueError("Baseline parent history changed")
    if git("show", "-s", "--format=%P", fixed) != base:
        raise ValueError("Fix is not additive to the exact reviewed baseline")
    if set(git("diff", "--name-only", base, fixed).splitlines()) != REPAIRS:
        raise ValueError("Unreviewed product delta")
    if set(git("diff", "--name-only", fixed, "HEAD").splitlines()) != DIAGNOSTIC:
        raise ValueError("Unexpected diagnostic-only delta")
    if git("show", f"{fixed}:compat/elf/exec/exec_spawn.cc") != git("show", f"{base}:compat/elf/exec/exec_spawn.cc"):
        raise ValueError("Empty-stdio product path must remain unfixed for observation")
    return {"base": base, "base_tree": BASE_TREE, "fixed": fixed, "fixed_tree": FIXED_TREE,
            "main": MAIN, "original73": ORIGINAL}

def paths() -> list[str]:
    names = git("ls-files", "-z", "--", *SCOPES).split("\0")
    return sorted(n for n in names if n)

def source_map() -> dict:
    # Git emits repository-relative slash paths on both hosts. Never normalize
    # arbitrary source bytes; .gitattributes requires the same actual LF bytes.
    names = paths()
    if any("\\" in p or Path(p).is_absolute() or ".." in Path(p).parts for p in names):
        raise ValueError("Noncanonical repository source path")
    return {p: sha(ROOT / p) for p in names}

def old_source_map(proof: dict) -> dict:
    result = {}
    for p in paths():
        if p in DIAGNOSTIC:
            continue
        rev = proof["fixed"] if p in OVERLAY else proof["base"]
        result[p] = hashlib.sha256(git("show", f"{rev}:{p}", raw=True)).hexdigest()
    return result

def generate(source: Path, out: Path) -> None:
    original = (source / "tests/exec/ofd-native-child.cc").read_text()
    if original.encode() != git("show", f"{ORIGINAL}:tests/exec/ofd-native-child.cc", raw=True):
        raise ValueError("Ordinary child assertions changed")
    marker = "int main(int argc, char** argv) {"
    if original.count(marker) != 1 or not CHILD_HELPERS:
        raise ValueError("Child observation boundary")
    call = '\n  qa73_child_handles(argc == 2 ? argv[1] : "bad-argc");'
    generated = original.replace(marker, CHILD_HELPERS + "\n" + marker + call, 1)
    if generated.replace(CHILD_HELPERS + "\n", "", 1).replace(call, "", 1) != original:
        raise ValueError("Original child did not recover exactly")
    out.mkdir(parents=True, exist_ok=True)
    (out / "ofd-native-child.cc").write_text(generated)
    (out / "generation.json").write_text(json.dumps({
        "original_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "original_predicates_exact": True,
        "scope": "Only three owned standard-handle/CRT observations; no environment or memory dump",
    }, indent=2) + "\n")

def build(work: Path, inputs: Path, toolchain: Path) -> None:
    proof = source_proof()
    input_receipt = json.loads((inputs / "component-inputs.json").read_text())
    if input_receipt["aosp_lock_sha256"] != sha(ROOT / "aosp.lock"):
        raise ValueError("Input source lock mismatch")
    for entry in input_receipt["inputs"]:
        if sha(inputs / entry["path"]) != entry["sha256"]:
            raise ValueError("Actual pinned input mismatch")
    work.mkdir(parents=True, exist_ok=True)
    old = work / "old-source"
    if old.exists():
        raise ValueError("Never overwrite an earlier diagnostic build")
    old.mkdir()
    archive = git("archive", proof["base"], raw=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(old, filter="data")
    for p in OVERLAY:
        (old / p).write_bytes(git("show", f"{proof['fixed']}:{p}", raw=True))
    expected_old = old_source_map(proof)
    if any(sha(old / p) != digest for p, digest in expected_old.items()):
        raise ValueError("Old production or same-assertion overlay differs")
    for mode, source in [("old", old), ("fixed", ROOT)]:
        folder = work / mode
        subprocess.run(["cmake", "-S", str(ROOT / "tools/qa73-main28b-controls"), "-B", str(folder),
                        "-G", "Ninja", "-DQA73_SOURCE_ROOT=" + str(source),
                        "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "cmake/toolchain-llvm-mingw.cmake"),
                        "-DLLVM_MINGW=" + str(toolchain), "-DAOSP_DIR=" + str(inputs),
                        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"], check=True)
        subprocess.run(["cmake", "--build", str(folder), "--parallel", "4",
                        "--target", *NAMES], check=True)
    (work / "source-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
    (work / "old-source-map.json").write_text(json.dumps(expected_old, indent=2) + "\n")

def package(work: Path, inputs: Path, toolchain: Path, archive: Path, out: Path) -> None:
    proof = source_proof()
    if sha(archive) != COMPILER_SHA:
        raise ValueError("Compiler differs from exact production toolchain")
    head = git("rev-parse", "HEAD")
    if git("status", "--porcelain") or (os.environ.get("GITHUB_ACTIONS") == "true" and head != os.environ["GITHUB_SHA"]):
        raise ValueError("Producer source must be clean and exact")
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("pipe_artifact", ROOT / "tools/pipe-component/artifact.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    binaries, parsers, generation = {}, {}, {}
    for mode in ("old", "fixed"):
        (out / mode).mkdir()
        for name in NAMES:
            key = mode + "/" + name + ".exe"
            shutil.copyfile(work / mode / "bin" / (name + ".exe"), out / key)
            binaries[key] = sha(out / key)
        shutil.copyfile(work / mode / "compile_commands.json", out / mode / "compile_commands.json")
        parsers[mode] = module.verify_stdio_parsers(toolchain, out / mode / "stdio_ofd_test.exe")
        generation[mode] = json.loads((work / mode / "generated/generation.json").read_text())
    receipt = {"head": head, "run_id": os.environ["GITHUB_RUN_ID"],
               "attempt": os.environ["GITHUB_RUN_ATTEMPT"], "source": proof,
               "source_bytes": source_map(), "old_source_bytes": old_source_map(proof),
               "binaries": binaries, "compiler_sha256": sha(archive),
               "compiler_version": subprocess.check_output(
                   [str(toolchain / "bin/x86_64-w64-mingw32-clang++"), "--version"], text=True),
               "inputs": json.loads((inputs / "component-inputs.json").read_text()),
               "stdio_real_parsers": parsers, "child_generation": generation}
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"head": head, "binaries": binaries, "source": proof}, indent=2))

def verify(out: Path) -> dict:
    receipt = json.loads((out / "receipt.json").read_text())
    for field, value in [("head", os.environ["GITHUB_SHA"]), ("run_id", os.environ["GITHUB_RUN_ID"]),
                         ("attempt", os.environ["GITHUB_RUN_ATTEMPT"])]:
        if receipt[field] != value:
            raise ValueError("Producer identity mismatch: " + field)
    proof = source_proof()
    if receipt["source"] != proof or receipt["source_bytes"] != source_map() or \
            receipt["old_source_bytes"] != old_source_map(proof) or receipt["compiler_sha256"] != COMPILER_SHA:
        raise ValueError("Strict source/compiler identity mismatch")
    for p, digest in receipt["binaries"].items():
        if sha(out / p) != digest:
            raise ValueError("Binary bytes differ from current producer: " + p)
    print("QA73_MAIN_SOURCE_AND_BINARY_BINDING_PASS", flush=True)
    return receipt

def run(out: Path) -> int:
    verify(out)
    results = out / "results"
    results.mkdir(exist_ok=True)
    rows, faults = [], []
    runner = out / "fixed/pipe_job_runner.exe"
    def owned(name, mode, executable, deadline, args=()):
        directory = results / name
        directory.mkdir()
        log = directory / "output.log"
        command = [str(runner), str(deadline), str(log), str(out / mode / (executable + ".exe")), *map(str, args)]
        completed = subprocess.run(command, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace", timeout=deadline / 1000 + 15)
        text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
        (directory / "controller.log").write_text(completed.stdout)
        print(completed.stdout, text, flush=True)
        row = {"name": name, "exit": completed.returncode, "output_sha256": sha(log) if log.exists() else None,
               "controller": completed.stdout, "deadline_ms": deadline}
        rows.append(row)
        if "empty=1" not in completed.stdout:
            faults.append(name + ": owned process tree did not retire")
        return completed.returncode, text
    observations = {}
    for mode in ("old", "fixed"):
        for kind, executable, deadline in [("stdio", "stdio_ofd_test", 60000),
                                            ("pipe", "pipe_native_process_test", 45000)]:
            try:
                code, output = owned(mode + "-" + kind, mode, executable, deadline)
                observations[mode + "-" + kind] = [code, output]
            except Exception as error:
                faults.append(mode + "-" + kind + ": " + str(error))
    expected_stdio = Counter(["wide stdio flush uses current append status",
                              "all wide span bytes share the owned output buffer"])
    expected_pipe = Counter({"import after last sender retirement preserves alias status and I/O": 2,
                             "retired-parent writer delivers actual data then last-writer EOF": 1})
    for kind, expected in [("stdio", expected_stdio), ("pipe", expected_pipe)]:
        if "old-" + kind not in observations or "fixed-" + kind not in observations:
            faults.append(kind + ": incomplete old/fixed observations")
            continue
        old_code, old = observations["old-" + kind]
        fixed_code, fixed = observations["fixed-" + kind]
        pattern = r"FAIL stdio: ([^\r\n]+)" if kind == "stdio" else r"FAIL pipe process: (.*?) errno="
        actual = Counter(re.findall(pattern, old))
        summary = r"(?:stdio OFD|pipe native process) contracts: (\d+) checks, (\d+) failures"
        old_summary, fixed_summary = re.search(summary, old), re.search(summary, fixed)
        ok = (old_code == 1 and fixed_code == 0 and actual == expected and old_summary and fixed_summary
              and old_summary[1] == fixed_summary[1] and int(old_summary[2]) == sum(expected.values())
              and fixed_summary[2] == "0" and not re.search(pattern, fixed))
        if kind == "stdio":
            ok = ok and fixed_summary and fixed_summary[1] == "180"
        if not ok:
            faults.append(kind + ": old failure or fixed same-assertion contract did not match")
    child_logs = results / "empty-children"
    child_logs.mkdir()
    try:
        code, output = owned("empty-options", "fixed", "empty_stdio_options", 70000,
                             (out / "fixed/native_child_observed.exe", child_logs))
        parsed = re.findall(r"EMPTY_STDIO_RESULT name=(\S+) create_error=(\d+) wait=(\d+) exit_read=(\d+) code=(\d+)", output)
        if code or len(parsed) != 6 or "EMPTY_STDIO_OBSERVATIONS_COMPLETE cases=6 completed=1" not in output:
            faults.append("empty stdio: six real process observations incomplete")
        cases = {name: {"create_error": int(error), "wait": int(wait), "exit_read": int(read), "code": int(status)}
                 for name, error, wait, read, status in parsed}
        if any(cases.get(name, {}).get("code") != 10 for name in ("production", "no-window-invalid")):
            faults.append("empty stdio: current original failure did not reproduce")
        for name in cases:
            path = child_logs / (name + ".log")
            if not path.exists() or len(re.findall(r"QA73 CHILD .* fd=[012] ", path.read_text())) != 3:
                faults.append("empty stdio: actual handle observations missing for " + name)
        (results / "empty-process-options.json").write_text(json.dumps(cases, indent=2) + "\n")
    except Exception as error:
        faults.append("empty stdio: " + str(error))
    summary = {"head": os.environ["GITHUB_SHA"], "run": os.environ["GITHUB_RUN_ID"],
               "attempt": os.environ["GITHUB_RUN_ATTEMPT"], "rows": rows, "faults": faults,
               "scope": "Old/fixed wide+gate contracts; empty-stdio options are observations only. Binder/full app/package runtime remain unqualified."}
    (results / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    return 1 if faults else 0

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("generate")
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p = sub.add_parser("build")
    for name in ("work", "inputs", "toolchain"): p.add_argument("--" + name, type=Path, required=True)
    p = sub.add_parser("package")
    for name in ("work", "inputs", "toolchain", "archive", "out"): p.add_argument("--" + name, type=Path, required=True)
    for command in ("verify", "run"):
        p = sub.add_parser(command); p.add_argument("--out", type=Path, required=True)
    sub.add_parser("source-proof")
    args = vars(parser.parse_args())
    command = args.pop("command")
    args = {k: v.resolve() if isinstance(v, Path) else v for k, v in args.items()}
    if command == "source-proof":
        print(json.dumps(source_proof(), indent=2)); return 0
    result = globals()[command](**args)
    return result if isinstance(result, int) else 0

if __name__ == "__main__":
    raise SystemExit(main())
