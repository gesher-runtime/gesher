#!/usr/bin/env python3
"""Verify the writev source exception cannot admit unrelated supplier changes."""
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
print('vector source policy: positive source and four negative controls passed')
