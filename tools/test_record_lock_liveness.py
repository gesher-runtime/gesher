#!/usr/bin/env python3
"""Contract tests for compat/posix/src/record_lock_liveness.cc (PR #313).

Compiles the complete, unmodified production record_lock_liveness.cc and
ErrnoFromWin32 against a controlled host Win32 boundary (without modifying any
hash-pinned POSIX source file) and exercises:
  - FileIdInfo primary path (64-bit volume serial + 128-bit file identifier),
  - Clean fallback to GetFileInformationByHandle on FAT32/exFAT/UDF volumes
    returning ERROR_INVALID_PARAMETER, ERROR_INVALID_FUNCTION, or ERROR_NOT_SUPPORTED,
  - kLegacyFileIndex (UINT64_MAX) upper-half sentinel disambiguation,
  - Guard/sentinel preservation on null/invalid/non-disk handles and Win32 errors,
  - Process/thread/object liveness probes and RegistryState pruning under ASan/UBSan.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

WINDOWS_HOST_HEADER = """#pragma once
#include <cstddef>
#include <cstdint>
#include <cwchar>

extern "C" inline int gesher_errno_from_crt(int err) { return err; }

using DWORD = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using BOOL = int;
using ULONGLONG = uint64_t;
using ULONG_PTR = uintptr_t;
using HANDLE = void*;
using HMODULE = void*;
using FARPROC = void (*)();

#define WINAPI
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))

constexpr BOOL FALSE = 0;
constexpr BOOL TRUE = 1;
constexpr DWORD INFINITE = 0xffffffffU;
constexpr DWORD WAIT_OBJECT_0 = 0U;
constexpr DWORD WAIT_TIMEOUT = 258U;
constexpr DWORD SYNCHRONIZE = 0x00100000U;
constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000U;
constexpr DWORD THREAD_QUERY_LIMITED_INFORMATION = 0x0800U;

constexpr DWORD FILE_TYPE_UNKNOWN = 0x0000U;
constexpr DWORD FILE_TYPE_DISK = 0x0001U;
constexpr DWORD FILE_TYPE_CHAR = 0x0002U;
constexpr DWORD FILE_TYPE_PIPE = 0x0003U;

constexpr DWORD ERROR_SUCCESS = 0U;
constexpr DWORD ERROR_INVALID_FUNCTION = 1U;
constexpr DWORD ERROR_FILE_NOT_FOUND = 2U;
constexpr DWORD ERROR_PATH_NOT_FOUND = 3U;
constexpr DWORD ERROR_TOO_MANY_OPEN_FILES = 4U;
constexpr DWORD ERROR_ACCESS_DENIED = 5U;
constexpr DWORD ERROR_INVALID_HANDLE = 6U;
constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8U;
constexpr DWORD ERROR_OUTOFMEMORY = 14U;
constexpr DWORD ERROR_INVALID_DRIVE = 15U;
constexpr DWORD ERROR_NOT_SAME_DEVICE = 17U;
constexpr DWORD ERROR_WRITE_PROTECT = 19U;
constexpr DWORD ERROR_GEN_FAILURE = 31U;
constexpr DWORD ERROR_SHARING_VIOLATION = 32U;
constexpr DWORD ERROR_LOCK_VIOLATION = 33U;
constexpr DWORD ERROR_HANDLE_DISK_FULL = 39U;
constexpr DWORD ERROR_NOT_SUPPORTED = 50U;
constexpr DWORD ERROR_BAD_NETPATH = 53U;
constexpr DWORD ERROR_BAD_NET_NAME = 67U;
constexpr DWORD ERROR_FILE_EXISTS = 80U;
constexpr DWORD ERROR_INVALID_PARAMETER = 87U;
constexpr DWORD ERROR_BROKEN_PIPE = 109U;
constexpr DWORD ERROR_DISK_FULL = 112U;
constexpr DWORD ERROR_CALL_NOT_IMPLEMENTED = 120U;
constexpr DWORD ERROR_INVALID_NAME = 123U;
constexpr DWORD ERROR_MOD_NOT_FOUND = 126U;
constexpr DWORD ERROR_PROC_NOT_FOUND = 127U;
constexpr DWORD ERROR_DIR_NOT_EMPTY = 145U;
constexpr DWORD ERROR_BAD_PATHNAME = 161U;
constexpr DWORD ERROR_ALREADY_EXISTS = 183U;
constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206U;
constexpr DWORD ERROR_NO_DATA = 232U;
constexpr DWORD ERROR_DIRECTORY = 267U;
constexpr DWORD ERROR_PRIVILEGE_NOT_HELD = 1314U;
constexpr DWORD ERROR_COMMITMENT_LIMIT = 1455U;
constexpr DWORD ERROR_TIMEOUT = 1460U;
constexpr DWORD ERROR_CANT_RESOLVE_FILENAME = 1921U;
constexpr DWORD ERROR_NOT_SAME_OBJECT = 2102U;

struct FILETIME {
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
};

struct FILE_ID_128 {
  uint8_t Identifier[16];
};

struct FILE_ID_INFO {
  ULONGLONG VolumeSerialNumber;
  FILE_ID_128 FileId;
};

enum FILE_INFO_BY_HANDLE_CLASS {
  FileBasicInfo = 0,
  FileIdInfo = 18,
};

struct BY_HANDLE_FILE_INFORMATION {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime;
  FILETIME ftLastAccessTime;
  FILETIME ftLastWriteTime;
  DWORD dwVolumeSerialNumber;
  DWORD nFileSizeHigh;
  DWORD nFileSizeLow;
  DWORD nNumberOfLinks;
  DWORD nFileIndexHigh;
  DWORD nFileIndexLow;
};

DWORD GetLastError();
void SetLastError(DWORD error);
HANDLE GetCurrentProcess();
DWORD GetCurrentProcessId();
HANDLE GetCurrentThread();
DWORD GetCurrentThreadId();
BOOL GetProcessTimes(HANDLE process, FILETIME* created, FILETIME* exited, FILETIME* kernel, FILETIME* user);
BOOL GetThreadTimes(HANDLE thread, FILETIME* created, FILETIME* exited, FILETIME* kernel, FILETIME* user);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
DWORD GetProcessIdOfThread(HANDLE thread);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
BOOL CloseHandle(HANDLE handle);
DWORD GetFileType(HANDLE file);
BOOL GetFileInformationByHandleEx(HANDLE file, FILE_INFO_BY_HANDLE_CLASS cls, void* info, DWORD size);
BOOL GetFileInformationByHandle(HANDLE file, BY_HANDLE_FILE_INFORMATION* info);
HMODULE GetModuleHandleW(const wchar_t* name);
FARPROC GetProcAddress(HMODULE module, const char* symbol);
"""


def read_source(rel_path: str, ref: str | None) -> str:
    if ref:
        return subprocess.run(
            ["git", "show", f"{ref}:{rel_path}"],
            cwd=ROOT, check=True, capture_output=True, text=True,
        ).stdout
    return (ROOT / rel_path).read_text(encoding="utf-8")


def extract_method(text: str, signature: str) -> str:
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start:end]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="Enable ASan and UBSan")
    parser.add_argument("--source-ref", help="Git revision of record_lock_liveness.cc (negative control)")
    args = parser.parse_args()

    liveness_rel = "compat/posix/src/record_lock_liveness.cc"
    liveness_src = read_source(liveness_rel, args.source_ref)
    internal_src = (ROOT / "compat/posix/src/internal.cc").read_text(encoding="utf-8")
    errno_fn = extract_method(internal_src, "int ErrnoFromWin32(DWORD error)")

    print("record_lock_liveness.cc SHA256 " + hashlib.sha256(liveness_src.encode()).hexdigest(), flush=True)

    # Strip only the two top-level local includes already provided at the top of the host translation unit.
    stripped_lines = [
        line for line in liveness_src.splitlines()
        if line.strip() not in ('#include "record_lock_liveness.h"', '#include "internal.h"')
    ]
    template = (ROOT / "tests/native/record_lock_liveness_host_test.cc.in").read_text(encoding="utf-8")
    generated_src = (
        template
        .replace("@ERRNO_FROM_WIN32@", errno_fn)
        .replace("@RECORD_LOCK_LIVENESS_CC@", "\n".join(stripped_lines))
    )

    with tempfile.TemporaryDirectory(prefix="gesher-record-lock-liveness-") as tmp:
        work = Path(tmp)
        include_dir = work / "win32_host"
        include_dir.mkdir()
        (include_dir / "windows.h").write_text(WINDOWS_HOST_HEADER, encoding="utf-8")

        cpp_path = work / "record_lock_liveness_host_test.cc"
        exe_path = work / "record_lock_liveness_host_test"
        cpp_path.write_text(generated_src, encoding="utf-8")

        cmd = shlex.split(args.cxx) + [
            "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
            "-I", str(include_dir),
            "-I", str(ROOT / "compat/posix/src"),
            str(cpp_path), "-o", str(exe_path),
        ]
        if args.sanitize:
            cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]

        subprocess.run(cmd, check=True, timeout=90)
        env = os.environ.copy()
        if args.sanitize:
            env["UBSAN_OPTIONS"] = "halt_on_error=1:" + env.get("UBSAN_OPTIONS", "")
        subprocess.run([str(exe_path)], check=True, env=env, timeout=30)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
