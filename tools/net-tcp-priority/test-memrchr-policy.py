#!/usr/bin/env python3
"""Exercise the exact memrchr transition without modifying any working files.

Repinned inputs must still restore the complete public main source and every
original qualification check. No native execution or approval is inferred.
"""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    'selected_policy', Path(__file__).with_name('test-selected-policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
FOLDER = 'tools/net-tcp-priority/qualified/'
MANIFEST = FOLDER + 'memrchr-source.json'
PATCH = FOLDER + 'memrchr-source.patch'
raw_read_bytes = Path.read_bytes
# The outer transition is checked against the real current file first. Keep
# every existing Memrchr negative control on that exact reconstructed input;
# the separate scandir suite mutates the complete composed chain itself.
preceding = policy.preceding_scandir_source(raw_read_bytes(ROOT / policy.SCANDIR_SOURCE))
def read_bytes(path):
    return preceding if path == ROOT / policy.SCANDIR_SOURCE else raw_read_bytes(path)
manifest = json.loads((ROOT / MANIFEST).read_bytes())
transition_patch = (ROOT / PATCH).read_bytes()
checks = 0


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rejected(label, changes, expected, repinned=None):
    global checks
    overrides = {ROOT / name: data for name, data in changes.items()}
    for path, data in overrides.items():
        assert data != read_bytes(path), 'negative control made no change: ' + str(path)
    pin = policy.MEMRCHR_TRANSITION_SHA256
    if repinned is not None:
        raw = (json.dumps(repinned, indent=2, sort_keys=True) + '\n').encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, 'read_bytes', mutated), \
            patch.object(policy, 'preceding_scandir_source', lambda current: current), \
            patch.object(policy, 'MEMRCHR_TRANSITION_SHA256', pin), \
            contextlib.redirect_stdout(io.StringIO()):
        try:
            policy.check()
        except AssertionError as error:
            assert expected in str(error), (label, expected, error)
        except subprocess.CalledProcessError as error:
            assert expected == 'exact patch application', (label, error)
            assert error.cmd == ['git', 'apply', '--reverse', '--check', '-'] and error.returncode == 1
        else:
            raise AssertionError('source policy accepted ' + label)
    checks += 1
    print('PASS memrchr source policy rejects ' + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

name = policy.MEMRCHR_SOURCE
original = read_bytes(ROOT / name)
changed = original + b'\n// undeclared source bytes\n'
rejected('unqualified source bytes', {name: changed}, name)
repinned = copy.deepcopy(manifest)
repinned['after_sha256'][name] = sha(changed)
rejected('a repinned undeclared endpoint', {name: changed}, 'memrchr reverse endpoint', repinned)

# Updating a whole-file digest cannot authorize a fortify, unrelated POSIX or
# optimized search change outside the independently reviewed exact transition.
for label, before, after, expected in [
    ('fortify bound', b'if (n > buf_size) FortifyFailure("memrchr");',
     b'if (n > buf_size + 1) FortifyFailure("memrchr");', 'memrchr reverse endpoint'),
    ('unrelated FIFO operation', b'int BIONIC_SYSV Mkfifo(const char*, uint32_t) { return errno = kEPERM, -1; }',
     b'int BIONIC_SYSV Mkfifo(const char*, uint32_t) { return 0; }', 'memrchr reverse endpoint'),
    ('word read range', b'while (n >= sizeof(uint64_t)) {',
     b'while (n > 0) {', 'exact patch application'),
]:
    assert original.count(before) == 1, label
    changed = original.replace(before, after, 1)
    repinned = copy.deepcopy(manifest)
    repinned['after_sha256'][name] = sha(changed)
    rejected('a repinned ' + label, {name: changed}, expected, repinned)

for field, expected in [('baseline', 'memrchr baseline changed'),
                        ('baseline_policy_sha256', 'memrchr baseline policy changed')]:
    repinned = copy.deepcopy(manifest)
    repinned[field] = '0' * len(repinned[field])
    rejected('a repinned ' + field, {}, expected, repinned)
repinned = copy.deepcopy(manifest)
repinned['before_sha256'][name] = '0' * 64
rejected('a repinned baseline digest', {}, name, repinned)
repinned = copy.deepcopy(manifest)
repinned['paths'].append('compat/posix/src/ofd.cc')
rejected('an expanded transition path', {}, 'memrchr transition paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['after_sha256']['compat/posix/src/ofd.cc'] = '0' * 64
rejected('an expanded endpoint map', {}, 'memrchr endpoint paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['native_execution_approved'] = True
rejected('an invented runtime approval', {}, 'memrchr transition schema changed', repinned)

changed = transition_patch + b'diff --git a/unqualified.cc b/unqualified.cc\n'
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed)
rejected('a repinned undeclared patch path', {PATCH: changed}, 'memrchr patch paths changed', repinned)
# This transition is insertion-only. A changed context byte applies in reverse
# only if both source and patch are repinned, then the exact baseline rejects it.
context = b' void* BIONIC_SYSV Memrchr(const void* s, int c, size_t n) {\n'
assert transition_patch.count(context) == 1
wrong_context = context.replace(b'const void* s', b'const void* source')
changed_patch = transition_patch.replace(context, wrong_context, 1)
changed_source = original.replace(context[1:], wrong_context[1:], 1)
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed_patch)
repinned['after_sha256'][name] = sha(changed_source)
rejected('a jointly repinned baseline context', {PATCH: changed_patch, name: changed_source},
         'memrchr reverse endpoint', repinned)

rejected('changed manifest without its independent pin', {MANIFEST: (ROOT / MANIFEST).read_bytes() + b'\n'},
         'memrchr source transition changed')
rejected('changed patch without its independent pin', {PATCH: transition_patch + b'\n'},
         'memrchr transition patch changed')
for name, expected in [
    (FOLDER + 'ofd-stream-source.json', 'OFD source qualification changed'),
    (FOLDER + 'ofd-stream-source.patch', 'OFD transition patch changed'),
    (FOLDER + 'shared-stdio-source.json', 'shared-stdio source transition changed'),
    ('compat/posix/src/ofd.cc', 'compat/posix/src/ofd.cc'),
    ('compat/bionic/stdio_windows_lock.h', 'compat/bionic/stdio_windows_lock.h'),
]:
    rejected('unrelated protected bytes in ' + name,
             {name: (ROOT / name).read_bytes() + b'\n'}, expected)
print('MEMRCHR_SOURCE_POLICY_PASS exact_main_round_trip=1 original_protected_digests=71 negative_controls=' + str(checks))
