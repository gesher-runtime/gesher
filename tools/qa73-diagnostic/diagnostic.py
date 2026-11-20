#!/usr/bin/env python3
"""Isolated observations of exact PR73 failures; never a product acceptance gate."""
from __future__ import annotations
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "de1e43c391275137c7b7b25dc170892684d55b5e"
TREE = "15a15ae18c209f7f2ef652647efb6f2e4e7f3c91"
OLD_RUN = 44672812748
OLD_JOB = 136084285734
OLD_ARTIFACT = 14603016878
OLD_SHA256 = "0254fe93f9d36f2fe29f344482f02aee7c4d9261903f197dfe770ac542045501"
OLD_SIZE = 116976228
ARCHIVE_SHA256 = "5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()

def one(text: str, before: str, after: str) -> str:
    if text.count(before) != 1:
        raise ValueError("Observation boundary is no longer unique: " + before[:80])
    return text.replace(before, after, 1)

STDIO_HELPERS = r'''
#include <errno.h>
// Diagnostic-only output goes directly to the inherited Win32 stderr. Preserve
// both error slots; no diagnostic FILE token or descriptor changes the fixture.
static void qa73_record(const char* text) {
  int saved_errno = errno; DWORD saved_error = GetLastError(), written = 0;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)strlen(text), &written, nullptr);
  errno = saved_errno; SetLastError(saved_error);
}
static int qa73_fflush(FILE* stream) {
  int result = fflush(stream), saved_errno = errno; DWORD saved_error = GetLastError();
  char text[160];
  snprintf(text, sizeof(text), "QA73 WIDE fflush=%d errno=%d win32=%lu\n", result, saved_errno, saved_error);
  qa73_record(text); errno = saved_errno; SetLastError(saved_error); return result;
}
'''
CHILD_HELPERS = r'''
#include <cerrno>
#include <cstdio>
#include <cstdlib>
static void qa73_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}
static void qa73_child_handles(const char* mode) {
  const int saved_errno = errno; const DWORD saved_error = GetLastError();
  struct Errors { int error; DWORD win32; ~Errors() { errno = error; SetLastError(win32); } } restore{saved_errno, saved_error};
  wchar_t path[32768];
  DWORD length = GetEnvironmentVariableW(L"GESHER_QA73_CHILD_LOG", path, 32768);
  if (!length || length >= 32768) return;
  HANDLE log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return;
  const DWORD std_ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  auto old_handler = _set_invalid_parameter_handler(qa73_invalid_parameter);
  for (int fd = 0; fd < 3; ++fd) {
    HANDLE handle = GetStdHandle(std_ids[fd]); DWORD flags = 0;
    SetLastError(0); BOOL valid = GetHandleInformation(handle, &flags); DWORD validity_error = GetLastError();
    SetLastError(0); DWORD type = GetFileType(handle), type_error = GetLastError();
    errno = 0; intptr_t crt = _get_osfhandle(fd); int crt_errno = errno;
    char text[384];
    int n = snprintf(text, sizeof(text), "QA73 CHILD pid=%lu mode=%s fd=%d std=%p valid=%d validity_error=%lu type=%lu type_error=%lu crt=%lld crt_errno=%d original_reject=%d\n",
      GetCurrentProcessId(), mode, fd, handle, !!valid, validity_error, type, type_error,
      (long long)crt, crt_errno, handle != nullptr && handle != INVALID_HANDLE_VALUE);
    DWORD written = 0; if (n > 0 && (size_t)n < sizeof(text)) WriteFile(log, text, (DWORD)n, &written, nullptr);
  }
  _set_invalid_parameter_handler(old_handler);
  CloseHandle(log); errno = saved_errno; SetLastError(saved_error);
}
'''

def generate(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    paths = ["tests/native/stdio_ofd_test.cc", "tests/native/stdio_ofd_contract.inc",
             "tests/exec/ofd-spawn-test.cc", "tests/exec/ofd-native-child.cc"]
    original = {p: (ROOT / p).read_text() for p in paths}
    for p in paths:
        if git("show", f"{SOURCE}:{p}") != original[p].rstrip("\n"):
            raise ValueError("Original fixture changed: " + p)
    contract = original[paths[1]]
    # Keep the original predicates, short-circuiting and all check() calls. Only
    # the one actual fflush call is observed, and pread's original result logged.
    contract = STDIO_HELPERS + contract
    contract = one(contract, 'fflush(stream) == 0 && stdio_bytes_equal(alias, "baseABC9")',
                   'qa73_fflush(stream) == 0 && stdio_bytes_equal(alias, "baseABC9")')
    before = "  return count == (ssize_t)wanted && memcmp(bytes, expected, wanted) == 0;"
    after = r'''  if (strcmp(expected, "baseABC9") == 0) {
    const int saved_errno = errno; const DWORD saved_error = GetLastError();
    char text[640]; int at = snprintf(text, sizeof(text), "QA73 WIDE pread=%lld errno=%d win32=%lu hex=", (long long)count, saved_errno, saved_error);
    for (ssize_t i = 0; i < count && i < (ssize_t)sizeof(bytes); ++i) at += snprintf(text + at, sizeof(text) - (size_t)at, "%02x", (unsigned char)bytes[i]);
    snprintf(text + at, sizeof(text) - (size_t)at, "\n"); qa73_record(text);
    errno = saved_errno; SetLastError(saved_error);
  }
''' + before
    contract = one(contract, before, after)
    (out / "stdio_ofd_contract.inc").write_text(contract)
    (out / "stdio_ofd_test.cc").write_text(original[paths[0]])
    spawn = one(original[paths[2]],
      "  bool completed = WaitForSingleObject(child->hProcess, 10000) == WAIT_OBJECT_0;",
      "  DWORD wait = WaitForSingleObject(child->hProcess, 10000);\n  bool completed = wait == WAIT_OBJECT_0;")
    spawn = one(spawn, "  CloseHandle(child->hProcess);", r'''  const int saved_errno = errno; const DWORD saved_error = GetLastError();
  std::printf("QA73 WAIT pid=%lu wait=%lu code=%lu completed=%d parent_errno=%d parent_win32=%lu\n",
      GetProcessId(child->hProcess), wait, code, completed, saved_errno, saved_error);
  std::fflush(stdout); errno = saved_errno; SetLastError(saved_error);
  CloseHandle(child->hProcess);''')
    (out / "ofd-spawn-test.cc").write_text(spawn)
    child = one(original[paths[3]], "int main(int argc, char** argv) {",
                CHILD_HELPERS + '\nint main(int argc, char** argv) {\n  qa73_child_handles(argc == 2 ? argv[1] : "bad-argc");')
    (out / "ofd-native-child.cc").write_text(child)
    # Both original executables are also run. These added observations do not
    # replace any existing check or its result.
    proof = {"source": SOURCE, "tree": TREE,
             "input_sha256": {p: sha(ROOT / p) for p in paths},
             "generated_sha256": {p.name: sha(p) for p in out.glob("*.cc")},
             "original_stdio_assertions": original[paths[0]].count('check("') + original[paths[1]].count('check("'),
             "original_spawn_assertions": original[paths[2]].count('Check("'),
             "unchanged_semantics": "Original predicates and 10000ms child deadline retained; added Win32 observations preserve error slots; original executables also built/run."}
    proof["generated_sha256"]["stdio_ofd_contract.inc"] = sha(out / "stdio_ofd_contract.inc")
    (out / "generation.json").write_text(json.dumps(proof, indent=2) + "\n")

def source_map() -> dict:
    paths = git("ls-files", "--", "compat/posix", "compat/elf/exec", "tests/native",
                "tests/exec", "tools/pipe-component", "tools/qa73-diagnostic", "tools/ci",
                "cmake/toolchain-llvm-mingw.cmake", "aosp.lock", "tests/e2e/apps.json",
                "tools/surface/download-verified-artifact.py",
                ".github/workflows/qa73-runtime-diagnostic.yml").splitlines()
    # Include newly authored diagnostic files during the local precommit build.
    paths += [str(p.relative_to(ROOT)) for p in (ROOT / "tools/qa73-diagnostic").rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    return {p: sha(ROOT / p) for p in sorted(set(paths))}

def package(build: Path, inputs: Path, toolchain: Path, archive: Path, out: Path) -> None:
    if sha(archive) != ARCHIVE_SHA256:
        raise ValueError("Compiler archive differs from the failed producer's pinned compiler")
    head = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    if os.environ.get("GITHUB_ACTIONS") == "true" and (dirty or head != os.environ["GITHUB_SHA"]):
        raise ValueError("Producer must use its exact clean workflow source")
    out.mkdir(parents=True, exist_ok=True)
    names = ["pipe_job_runner", "stdio_ofd_test", "stdio_ofd_observed", "ofd_spawn_original",
             "ofd_spawn_observed", "ofd_native_child", "native_child_observed", "puzzles_pipe_probe", "binder_observer"]
    binaries = {}
    for n in names:
        p = out / (n + ".exe"); shutil.copyfile(build / "bin" / p.name, p); binaries[p.name] = sha(p)
    spec = importlib.util.spec_from_file_location("pipe_artifact", ROOT / "tools/pipe-component/artifact.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    parsers = {n: module.verify_stdio_parsers(toolchain, out / (n + ".exe")) for n in ["stdio_ofd_test", "stdio_ofd_observed"]}
    receipt = {"head": head, "dirty": dirty, "run_id": os.environ.get("GITHUB_RUN_ID"),
               "attempt": os.environ.get("GITHUB_RUN_ATTEMPT"), "baseline": SOURCE,
               "compiler_archive_sha256": sha(archive), "compiler_version": subprocess.check_output([str(toolchain / "bin/x86_64-w64-mingw32-clang++"), "--version"], text=True),
               "sources": source_map(), "binaries": binaries, "stdio_parsers": parsers,
               "inputs": json.loads((inputs / "component-inputs.json").read_text()),
               "generation": json.loads((build / "generated/generation.json").read_text())}
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    shutil.copyfile(build / "compile_commands.json", out / "compile_commands.json")
    print(json.dumps({"head": head, "dirty": dirty, "sources": len(receipt["sources"]), "binaries": binaries}, indent=2))

def verify(out: Path) -> None:
    receipt = json.loads((out / "receipt.json").read_text())
    for key, expected in [("head", os.environ["GITHUB_SHA"]), ("run_id", os.environ["GITHUB_RUN_ID"]), ("attempt", os.environ["GITHUB_RUN_ATTEMPT"])]:
        if receipt[key] != expected: raise ValueError("Producer mismatch: " + key)
    if receipt["dirty"] or receipt["sources"] != source_map(): raise ValueError("Producer source mismatch")
    for name, digest in receipt["binaries"].items():
        if sha(out / name) != digest: raise ValueError("Binary mismatch: " + name)
    print("QA73_SOURCE_BINARY_BINDING_VERIFIED")

def api(path: str) -> dict:
    if os.environ["GITHUB_REPOSITORY"] != "gesher-runtime/gesher":
        raise ValueError("Diagnostic repository mismatch")
    sys.path.insert(0, str(ROOT / "tools/ci"))
    from github_read import get_json
    return get_json("repos/gesher-runtime/gesher" + path)

def historical(out: Path) -> None:
    # A fixed failed control is deliberately fetched by exact ID. It is never
    # represented as a binary built from this diagnostic or from new main.
    run, job, artifact = api(f"/actions/runs/{OLD_RUN}"), api(f"/actions/jobs/{OLD_JOB}"), api(f"/actions/artifacts/{OLD_ARTIFACT}")
    if run["head_sha"] != SOURCE or run["run_attempt"] != 1 or run["status"] != "completed": raise ValueError("Historical run changed")
    if job["run_id"] != OLD_RUN or job["run_attempt"] != 1 or job["head_sha"] != SOURCE or job["name"] != "build" or job["status"] != "completed" or job["conclusion"] != "success": raise ValueError("Historical producer mismatch")
    if artifact["name"] != "windows-x64" or artifact["expired"] or artifact["workflow_run"]["id"] != OLD_RUN or artifact["workflow_run"]["head_sha"] != SOURCE or artifact["digest"] != "sha256:" + OLD_SHA256 or artifact["size_in_bytes"] != OLD_SIZE: raise ValueError("Historical archive mismatch")
    stamp = lambda value: datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if not stamp(job["started_at"]) <= stamp(artifact["created_at"]) <= stamp(job["completed_at"]):
        raise ValueError("Historical output is outside its verified producer interval")
    out.mkdir(parents=True, exist_ok=True)
    # Reuse the qualified downloader: API credentials are never forwarded to
    # signed storage redirects; the raw ZIP digest and member paths are checked
    # before extraction. Metadata/source/producer predicates remain above.
    subprocess.run([sys.executable, str(ROOT / "tools/surface/download-verified-artifact.py"),
                    "--id", str(OLD_ARTIFACT), "--digest", "sha256:" + OLD_SHA256,
                    "--destination", str(out / "bin")], check=True, cwd=ROOT)
    if api(f"/actions/jobs/{OLD_JOB}") != job or api(f"/actions/artifacts/{OLD_ARTIFACT}") != artifact: raise ValueError("Historical producer changed during read")
    bins = {p.name: sha(p) for p in (out / "bin").glob("*.exe")}
    (out / "historical-receipt.json").write_text(json.dumps({"run": run, "producer": job, "artifact": artifact, "zip_sha256": OLD_SHA256, "verified_downloader_sha256": sha(ROOT / "tools/surface/download-verified-artifact.py"), "binaries": bins, "purpose": "Exact historical failed control only"}, indent=2) + "\n")
    print(f"QA73_HISTORICAL source={SOURCE} run={OLD_RUN} attempt=1 producer={OLD_JOB} artifact={OLD_ARTIFACT} sha256={OLD_SHA256}")

def main() -> None:
    p = argparse.ArgumentParser(); p.add_argument("operation", choices=["generate", "package", "verify", "historical"])
    p.add_argument("--out", type=Path, required=True)
    for name in ["build", "inputs", "toolchain", "archive"]: p.add_argument("--" + name, type=Path)
    a = p.parse_args()
    if a.operation == "generate": generate(a.out)
    elif a.operation == "package": package(a.build, a.inputs, a.toolchain, a.archive, a.out)
    elif a.operation == "verify": verify(a.out)
    else: historical(a.out)

if __name__ == "__main__": main()
