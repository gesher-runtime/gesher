#!/usr/bin/env python3
"""Reject repinned changes outside the exact Scandir -> Memrchr -> OFD chain."""
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
spec = importlib.util.spec_from_file_location('scandir_selected_policy',
    Path(__file__).with_name('test-selected-policy.py'))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
FOLDER = 'tools/net-tcp-priority/qualified/'
MANIFEST = FOLDER + 'scandir-source.json'
PATCH = FOLDER + 'scandir-source.patch'
read_bytes = Path.read_bytes
manifest = json.loads((ROOT / MANIFEST).read_bytes())
transition_patch = (ROOT / PATCH).read_bytes()
checks = 0


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rejected(label, changes, expected, repinned=None):
    global checks
    overrides = {ROOT / name: data for name, data in changes.items()}
    assert all(data != read_bytes(path) for path, data in overrides.items()), label
    pin = policy.SCANDIR_TRANSITION_SHA256
    if repinned is not None:
        raw = (json.dumps(repinned, indent=2, sort_keys=True) + '\n').encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, 'read_bytes', mutated), \
            patch.object(policy, 'SCANDIR_TRANSITION_SHA256', pin), \
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
    print('PASS scandir source policy rejects ' + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

name = policy.SCANDIR_SOURCE
original = (ROOT / name).read_bytes()
previous = policy.preceding_scandir_source(original)
assert sha(previous) == manifest['before_sha256'][name]
assert previous == subprocess.check_output(['git', '-C', str(ROOT), 'show', policy.SCANDIR_BASE + ':' + name])
parent_manifest = json.loads((ROOT / (FOLDER + 'memrchr-source.json')).read_bytes())
assert sha(previous) == parent_manifest['after_sha256'][name]

changed = original + b'\n// undeclared source bytes\n'
rejected('unqualified source bytes', {name: changed}, name)
repinned = copy.deepcopy(manifest)
repinned['after_sha256'][name] = sha(changed)
rejected('a repinned undeclared endpoint', {name: changed}, 'scandir reverse endpoint', repinned)
for label, before, after, expected in [
    ('stable tie order', b'to[out] = compare(&x, &y) <= 0',
     b'to[out] = compare(&x, &y) < 0', 'exact patch application'),
    ('optional allocation result', b'if (scratch == nullptr) return false;',
     b'if (scratch == nullptr) return true;', 'exact patch application'),
    ('adaptive work limit', b'size_t remaining_swaps = count * 8;',
     b'size_t remaining_swaps = count * 9;', 'exact patch application'),
    ('scratch ownership', b'std::free(scratch);', b'(void)scratch;', 'exact patch application'),
    ('unrelated FIFO operation', b'int BIONIC_SYSV Mkfifo(const char*, uint32_t) { return errno = kEPERM, -1; }',
     b'int BIONIC_SYSV Mkfifo(const char*, uint32_t) { return 0; }', 'scandir reverse endpoint'),
    ('previous Memrchr word bound', b'while (n >= sizeof(uint64_t)) {',
     b'while (n > 0) {', 'scandir reverse endpoint'),
    ('public export', b'E("scandir", Scandir);', b'E("scandir", ScandirWrong);', 'scandir reverse endpoint'),
]:
    assert original.count(before) == 1, label
    changed = original.replace(before, after, 1)
    repinned = copy.deepcopy(manifest)
    repinned['after_sha256'][name] = sha(changed)
    rejected('a repinned ' + label, {name: changed}, expected, repinned)

for field, expected in [('baseline', 'scandir baseline changed'),
                        ('baseline_policy_sha256', 'scandir baseline policy changed')]:
    repinned = copy.deepcopy(manifest)
    repinned[field] = '0' * len(repinned[field])
    rejected('a repinned ' + field, {}, expected, repinned)
repinned = copy.deepcopy(manifest)
repinned['before_sha256'][name] = '0' * 64
rejected('a repinned predecessor digest', {}, name, repinned)
repinned = copy.deepcopy(manifest)
repinned['paths'].append('compat/posix/src/ofd.cc')
rejected('expanded transition paths', {}, 'scandir transition paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['after_sha256']['compat/posix/src/ofd.cc'] = '0' * 64
rejected('expanded endpoint paths', {}, 'scandir endpoint paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['native_execution_approved'] = True
rejected('invented runtime approval', {}, 'scandir transition schema changed', repinned)

changed = transition_patch + b'diff --git a/unqualified.cc b/unqualified.cc\n'
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed)
rejected('a jointly repinned undeclared patch path', {PATCH: changed}, 'scandir patch paths changed', repinned)
context = b'\n int ScanDirectory(ShimDir* d, BionicDirent*** list, SysvFilter filter,\n'
assert transition_patch.count(context) == 1
wrong_context = context.replace(b'*** list', b'*** output')
changed_patch = transition_patch.replace(context, wrong_context, 1)
changed_source = original.replace(context[2:], wrong_context[2:], 1)
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed_patch)
repinned['after_sha256'][name] = sha(changed_source)
rejected('jointly repinned predecessor context', {PATCH: changed_patch, name: changed_source},
         'scandir reverse endpoint', repinned)
rejected('changed manifest without independent pin', {MANIFEST: read_bytes(ROOT / MANIFEST) + b'\n'},
         'scandir source transition changed')
rejected('changed patch without independent pin', {PATCH: transition_patch + b'\n'},
         'scandir transition patch changed')
for path, expected in [
    (FOLDER + 'memrchr-source.json', 'memrchr source transition changed'),
    (FOLDER + 'memrchr-source.patch', 'memrchr transition patch changed'),
    (FOLDER + 'ofd-stream-source.json', 'OFD source qualification changed'),
    (FOLDER + 'ofd-stream-source.patch', 'OFD transition patch changed'),
    (FOLDER + 'shared-stdio-source.json', 'shared-stdio source transition changed'),
    ('compat/posix/src/ofd.cc', 'compat/posix/src/ofd.cc'),
    ('compat/bionic/stdio_windows_lock.h', 'compat/bionic/stdio_windows_lock.h'),
]:
    rejected('changed prior protected bytes in ' + path, {path: read_bytes(ROOT / path) + b'\n'}, expected)
print('SCANDIR_SOURCE_POLICY_PASS exact_parent_round_trip=1 original_protected_digests=71 negative_controls=' + str(checks))
