#!/usr/bin/env python3
"""CI-only actual Linux fcntl oracle; no Gesher implementation is imported.

Workers are independent processes. JSON commands/results are the protocol a
future Windows native client must implement. This script is source-parsed
locally only: every syscall/fixture execution belongs in CI.
"""
import argparse
import ctypes
import errno
import json
import os
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


class Flock(ctypes.Structure):
    _fields_ = [("type", ctypes.c_short), ("whence", ctypes.c_short),
                ("start", ctypes.c_int64), ("length", ctypes.c_int64),
                ("pid", ctypes.c_int)]


def worker(inherited):
    import fcntl
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fcntl.restype = ctypes.c_int
    libc.fdopen.argtypes = [ctypes.c_int, ctypes.c_char_p]
    libc.fdopen.restype = ctypes.c_void_p
    libc.fclose.argtypes = [ctypes.c_void_p]
    libc.fclose.restype = ctypes.c_int
    libc.freopen.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
    libc.freopen.restype = ctypes.c_void_p
    libc.fileno.argtypes = [ctypes.c_void_p]
    libc.fileno.restype = ctypes.c_int
    slots = {index: fd for index, fd in enumerate(inherited)}
    streams = {}
    signal_count = 0

    def caught(_signum, _frame):
        nonlocal signal_count
        signal_count += 1

    print(json.dumps({"ready": True, "pid": os.getpid(),
                      "flock_size": ctypes.sizeof(Flock)}), flush=True)
    for line in sys.stdin:
        request = json.loads(line)
        op = request["op"]
        result = {"op": op, "result": 0, "errno": 0}
        try:
            slot = request.get("slot", 0)
            if op == "open":
                slots[slot] = os.open(request["path"], request.get("flags", os.O_RDWR))
            elif op == "close":
                os.close(slots.pop(slot))
            elif op == "dup":
                slots[request["target"]] = os.dup(slots[slot])
            elif op == "dup2":
                os.dup2(slots[slot], slots[request["target"]])
            elif op == "seek":
                result["offset"] = os.lseek(slots[slot], request["offset"], request.get("whence", 0))
            elif op == "io":
                os.pwrite(slots[slot], b"advisory", request["offset"])
                result["data"] = os.pread(slots[slot], 8, request["offset"]).decode("ascii")
            elif op == "flock":
                fcntl.flock(slots[slot], request["operation"])
            elif op == "signal":
                signal.signal(signal.SIGUSR1, caught)
                signal.siginterrupt(signal.SIGUSR1, not request["restart"])
            elif op == "stdio":
                ctypes.set_errno(0)
                stream = libc.fdopen(slots[slot], b"r+")
                if not stream:
                    raise OSError(ctypes.get_errno(), "fdopen")
                streams[slot] = stream
            elif op == "fclose":
                ctypes.set_errno(0)
                code = libc.fclose(streams.pop(slot))
                slots.pop(slot)
                result.update(result=code, errno=ctypes.get_errno() if code < 0 else 0)
            elif op == "freopen":
                ctypes.set_errno(0)
                stream = libc.freopen(os.fsencode(request["path"]), b"r+", streams[slot])
                if stream:
                    streams[slot] = stream
                    slots[slot] = libc.fileno(stream)
                else:
                    streams.pop(slot)
                    slots.pop(slot)
                    result.update(result=-1, errno=ctypes.get_errno())
            elif op == "lock":
                lock = Flock(request.get("type", 1), request.get("whence", 0),
                             request.get("start", 0), request.get("length", 0), request.get("pid", 0))
                ctypes.set_errno(0)
                code = libc.fcntl(ctypes.c_int(slots[slot]), ctypes.c_int(request.get("command", 6)),
                                  ctypes.byref(lock))
                result.update(result=code, errno=ctypes.get_errno() if code < 0 else 0,
                              command=request.get("command", 6),
                              type=lock.type, whence=lock.whence, start=lock.start,
                              length=lock.length, pid=lock.pid, signals=signal_count)
            elif op == "exit":
                print(json.dumps(result), flush=True)
                return
            else:
                raise ValueError("unknown operation")
        except OSError as error:
            result.update(result=-1, errno=error.errno)
        print(json.dumps(result), flush=True)


class Client:
    def __init__(self, label, inherited=()):
        self.label = label
        self.process = subprocess.Popen(
            [sys.executable, __file__, "--worker", *map(str, inherited)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1, pass_fds=inherited)
        self.rows = queue.Queue()
        self.cleaned = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        try:
            ready = self.take()
            if not ready.get("ready") or ready["flock_size"] != 32:
                raise AssertionError(f"Unexpected Linux LP64 worker ABI: {ready}")
            self.pid = ready["pid"]
        except BaseException:
            self.cleanup()
            raise

    def _read(self):
        try:
            for line in self.process.stdout:
                self.rows.put(json.loads(line))
        finally:
            self.rows.put({"eof": True})

    def send(self, **request):
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()

    def take(self, seconds=5):
        row = self.rows.get(timeout=seconds)
        if row.get("eof"):
            raise AssertionError(f"{self.label} exited before a result")
        return row

    def call(self, **request):
        self.send(**request)
        return self.take()

    def blocked(self):
        try:
            row = self.rows.get(timeout=0.25)
        except queue.Empty:
            if self.process.poll() is not None:
                raise AssertionError(f"{self.label} died while expected blocked")
            return
        raise AssertionError(f"{self.label} unexpectedly completed blocking request: {row}")

    def cleanup(self):
        if self.cleaned:
            return
        if self.process.poll() is None:
            try:
                self.process.kill()
            except ProcessLookupError:
                pass
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"{self.label} could not be terminated")
        self.reader.join(timeout=2)
        if self.reader.is_alive():
            # TextIO.close can itself block on the live reader's lock. Report
            # the unfinished pipe instead of hiding it in an unbounded close.
            raise AssertionError(f"{self.label} output pipe did not reach EOF after termination")
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.stderr.close()
        self.cleaned = True


def reference(output):
    clients = []
    rows = []
    names = {}

    def client(label, inherited=()):
        value = Client(label, inherited)
        clients.append(value)
        names[value.pid] = label
        return value

    def record(name, value, code=0, error=0, **fields):
        if value["result"] != code or value["errno"] != error:
            raise AssertionError(f"{name}: wrong return/errno {value}")
        for key, expected in fields.items():
            if value.get(key) != expected:
                raise AssertionError(f"{name}: expected {key}={expected}, actual {value}")
        value = dict(value)
        if (value.get("command") in (5, 36) and value["result"] == 0 and
                value.get("type") != 2 and value.get("pid", 0) > 0):
            value["pid"] = names.get(value["pid"], "unrecognized-live-pid")
        rows.append({"name": name, "actual": value})
        print(f"ACTUAL_LINUX_RECORD_LOCK {name} {json.dumps(value, sort_keys=True)}", flush=True)

    def lock(who, **options):
        value = who.call(op="lock", **options)
        # Linux GETLK modifies only l_type for a nonconflicting query.
        if options.get("command", 6) in (5, 36) and value["result"] == 0 and value["type"] == 2:
            for field in ("whence", "start", "length", "pid"):
                if value[field] != options.get(field, 0):
                    raise AssertionError(f"nonconflicting query changed {field}: {value}")
        return value

    try:
        with tempfile.TemporaryDirectory(prefix="gesher-record-lock-oracle-") as folder:
            first = os.path.join(folder, "first")
            second = os.path.join(folder, "second")
            alias = os.path.join(folder, "hardlink")
            for path in (first, second):
                with open(path, "wb") as stream:
                    stream.truncate(4096)
            os.link(first, alias)
            a, b = client("A"), client("B")
            for who in (a, b):
                record(f"{who.label}-open-first", who.call(op="open", path=first))
                record(f"{who.label}-open-second", who.call(op="open", slot=1, path=second))
            record("posix-write-range", lock(a, start=10, length=10))
            record("posix-query-own-excluded", lock(a, command=5, start=10, length=10), type=2)
            record("posix-query-other", lock(b, command=5, start=12, length=1),
                   type=1, whence=0, start=10, length=10, pid=a.pid)
            record("posix-nonblocking-conflict", lock(b, start=11, length=1), -1, errno.EAGAIN)
            record("record-is-advisory", b.call(op="io", offset=10), data="advisory")
            record("split-unlock-middle", lock(a, type=2, start=15, length=2))
            record("query-left-fragment", lock(b, command=5, start=12, length=1), start=10, length=5)
            record("query-right-fragment", lock(b, command=5, start=18, length=1), start=17, length=3)
            record("query-unlocked-hole", lock(b, command=5, start=15, length=2), type=2)
            record("clear-process-ranges", lock(a, type=2))
            record("read-range-A", lock(a, type=0, start=20, length=10))
            record("read-range-B-compatible", lock(b, type=0, start=20, length=10))
            record("write-conversion-conflicts", lock(a, start=20, length=10), -1, errno.EAGAIN)
            record("clear-B-read", lock(b, type=2))
            record("write-conversion-after-release", lock(a, start=20, length=10))
            record("hardlink-open", a.call(op="open", slot=2, path=alias))
            record("any-close-releases-posix", a.call(op="close", slot=2))
            record("hardlink-close-query", lock(b, command=5, start=20, length=10), type=2)
            record("relative-seek", a.call(op="seek", offset=50), offset=50)
            record("negative-length-relative", lock(a, whence=1, start=-10, length=-5))
            record("normalized-relative-query", lock(b, command=5, start=36, length=1),
                   type=1, whence=0, start=35, length=5, pid=a.pid)
            record("clear-relative", lock(a, type=2))
            record("zero-length-through-future-eof", lock(a, start=4096, length=0))
            record("query-beyond-current-eof", lock(b, command=5, start=1 << 40, length=1),
                   start=4096, length=0, pid=a.pid)
            record("clear-to-eof", lock(a, type=2))
            record("invalid-negative-start", lock(a, start=-1, length=1), -1, errno.EINVAL)
            record("invalid-whence", lock(a, whence=99), -1, errno.EINVAL)
            record("invalid-type", lock(a, type=99), -1, errno.EINVAL)
            record("invalid-range-overflow", lock(a, start=(1 << 63) - 2, length=4), -1, errno.EOVERFLOW)
            record("readonly-open", a.call(op="open", slot=2, path=first, flags=os.O_RDONLY))
            record("readonly-write-lock-rejected", lock(a, slot=2, length=1), -1, errno.EBADF)
            record("readonly-close", a.call(op="close", slot=2))
            record("writeonly-open", a.call(op="open", slot=2, path=first, flags=os.O_WRONLY))
            record("writeonly-read-lock-rejected", lock(a, slot=2, type=0, length=1), -1, errno.EBADF)
            record("writeonly-close", a.call(op="close", slot=2))

            # Distinct open descriptions conflict even inside one process.
            record("OFD-independent-open", a.call(op="open", slot=2, path=alias))
            record("OFD-write", lock(a, command=37, start=100, length=10))
            record("OFD-own-query-excluded", lock(a, command=36, start=101, length=1), type=2)
            record("OFD-other-description-same-process", lock(a, slot=2, command=36, start=101, length=1),
                   type=1, start=100, length=10, pid=-1)
            record("POSIX-conflicts-with-own-process-OFD", lock(a, start=101, length=1), -1, errno.EAGAIN)
            record("OFD-invalid-input-pid", lock(b, command=37, start=200, length=1, pid=1), -1, errno.EINVAL)
            record("dup-shares-description", a.call(op="dup", target=3))
            record("OFD-dup-query-excluded", lock(a, slot=3, command=36, start=101, length=1), type=2)
            record("OFD-close-original", a.call(op="close"))
            record("OFD-survives-duplicate", lock(b, command=5, start=101, length=1), pid=-1)
            record("OFD-close-unrelated", a.call(op="close", slot=2))
            record("OFD-survives-unrelated-close", lock(b, command=5, start=101, length=1), pid=-1)
            record("OFD-close-final", a.call(op="close", slot=3))
            record("OFD-final-close-releases", lock(b, command=5, start=101, length=1), type=2)
            record("A-reopen-first", a.call(op="open", path=first))
            record("flock-independent", a.call(op="flock", operation=2 | 4))
            record("record-independent-of-flock", lock(b, start=300, length=5))
            record("flock-release", a.call(op="flock", operation=8))
            record("clear-B-record", lock(b, type=2))

            # Actual blocking and signal behavior; no deadline is turned into PASS.
            record("blocking-owner", lock(a, start=400, length=5))
            b.send(op="lock", command=7, start=400, length=5)
            b.blocked()
            record("blocking-unlock-owner", lock(a, type=2))
            record("blocking-waiter-acquires", b.take())
            record("clear-blocking-waiter", lock(b, type=2))
            record("interrupt-handler", b.call(op="signal", restart=False))
            record("interrupt-owner", lock(a, start=500, length=5))
            b.send(op="lock", command=7, start=500, length=5)
            b.blocked()
            os.kill(b.pid, signal.SIGUSR1)
            record("blocking-EINTR", b.take(), -1, errno.EINTR, signals=1)
            record("restart-handler", b.call(op="signal", restart=True))
            b.send(op="lock", command=7, start=500, length=5)
            b.blocked()
            os.kill(b.pid, signal.SIGUSR1)
            b.blocked()
            record("restart-unlock-owner", lock(a, type=2))
            record("SA_RESTART-waiter-acquires", b.take(), signals=2)
            record("restart-clear", lock(b, type=2))

            # Global wait graph: two files, two real POSIX owners.
            record("deadlock-A-first", lock(a, length=1))
            record("deadlock-B-second", lock(b, slot=1, length=1))
            a.send(op="lock", slot=1, command=7, length=1)
            a.blocked()
            record("POSIX-cross-file-EDEADLK", lock(b, command=7, length=1), -1, errno.EDEADLK)
            record("deadlock-B-release", lock(b, slot=1, type=2))
            record("deadlock-A-completes", a.take())
            record("deadlock-clear-A-first", lock(a, type=2))
            record("deadlock-clear-A-second", lock(a, slot=1, type=2))

            # Linux intentionally does not detect OFD-owner deadlock cycles.
            record("OFD-cycle-signal-handler", b.call(op="signal", restart=False))
            record("OFD-cycle-A-first", lock(a, command=37, length=1))
            record("OFD-cycle-B-second", lock(b, slot=1, command=37, length=1))
            a.send(op="lock", slot=1, command=38, length=1)
            a.blocked()
            b.send(op="lock", command=38, length=1)
            b.blocked()
            os.kill(b.pid, signal.SIGUSR1)
            record("OFD-cycle-no-EDEADLK-interrupt", b.take(), -1, errno.EINTR)
            record("OFD-cycle-B-release", lock(b, slot=1, command=37, type=2))
            record("OFD-cycle-A-completes", a.take())
            record("OFD-cycle-clear-A-first", lock(a, command=37, type=2))
            record("OFD-cycle-clear-A-second", lock(a, slot=1, command=37, type=2))

            record("dup2-target-posix-lock", lock(a, start=900, length=5))
            record("dup2-replaces-target", a.call(op="dup2", slot=1, target=0))
            record("dup2-target-close-releases", lock(b, command=5, start=900, length=1), type=2)
            record("close-dup2-new-target", a.call(op="close"))
            record("reopen-for-stdio", a.call(op="open", path=first))
            record("stdio-posix-lock", lock(a, start=900, length=5))
            record("stdio-fdopen", a.call(op="stdio"))
            record("stdio-fclose", a.call(op="fclose"))
            record("stdio-close-releases-posix", lock(b, command=5, start=900, length=1), type=2)
            record("reopen-for-freopen", a.call(op="open", path=first))
            record("freopen-fdopen", a.call(op="stdio"))
            record("freopen-posix-lock", lock(a, start=900, length=5))
            record("freopen-replaces-file", a.call(op="freopen", path=second))
            record("freopen-closes-old-description", lock(b, command=5, start=900, length=1), type=2)
            record("freopen-close-new-description", a.call(op="fclose"))
            record("reopen-for-failed-freopen", a.call(op="open", path=first))
            record("failed-freopen-fdopen", a.call(op="stdio"))
            record("failed-freopen-posix-lock", lock(a, start=900, length=5))
            record("failed-freopen-still-closes-old", a.call(op="freopen", path=first + "/missing"),
                   -1, errno.ENOTDIR)
            record("failed-freopen-released-posix", lock(b, command=5, start=900, length=1), type=2)

            # Actual crash cleanup must not depend on a graceful unlock.
            c = client("crashed-owner")
            record("crash-open", c.call(op="open", path=first))
            record("crash-lock", lock(c, start=700, length=5))
            record("crash-lock-visible", lock(b, command=5, start=700, length=1), pid=c.pid)
            c.cleanup()
            record("crash-removes-lock", lock(b, command=5, start=700, length=1), type=2)

            # OFD inherited through supported exec descriptor transfer.
            shared = os.open(first, os.O_RDWR)
            d, e = client("inherited-D", (shared,)), client("inherited-E", (shared,))
            os.close(shared)
            record("inherited-OFD-lock", lock(d, command=37, start=800, length=5))
            record("inherited-OFD-same-description", lock(e, command=36, start=800, length=1), type=2)
            record("inherited-OFD-close-D", d.call(op="close"))
            record("inherited-OFD-remains", lock(b, command=5, start=800, length=1), pid=-1)
            record("inherited-OFD-close-E", e.call(op="close"))
            record("inherited-OFD-final-release", lock(b, command=5, start=800, length=1), type=2)
    finally:
        cleanup_errors = []
        for value in clients:
            try:
                value.cleanup()
            except Exception as error:
                cleanup_errors.append(error)
        if cleanup_errors:
            raise ExceptionGroup("record-lock worker cleanup failures", cleanup_errors)
    manifest = {"schema": 1, "source": os.environ["GITHUB_SHA"], "producer": "actual-linux-fcntl",
                "kernel": os.uname().release, "flock_size": ctypes.sizeof(Flock), "rows": rows,
                "remaining_reference_scope": ["close-EINTR deterministic injection", "simultaneous mutation contention",
                                              "PID reuse and denied owner query"]}
    with open(output, "w", encoding="utf-8") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f"ACTUAL_LINUX_RECORD_LOCK_SUMMARY rows={len(rows)} failures=0; scope=manifest only", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", nargs="*", type=int)
    parser.add_argument("--output", default="record-lock-reference.json")
    args = parser.parse_args()
    if os.environ.get("GITHUB_ACTIONS") != "true" or not sys.platform.startswith("linux"):
        raise SystemExit("The native syscall oracle executes only on Linux CI")
    if args.worker is not None:
        worker(args.worker)
    else:
        reference(args.output)


if __name__ == "__main__":
    main()
