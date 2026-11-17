#!/usr/bin/env python3
"""Run real pinned SQLite IO through the production guest-path boundary.

Windows production acceptance is sqlite_path_test.exe (CMake/CI). This portable
fixture compiles the exact patched winFullPathname callback and sqlite_path.cc,
plus the unchanged DiskPath/MapPath/DrivePath/Lengthen/virtual classifier. Its
UTF conversion, GetFullPathNameW, font lookup and procfs/device backends are
adapters; file IO, URI parsing, transactions, ATTACH, WAL and journals use the
pinned SQLite engine's real Unix VFS. It does not execute Win32 or ART.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PIN = "android-16.0.0_r4"
DIST = "dist/sqlite-autoconf-3500400"
HASHES = {
    "sqlite3.c": "ab8a927dfac4e8a673e141b93105daabd807409cfe164b8f669bd42b4a13d14d",
    "sqlite3.h": "abd1514e0351f79393d1be882830afdb40a8099e8257f311f0bfdf8486f11bea",
}


def function(source: str, signature: str) -> str:
    if source.count(signature) != 1:
        raise ValueError(f"production function boundary changed: {signature}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def namespace_fixture() -> str:
    source = (ROOT / "compat/posix/src/path.cc").read_text()
    internal = (ROOT / "compat/posix/src/internal.h").read_text()
    kinds = internal[internal.index("enum class PathKind {"):]
    kinds = kinds[:kinds.index("};") + 2]
    prologue = r'''
#pragma once
#include <algorithm>
#include <cerrno>
#include <codecvt>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <locale>
#include <string>
#include <string_view>
#include "reserved_names.h"
#include "unc_paths.h"
using DWORD = uint32_t;
constexpr unsigned MAX_PATH = 260;
// Windows lexical resolution only; no filesystem IO is simulated here.
static DWORD GetFullPathNameW(const wchar_t* input, DWORD size, wchar_t* out, wchar_t**) {
  std::wstring full(input);
  std::replace(full.begin(), full.end(), L'/', L'\\');
  if (full.rfind(L"\\\\", 0) != 0 && !(full.size() >= 3 && full[1] == L':' && full[2] == L'\\'))
    full = (full.starts_with(L'\\') ? L"D:" : L"D:\\work\\") + full;
  if (size <= full.size()) return static_cast<DWORD>(full.size() + 1);
  std::copy(full.begin(), full.end(), out);
  out[full.size()] = L'\0';
  return static_cast<DWORD>(full.size());
}
namespace gesher {
std::wstring Widen(const char* input) {
  if (!input) return {};
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(input);
}
std::string Narrow(const wchar_t* input) {
  if (!input) return {};
  return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(input);
}
static bool BinderDeviceExists() { return false; }
static int SystemFontPath(const char*, std::wstring*) { return 0; }
namespace procfs { static unsigned Type(const std::wstring&) { return 0; } }
'''
    signatures = ["bool IsDriveLetter(", "std::string_view NextPart(", "PathKind VirtualKind(",
                  "bool DrivePath(", "void Lengthen(", "PathKind MapPath(", "bool DiskPath("]
    return prologue + kinds + "\n" + "\n".join(function(source, name) for name in signatures) + "\n}\n"


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, timeout=180, **kwargs)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sqlite-source", type=Path, help="directory containing pristine pinned sqlite3.c and sqlite3.h")
    parser.add_argument("--fetch", action="store_true", help="download hash-verified sources from the pinned AOSP tag")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--negative-control", action="store_true", help="also require the original unpatched callback to fail guest open and ATTACH")
    args = parser.parse_args()
    if os.name == "nt":
        parser.error("use the production CMake sqlite_path_test.exe on Windows")
    if not args.sqlite_source and not args.fetch:
        parser.error("provide --sqlite-source or --fetch for the exact pinned SQLite engine")
    with tempfile.TemporaryDirectory(prefix="gesher-sqlite-contract-") as temporary:
        work = Path(temporary)
        dist = work / DIST
        dist.mkdir(parents=True)
        for name, expected in HASHES.items():
            if args.sqlite_source:
                data = (args.sqlite_source / name).read_bytes()
            else:
                url = f"https://android.googlesource.com/platform/external/sqlite/+/refs/tags/{PIN}/{DIST}/{name}?format=TEXT"
                with urllib.request.urlopen(url, timeout=60) as response:
                    data = base64.b64decode(response.read(), validate=True)
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f"pinned SQLite source hash mismatch: {name}")
            (dist / name).write_bytes(data)
        original = (dist / "sqlite3.c").read_text()
        patch = ROOT / "patches/external/sqlite/0001-win32-guest-paths.patch"
        run(["git", "apply", "--check", str(patch)], cwd=work)
        run(["git", "apply", str(patch)], cwd=work)
        patched = (dist / "sqlite3.c").read_text()
        (work / "sqlite_path_namespace.h").write_text(namespace_fixture())
        for name in ("reserved_names.h", "unc_paths.h"):
            shutil.copyfile(ROOT / "compat/posix/include/gesher" / name, work / name)
        bridge = (ROOT / "compat/posix/src/sqlite_path.cc").read_text()
        assert bridge.count('#include "internal.h"') == 1
        (work / "sqlite_path.cc").write_text(bridge.replace('#include "internal.h"', '#include "sqlite_path_namespace.h"'))
        flags = ["-O1", "-g", "-fno-omit-frame-pointer"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        # Match production's path-relevant options. ICU/collators and the ART
        # integration are exercised by the Windows runtime fixtures, not here.
        run(shlex.split(args.cc) + flags + ["-std=c17", "-DSQLITE_THREADSAFE=2", "-DSQLITE_TEMP_STORE=3",
            "-DSQLITE_DEFAULT_AUTOVACUUM=1", "-DSQLITE_OMIT_LOAD_EXTENSION", "-c", str(dist / "sqlite3.c"), "-o", str(work / "sqlite3.o")])
        host = (ROOT / "tests/native/sqlite_path_host.h.in").read_text()
        variants = [("patched", patched)] + ([("original-negative", original)] if args.negative_control else [])
        for label, source in variants:
            if label == "patched":
                copy = function(source, "static int gesherFullPathnameCopy(\n")
            else:
                # Exact stock Win32 behavior: snprintf truncates, but the
                # original callback nevertheless reports SQLITE_OK.
                copy = '''static int gesherFullPathnameCopy(sqlite3_vfs* vfs, int capacity, char* out, const char* name) {
  sqlite3_snprintf(capacity < vfs->mxPathname ? capacity : vfs->mxPathname, out, "%s", name);
  return SQLITE_OK;
}'''
            (work / "sqlite_path_host.h").write_text(host.replace("@WIN_FULL_COPY@", copy).replace("@WIN_FULL_PATHNAME@", function(source, "static int winFullPathname(\n")))
            executable = work / label
            run(shlex.split(args.cxx) + flags + ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-fno-exceptions",
                "-DGESHER", "-I", str(dist), "-I", str(work), str(ROOT / "tests/native/sqlite_path_test.cc"),
                str(work / "sqlite_path.cc"), str(work / "sqlite3.o"), "-pthread", "-ldl", "-o", str(executable)])
            if label == "patched":
                run([str(executable)])
            else:
                result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=60)
                required = ("FAIL: guest /mnt/<drive> opens existing directory", "FAIL: SQL ATTACH maps its guest filename",
                            "FAIL: SQL VACUUM INTO maps its guest filename", "FAIL: small VFS output rejects truncation and preserves canary")
                if result.returncode != 1 or not all(message in result.stderr for message in required):
                    raise RuntimeError(f"negative control did not reproduce expected production failures:\n{result.stdout}\n{result.stderr}")
                print(result.stdout.strip())
                print("Unpatched callback compiled and failed guest-open, ATTACH, VACUUM and bounded-output gates as expected.")


if __name__ == "__main__":
    main()
