#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Execute actual epoll/timerfd and inotify publication against a Linux oracle.

The existing complete-epoll host adapter supplies Win32 events, PollFds and CRT
descriptors. This fixture controls the timer clock/native callback transport and
inotify raw-record boundary only. It compiles unchanged complete timerfd.cc and
unchanged Inotify::Emit/ReadInotify bodies. The shared public corpus also runs on
real Linux and is built into the real Windows epoll_probe; no host run is Windows
runtime evidence. --source-ref compiles the same tests against old production.
"""
from __future__ import annotations
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def section(source: str, first: str, following: str) -> str:
    if source.count(first) != 1 or source.count(following) != 1:
        raise ValueError("ambiguous production boundary: " + first)
    start = source.index(first)
    return source[start:source.index(following, start)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--keep-dir", type=Path)
    args = parser.parse_args()

    def read(path: str) -> str:
        if args.source_ref:
            return subprocess.check_output(["git", "show", args.source_ref + ":" + path], cwd=ROOT, text=True)
        return (ROOT / path).read_text()

    sources = {name: read("compat/posix/src/" + name + ".cc") for name in ("epoll", "timerfd", "inotify")}
    for name, source in sources.items():
        print(name + ".cc SHA256 " + hashlib.sha256(source.encode()).hexdigest(), flush=True)
    epoll = re.sub(r'^#include "(?:event_internal|internal|socket_internal|record_lock_descriptors|native_fence_internal)\.h"\n', "", sources["epoll"], flags=re.M)
    epoll = epoll.replace("#include <windows.h>\n", "")
    host = (ROOT / "tests/native/epoll_eventfd_edges_host.cc.in").read_text()
    if host.count("@EPOLL_SOURCE@") != 1:
        raise ValueError("ambiguous complete epoll source marker")
    host = "#define GESHER_READONLY_EDGES 1\n" + host.replace("@EPOLL_SOURCE@", epoll)
    extension = (ROOT / "tests/native/epoll_readonly_edges_host.cc.in").read_text()
    pieces = {
        "TIMER_SOURCE": re.sub(r"^#include[^\n]*\n", "", sources["timerfd"], flags=re.M),
        "INOTIFY_EMIT": section(sources["inotify"], "  void Emit(int wd,", "  // A watch's event"),
        "INOTIFY_READ": section(sources["inotify"], "int ReadInotify(", "\n}  // namespace gesher"),
    }
    for key, value in pieces.items():
        marker = "@" + key + "@"
        if extension.count(marker) != 1:
            raise ValueError("ambiguous production marker: " + marker)
        extension = extension.replace(marker, value)
    reference = '''#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>
static int checked, failed;
static void check(const char* label, int ok) {
  ++checked; if (!ok) { ++failed; std::printf("FAIL %s\\n", label); }
}
#include "epoll_readonly_edge_test.inc"
int main() {
  test_epoll_readonly_edges();
  std::printf("readonly edge Linux reference: %d checks, %d failures\\n", checked, failed);
  return failed != 0;
}
'''
    failed = False
    with tempfile.TemporaryDirectory(prefix="gesher-readonly-edge-") as temporary:
        directory = args.keep_dir or Path(temporary)
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "gesher").mkdir(exist_ok=True)
        for header in ("timerfd.h", "resume.h"):
            (directory / "gesher" / header).write_text(read("compat/posix/include/gesher/" + header))
        (directory / "poll_scratch.h").write_text(read("compat/posix/src/poll_scratch.h"))
        (directory / "epoll_readonly_edges_host.cc").write_text(extension)
        for role, content in (("reference", reference), ("production-host", host)):
            path, binary = directory / (role + ".cc"), directory / role
            path.write_text(content)
            command = shlex.split(args.cxx) + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-pthread", "-I", str(directory), "-I", str(ROOT / "tests/native"), str(path), "-o", str(binary)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
            subprocess.run(command, check=True, timeout=120)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=45)
            (directory / (role + ".txt")).write_text(result.stdout + result.stderr)
            print(result.stdout, end="", flush=True)
            if result.returncode:
                failed = True
                print(f"FAIL {role}: exit={result.returncode}\n{result.stderr[:8000]}", flush=True)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
