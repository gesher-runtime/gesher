"""Source compatibility proof only: Native2 ownership fixes are not new measurements.

Project exactly the reviewed live Bionic source back to the immutable qualified
3c7 endpoint before running the unchanged historical Scandir proof. Keep the
actual sorting routine identical; old observations remain old observations.
"""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

BASELINE = '3c7fe0dd2813cd6495e303405b974fc46dfea30f'
PATH = 'compat/bionic/posix_bionic.cc'
BASELINE_SHA256 = '9ba77321ff4a4fedd64b9dfc433a5c93349daf6eef90552bda95763b0ef3a531'
MANIFEST = 'tools/net-tcp-priority/qualified/scandir-native2-source.json'
PATCH = 'tools/net-tcp-priority/qualified/scandir-native2-source.patch'
MANIFEST_SHA256 = '40ea1456c33611e66c174f92a2f3c9fe47d9cc5a40ad06f2783976694d4aaa8b'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, label):
    if not condition:
        raise AssertionError('Scandir Native2 projection: ' + label)


def sorting_section(source):
    start = b"// scandir with the .so's System V filter and comparator; alphasort is the"
    end = b'// --- the *at() calls and other path calls'
    require(source.count(start) == source.count(end) == 1, 'sorting boundaries')
    return source[source.index(start):source.index(end)]


def project(current, baseline, manifest_raw, patch, manifest_pin=MANIFEST_SHA256):
    require(sha(manifest_raw) == manifest_pin, 'pinned manifest')
    manifest = json.loads(manifest_raw)
    require(set(manifest) == {'version', 'baseline', 'paths', 'before_sha256', 'after_sha256',
                             'patch_sha256', 'sorting_sha256', 'source_compatibility_only',
                             'native_runtime_approval'}, 'manifest schema')
    require(manifest['version'] == 1 and manifest['baseline'] == BASELINE, 'immutable endpoint identity')
    require(manifest['source_compatibility_only'] is True and
            manifest['native_runtime_approval'] is False, 'source compatibility only')
    require(manifest['paths'] == [PATH] and sorted(manifest['before_sha256']) == [PATH] and
            sorted(manifest['after_sha256']) == [PATH], 'exact source scope')
    require(manifest['before_sha256'][PATH] == BASELINE_SHA256 and
            (baseline is None or sha(baseline) == BASELINE_SHA256),
            'immutable baseline bytes')
    require(PATH in current and sha(current[PATH]) == manifest['after_sha256'][PATH], 'live source bytes')
    require(sha(patch) == manifest['patch_sha256'], 'pinned complete patch')
    require([line for line in patch.splitlines() if line.startswith(b'diff --git ')] == [
        ('diff --git a/' + PATH + ' b/' + PATH).encode()], 'exact patch scope')
    if baseline is not None:
        require(sha(sorting_section(baseline)) == manifest['sorting_sha256'] and
                sorting_section(current[PATH]) == sorting_section(baseline), 'unchanged sorting routine')
    with tempfile.TemporaryDirectory(prefix='gesher-scandir-native2-') as temporary:
        work = Path(temporary)
        target = work / PATH
        target.parent.mkdir(parents=True)
        target.write_bytes(current[PATH])
        apply = ['git', '-c', 'core.autocrlf=false', 'apply']
        subprocess.run([*apply, '--reverse', '--check', '-'], cwd=work, input=patch,
                       check=True, capture_output=True)
        subprocess.run([*apply, '--reverse', '-'], cwd=work, input=patch,
                       check=True, capture_output=True)
        reconstructed = target.read_bytes()
        require(sha(reconstructed) == BASELINE_SHA256 and
                (baseline is None or reconstructed == baseline), 'exact reverse endpoint')
        baseline = reconstructed
        require(sha(sorting_section(baseline)) == manifest['sorting_sha256'] and
                sorting_section(current[PATH]) == sorting_section(baseline), 'unchanged sorting routine')
        subprocess.run([*apply, '--check', '-'], cwd=work, input=patch,
                       check=True, capture_output=True)
        subprocess.run([*apply, '-'], cwd=work, input=patch, check=True, capture_output=True)
        require(target.read_bytes() == current[PATH], 'exact forward endpoint')
    projected = dict(current)
    projected[PATH] = baseline
    require(set(projected) == set(current), 'complete projected source set')
    return projected


def baseline_source(root, current, manifest_raw, patch):
    # Squash merging can remove the feature commit from later checkout graphs.
    # Reconstruct its independently pinned bytes; retain Git as an extra check
    # when the original object is available, never as an availability condition.
    baseline = project(current, None, manifest_raw, patch)[PATH]
    available = subprocess.run(['git', '-C', str(root), 'cat-file', '-e', BASELINE + '^{commit}'],
                               capture_output=True).returncode == 0
    if available:
        historical = subprocess.check_output(['git', '-C', str(root), 'show', BASELINE + ':' + PATH])
        require(historical == baseline, 'available Git baseline cross-check')
    return baseline


def negative_controls(current, baseline, manifest_raw, patch, manifest_pin=MANIFEST_SHA256):
    cases = 0

    def rejects(label, *, source=None, old=None, raw=None, delta=None, pin=manifest_pin, expected=None):
        nonlocal cases
        try:
            project(current if source is None else source, baseline if old is None else old,
                    manifest_raw if raw is None else raw, patch if delta is None else delta, pin)
        except AssertionError as error:
            if expected is not None:
                require(error.args == ('Scandir Native2 projection: ' + expected,),
                        'negative control reached intended layer: ' + label)
            cases += 1
        except subprocess.CalledProcessError:
            require(expected is None, 'negative control failed before its intended assertion: ' + label)
            cases += 1
        else:
            raise AssertionError('Scandir Native2 control accepted: ' + label)

    rejects('live bytes changed', source={**current, PATH: current[PATH] + b'\n'}, expected='live source bytes')
    manifest = json.loads(manifest_raw)
    altered = current[PATH] + b'\n// unqualified source\n'
    updated = copy.deepcopy(manifest); updated['after_sha256'][PATH] = sha(altered)
    raw = (json.dumps(updated, sort_keys=True, indent=2) + '\n').encode()
    rejects('repinned live bytes', source={**current, PATH: altered}, raw=raw, pin=sha(raw))
    # Refresh the new source/manifest hashes so an outer checksum cannot stand
    # in for the independent unchanged-algorithm assertion.
    section = sorting_section(current[PATH])
    marker = b'to[out] = compare(&x, &y) <= 0 ? from[left++] : from[right++];'
    require(section.count(marker) == 1, 'sort mutation anchor')
    altered = current[PATH].replace(marker, marker.replace(b'<= 0', b'< 0'), 1)
    updated = copy.deepcopy(manifest); updated['after_sha256'][PATH] = sha(altered)
    raw = (json.dumps(updated, sort_keys=True, indent=2) + '\n').encode()
    rejects('repinned sorting code', source={**current, PATH: altered}, raw=raw, pin=sha(raw),
            expected='unchanged sorting routine')
    rejects('baseline bytes changed', old=baseline + b'\n', expected='immutable baseline bytes')
    updated = copy.deepcopy(manifest); updated['before_sha256'][PATH] = sha(baseline + b'\n')
    raw = (json.dumps(updated, sort_keys=True, indent=2) + '\n').encode()
    rejects('repinned baseline bytes', old=baseline + b'\n', raw=raw, pin=sha(raw),
            expected='immutable baseline bytes')
    for field, value, expected in (
            ('baseline', '0' * 40, 'immutable endpoint identity'),
            ('paths', [], 'exact source scope'),
            ('native_runtime_approval', True, 'source compatibility only'),
            ('source_compatibility_only', False, 'source compatibility only')):
        updated = copy.deepcopy(manifest); updated[field] = value
        raw = (json.dumps(updated, sort_keys=True, indent=2) + '\n').encode()
        rejects('repinned ' + field, raw=raw, pin=sha(raw), expected=expected)
    rejects('patch changed', delta=patch + b'\n', expected='pinned complete patch')
    updated = copy.deepcopy(manifest); bad_patch = b'not a reversible patch\n'
    updated['patch_sha256'] = sha(bad_patch)
    raw = (json.dumps(updated, sort_keys=True, indent=2) + '\n').encode()
    rejects('repinned malformed patch', raw=raw, delta=bad_patch, pin=sha(raw), expected='exact patch scope')
    print('SCANDIR_NATIVE2_PROJECTION_PASS source_compatibility_only=true native_runtime_approval=false '
          'negative_controls=' + str(cases))
    return cases
