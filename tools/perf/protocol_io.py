"""Concurrent single-writer JSON mailboxes for the owned workload handshake.

These are live protocol reads, not the separate post-failure diagnostic reader.
Windows readers permit replacement, but not in-place writes. A reader retains
one complete old or new file object while its peer publishes the next message.
Only native Windows access/sharing errors receive bounded transport retries;
all parsing and invocation/phase/ownership checks remain outside that recovery.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import time
from functools import cache

from contract import parse_json

WINDOWS = os.name == "nt"
MAX_BYTES = 4 * 1024 * 1024
MAX_ATTEMPTS = 5
RETRY_SECONDS = .01


def _deadline(deadline: float) -> None:
    if time.monotonic() >= deadline:
        raise TimeoutError("Workload protocol exceeded its existing deadline")


def _retry(operation, deadline):
    for attempt in range(MAX_ATTEMPTS):
        _deadline(deadline)
        try:
            value = operation()
        except OSError as error:
            # ERROR_ACCESS_DENIED can also mean a genuine denial. It is never
            # accepted as success: persistent denial preserves the final error.
            # Do not classify generic errno.EACCES or other platform errors.
            if not WINDOWS or getattr(error, "winerror", None) not in (5, 32) or attempt == MAX_ATTEMPTS - 1:
                raise
            _deadline(deadline)
            backoff = RETRY_SECONDS * (2 ** attempt)
            time.sleep(min(backoff, max(0, deadline - time.monotonic())))
        else:
            _deadline(deadline)
            return value


@cache
def _kernel():
    import ctypes as c
    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [c.c_wchar_p, c.c_uint32, c.c_uint32, c.c_void_p,
                                  c.c_uint32, c.c_uint32, c.c_void_p]
    kernel.CreateFileW.restype = c.c_void_p
    kernel.CloseHandle.argtypes = [c.c_void_p]
    kernel.CloseHandle.restype = c.c_int
    kernel.ReplaceFileW.argtypes = [c.c_wchar_p, c.c_wchar_p, c.c_wchar_p,
                                   c.c_uint32, c.c_void_p, c.c_void_p]
    kernel.ReplaceFileW.restype = c.c_int
    return c, kernel


def _open_snapshot(path):
    if not WINDOWS:
        return path.open("rb")
    import msvcrt
    c, kernel = _kernel()
    # FILE_SHARE_READ | FILE_SHARE_DELETE. Normal Python opens omit DELETE;
    # that both blocks replacement and conflicts with a rename handle in flight.
    handle = kernel.CreateFileW(str(path), 0x80000000, 0x5, None, 3, 0x80, None)
    if handle == c.c_void_p(-1).value:
        error = c.WinError(c.get_last_error())
        error.filename = str(path)
        raise error
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
    except BaseException:
        kernel.CloseHandle(handle)
        raise
    try:
        return os.fdopen(descriptor, "rb")  # Owns the same handle, no path reopen.
    except BaseException:
        os.close(descriptor)
        raise


def _read_snapshot(path):
    with _open_snapshot(path) as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("Protocol input must be a regular file")
        value = stream.read(MAX_BYTES + 1)
    if len(value) > MAX_BYTES:
        raise ValueError("JSON input exceeds the 4 MiB evidence budget")
    return value


def read_json(path: Path, *, deadline: float):
    data = _retry(lambda: _read_snapshot(path), deadline)
    # Invalid JSON, duplicate keys, nonfinite values and invalid UTF-8 are not
    # transport errors and cannot be skipped in favor of a later message.
    value = parse_json(data.decode("utf-8-sig"))
    _deadline(deadline)
    return value


def _publish(temporary, path):
    if WINDOWS and path.exists():
        c, kernel = _kernel()
        # ReplaceFile preserves an open old snapshot when readers share DELETE.
        # Flags=0 retains ACL/attribute errors; there is no delete-then-move gap.
        if not kernel.ReplaceFileW(str(path), str(temporary), None, 0, None, None):
            error = c.WinError(c.get_last_error())
            error.filename, error.filename2 = str(temporary), str(path)
            raise error
    else:
        temporary.replace(path)


def write_json(path: Path, value, *, deadline: float):
    _deadline(deadline)
    text = json.dumps(value, indent=2, allow_nan=False) + "\n"
    if len(text.encode("utf-8")) > MAX_BYTES:
        raise ValueError("JSON input exceeds the 4 MiB evidence budget")
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    _retry(lambda: _publish(temporary, path), deadline)
