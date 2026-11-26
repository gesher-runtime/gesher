#!/usr/bin/env python3
"""Reject unreviewed TSF bytes, repinned endpoints and expanded source scope.

These controls call the actual selected-source policy with in-memory inputs;
the working tree, original qualified controller and all receipts stay intact.
They grant no native execution or supplier/runtime approval.
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
MANIFEST = FOLDER + 'tsf-output-metadata-source.json'
PATCH = FOLDER + 'tsf-output-metadata-source.patch'
read_bytes = Path.read_bytes
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
    pin = policy.TSF_METADATA_QUALIFICATION_SHA256
    if repinned is not None:
        raw = (json.dumps(repinned, indent=2, sort_keys=True) + '\n').encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, 'read_bytes', mutated), \
            patch.object(policy, 'TSF_METADATA_QUALIFICATION_SHA256', pin), \
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
    print('PASS TSF metadata source policy rejects ' + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

for name in [*manifest['paths'], policy.TSF_METADATA_ADDITIONAL_SOURCE]:
    changed = (ROOT / name).read_bytes() + b'\n// undeclared source bytes\n'
    rejected('unqualified bytes in ' + name, {name: changed}, name)

for name in manifest['paths']:
    changed = (ROOT / name).read_bytes() + b'\n// undeclared endpoint even with a new digest\n'
    repinned = copy.deepcopy(manifest)
    repinned['after_sha256'][name] = sha(changed)
    rejected('repinned undeclared endpoint in ' + name, {name: changed},
             'TSF metadata reverse endpoint', repinned)

name = policy.TSF_METADATA_ADDITIONAL_SOURCE
changed = (ROOT / name).read_bytes() + b'\n// unreviewed control source\n'
repinned = copy.deepcopy(manifest)
repinned['additional_sources'][name] = sha(changed)
rejected('repinned new control source', {name: changed}, 'TSF metadata additional source endpoint', repinned)

# An unrelated deadline or inherited-writer change cannot be admitted merely by
# updating the endpoint digest. The original controller and entry remain exact.
for name, before, after in [
    ('tools/windows4/test-tsf-job-supervisor.ps1', b'$r.MainMilliseconds -gt 5000', b'$r.MainMilliseconds -gt 5001'),
    ('tools/windows4/TsfJobSupervisor.cs', b'const int capacity=64;', b'const int capacity=65;'),
]:
    original = (ROOT / name).read_bytes()
    assert original.count(before) == 1
    changed = original.replace(before, after, 1)
    repinned = copy.deepcopy(manifest)
    repinned['after_sha256'][name] = sha(changed)
    rejected('repinned unrelated controller/entry change: ' + name, {name: changed},
             'TSF metadata reverse endpoint', repinned)

name = 'tools/windows4/test-tsf-job-supervisor.ps1'
original = (ROOT / name).read_bytes()
changed = original.replace(b'  [TsfOutputMetadataChecks]::Run($directory)\n', b'', 1)
repinned = copy.deepcopy(manifest)
repinned['after_sha256'][name] = sha(changed)
rejected('repinned omission of required real metadata controls', {name: changed},
         'exact patch application', repinned)

for field, expected in [('baseline', 'TSF metadata baseline changed'),
                        ('feature_head', 'TSF metadata feature checkpoint changed'),
                        ('baseline_policy_sha256', 'TSF metadata baseline policy changed')]:
    repinned = copy.deepcopy(manifest)
    repinned[field] = '0' * len(repinned[field])
    rejected('repinned ' + field, {}, expected, repinned)

repinned = copy.deepcopy(manifest)
repinned['before_sha256'][manifest['paths'][0]] = '0' * 64
rejected('repinned baseline digest', {}, manifest['paths'][0], repinned)
repinned = copy.deepcopy(manifest)
repinned['paths'].append('tools/windows4/TsfJobWorker.cs')
rejected('an expanded transition path', {}, 'TSF metadata paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['after_sha256']['tools/windows4/TsfJobWorker.cs'] = '0' * 64
rejected('an expanded endpoint map', {}, 'TSF metadata endpoint paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['additional_sources']['tools/windows4/TsfJobWorker.cs'] = '0' * 64
rejected('an expanded new-file scope', {}, 'TSF metadata additional paths changed', repinned)
repinned = copy.deepcopy(manifest)
repinned['native_execution_approved'] = True
rejected('an invented runtime approval', {}, 'TSF metadata schema changed', repinned)

changed = transition_patch + b'diff --git a/unqualified.cs b/unqualified.cs\n'
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed)
rejected('a repinned undeclared patch path', {PATCH: changed}, 'TSF metadata patch paths changed', repinned)
line = next(line for line in transition_patch.splitlines(keepends=True)
            if line.startswith(b'-') and not line.startswith(b'---') and line.strip() != b'-')
changed = transition_patch.replace(line, b'-/* unreviewed baseline */ ' + line[1:], 1)
repinned = copy.deepcopy(manifest)
repinned['patch_sha256'] = sha(changed)
rejected('a repinned changed before-hunk', {PATCH: changed}, 'TSF metadata reverse endpoint', repinned)

rejected('changed manifest without its independent pin', {MANIFEST: (ROOT / MANIFEST).read_bytes() + b'\n'},
         'TSF metadata qualification changed')
rejected('changed patch without its independent pin', {PATCH: transition_patch + b'\n'},
         'TSF metadata patch changed')
name = FOLDER + 'TsfJobSupervisor.cs'
rejected('changed original qualified controller', {name: (ROOT / name).read_bytes() + b'\n'}, name)
print('TCP_SELECTED_TSF_METADATA_NEGATIVE_CONTROLS_PASS cases=' + str(checks))
