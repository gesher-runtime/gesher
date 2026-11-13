#!/usr/bin/env python3
"""Verify reviewed writev/fcntl repairs cannot admit unrelated supplier changes."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('selected_policy', Path(__file__).with_name('test-selected-policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
source_path = ROOT / 'compat/posix/src/system.cc'
original = source_path.read_bytes()
read_bytes = Path.read_bytes


def rejected(label, replacement, repin=False):
    def mutated(path):
        return replacement if path == source_path else read_bytes(path)
    with patch.object(Path, 'read_bytes', mutated), contextlib.redirect_stdout(io.StringIO()):
        saved = policy.SYSTEM_SHA256
        try:
            if repin:
                policy.SYSTEM_SHA256 = hashlib.sha256(replacement).hexdigest()
            try:
                policy.check()
            except AssertionError:
                print_result = label
            else:
                raise RuntimeError('source policy accepted ' + label)
        finally:
            policy.SYSTEM_SHA256 = saved
    print('PASS source policy rejects ' + print_result)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()
rejected('unrelated readv edit even with a new full-file hash',
         original.replace(b'ssize_t readv(', b'// unrelated readv change\nssize_t readv(', 1), True)
rejected('socket gather edit even with a new full-file hash',
         original.replace(b'bool SocketIo(', b'// unrelated socket change\nbool SocketIo(', 1), True)
rejected('ambiguous writev boundary', original + b'\n// ssize_t writev(\n', True)
rejected('unreviewed bytes inside writev',
         original.replace(b'  if (int error = gesher::EventObjectWriteError',
                          b'  // unreviewed change\n  if (int error = gesher::EventObjectWriteError', 1))
rejected('fcntl getter operand consumption even with a new full-file hash',
         original.replace(b'  if (cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||\n',
                          b'  if (cmd == F_GETFD || cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||\n', 1), True)
rejected('fcntl integer operand ABI change even with a new full-file hash',
         original.replace(b'    arg = va_arg(args, int);\n',
                          b'    arg = va_arg(args, long);\n', 1), True)
rejected('fcntl lock-pointer ABI change even with a new full-file hash',
         original.replace(b'    struct flock* argument = va_arg(pointers, struct flock*);\n',
                          b'    struct flock* argument = reinterpret_cast<struct flock*>(va_arg(pointers, long));\n', 1), True)
rejected('fcntl socket descriptor flags edit even with a new full-file hash',
         original.replace(b'        return FD_CLOEXEC;  // sockets are never inherited (winsock.cc)\n',
                          b'        return 0;  // sockets are never inherited (winsock.cc)\n', 1), True)
rejected('duplicate reviewed fcntl preamble even with a new full-file hash',
         original + b'\n' + policy.FCNTL_REPLACEMENTS[1][1], True)
print('vector/fcntl source policy: positive source and nine negative controls passed')
