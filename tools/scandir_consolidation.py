"""Preserve the complete historical Scandir proof through draft consolidation.

The archived observations remain observations of their original sources. Every
current report input is checked against an exact reversible whole-file
transition to the reviewed PR586 endpoint before the unchanged original
supplier-CI adapter and original archive/statistics checks execute. This grants
no Windows/native runtime approval and makes no new performance measurement.
"""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import scandir_ci_integration as original

BASELINE = '3a309b747fc604f84fa20a42bbd875d706116d6d'
ARCHIVE_VERIFIER_BASE = 'd5c8e60673cd99e388cdf4ec1e895039c5d9fe66'
ARCHIVE_VERIFIER_SHA256 = '8e62fae9c0b12f9cfa14665b4baab386270e95d69456f6e8b086ed6771cb7e01'
MANIFEST = 'tools/net-tcp-priority/qualified/scandir-consolidation-source.json'
PATCH = 'tools/net-tcp-priority/qualified/scandir-consolidation-source.patch'
ADAPTER = 'tools/scandir_consolidation.py'
MANIFEST_SHA256 = 'e97d632121615f43758ab64dae0853fefaeb274aadd8cba2961347e8d188a27c'
ORIGINAL_ADAPTER_SHA256 = 'f6bfd6c916300cbf95b31a6de4ad394b3c6da13cdd189339c0fc823a167481cd'
NATIVE2_ADAPTER = 'tools/scandir_native2_projection.py'
NATIVE2_ADAPTER_SHA256 = '92315526cfd0331ee455f892c2cff66f6b25ae91f585b3f31c190e5bb137969c'



def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(value, label):
    if not value:
        raise AssertionError('Scandir draft consolidation: ' + label)


def declaration(helper_digest, manifest_digest):
    return {'version': 1, 'reviewed_feature': BASELINE,
            'manifest_sha256': manifest_digest, 'adapter_sha256': helper_digest,
            'native_runtime_approval': False}


def integrated_verifier(archival_verifier, helper_digest):
    """Retain main's standalone archive verifier and add the branch source proof at CLI entry."""
    require(sha(archival_verifier) == ARCHIVE_VERIFIER_SHA256, 'immutable archive verifier')
    definition = b'def main():\n'
    anchor = (b"    for path, expected in report['sources'].items():\n"
              b"        require(digest(members['repo/' + path]) == expected, 'archived source: ' + path)\n")
    entry = b"if __name__ == '__main__':\n    main()\n"
    for marker in (definition, anchor, entry):
        require(archival_verifier.count(marker) == 1, 'unique archive verifier routing')
    source_check = (
        b"    if check_current_sources:\n"
        b"        from scandir_consolidation import historical_sources\n"
        b"        source_bytes = historical_sources(ROOT, report, members, '" + helper_digest.encode() + b"')\n"
        b"        for path, expected in report['sources'].items():\n"
        b"            require(digest(source_bytes[path]) == expected, 'current source: ' + path)\n"
    )
    return archival_verifier.replace(definition, b'def main(*, check_current_sources=False):\n').replace(
        anchor, source_check + anchor).replace(entry, b"if __name__ == '__main__':\n    main(check_current_sources=True)\n")


def directory_section(source):
    first = b'struct BionicDirent {'
    end = b'// --- the *at() calls and other path calls'
    require(source.count(first) == source.count(end) == 1, 'directory extraction boundaries')
    return source[source.index(first):source.index(end)]


def normalize(report, current, baseline, archived, helper, expected_helper,
              manifest_raw, patch, old_helper, archival_verifier, manifest_pin=MANIFEST_SHA256):
    require(sha(helper) == expected_helper, 'pinned consolidation adapter')
    require(sha(old_helper) == ORIGINAL_ADAPTER_SHA256, 'unchanged original adapter')
    require(sha(manifest_raw) == manifest_pin, 'pinned consolidation manifest')
    require(report.get('draft_consolidation') == declaration(expected_helper, manifest_pin),
            'exact draft source declaration')
    transition = json.loads(manifest_raw)
    require(set(transition) == {'version', 'baseline', 'native_runtime_approval', 'paths',
                               'before_sha256', 'after_sha256', 'patch_sha256'}, 'manifest schema')
    require(transition['version'] == 1 and transition['baseline'] == BASELINE and
            transition['native_runtime_approval'] is False, 'fixed baseline and unapproved runtime')
    source_paths = set(report['sources'])
    require(set(current) == set(baseline) == source_paths, 'complete source endpoints')
    paths = sorted(source_paths - {original.VERIFIER})
    require(transition['paths'] == paths and sorted(transition['before_sha256']) == paths and
            sorted(transition['after_sha256']) == paths, 'complete transition path set')
    require(sha(patch) == transition['patch_sha256'], 'pinned complete transition patch')
    changed = [path for path in paths if baseline[path] != current[path]]
    require(changed and [line for line in patch.splitlines() if line.startswith(b'diff --git ')] == [
        ('diff --git a/' + path + ' b/' + path).encode() for path in changed],
        'exact changed patch paths')
    for path in paths:
        require(sha(current[path]) == transition['after_sha256'][path], 'current source ' + path)
        require(sha(baseline[path]) == transition['before_sha256'][path], 'immutable baseline ' + path)
    require(current[original.VERIFIER] == integrated_verifier(archival_verifier, expected_helper),
            'complete current verifier reconstruction')
    for command in original.COMMANDS:
        require(current[original.CI].count(command) == 1, 'unique retained feature command')
    require(directory_section(current['compat/bionic/posix_bionic.cc']) ==
            directory_section(baseline['compat/bionic/posix_bionic.cc']),
            'unchanged measured directory implementation')
    with tempfile.TemporaryDirectory(prefix='gesher-scandir-consolidation-') as temporary:
        work = Path(temporary)
        for path in paths:
            (work / path).parent.mkdir(parents=True, exist_ok=True)
            (work / path).write_bytes(current[path])
        # This scratch tree has no repository attributes. Git must preserve
        # the exact source bytes even when the user's default is CRLF.
        apply_patch = ['git', '-c', 'core.autocrlf=false', 'apply']
        subprocess.run([*apply_patch, '--reverse', '--check', '-'], input=patch,
                       cwd=work, check=True, capture_output=True)
        subprocess.run([*apply_patch, '--reverse', '-'], input=patch,
                       cwd=work, check=True, capture_output=True)
        for path in paths:
            require((work / path).read_bytes() == baseline[path], 'exact reverse endpoint ' + path)
        subprocess.run([*apply_patch, '--check', '-'], input=patch,
                       cwd=work, check=True, capture_output=True)
        subprocess.run([*apply_patch, '-'], input=patch,
                       cwd=work, check=True, capture_output=True)
        for path in paths:
            require((work / path).read_bytes() == current[path], 'exact forward endpoint ' + path)
    # The original adapter retains its complete historical source pins,
    # archive identity, workflow reconstruction and verifier reconstruction.
    result = original.normalize(report, baseline, archived, old_helper, ORIGINAL_ADAPTER_SHA256)
    for path, expected in report['sources'].items():
        require(sha(result[path]) == expected, 'complete original source proof ' + path)
    return result


def negative_controls(report, current, baseline, archived, helper, expected_helper,
                      manifest_raw, patch, old_helper, archival_verifier):
    cases = 0

    def rejects(label, *, modified_report=None, modified_current=None, modified_baseline=None,
                modified_manifest=None, modified_patch=None, modified_helper=None,
                modified_old_helper=None, modified_archive_verifier=None, manifest_pin=MANIFEST_SHA256):
        nonlocal cases
        try:
            normalize(report if modified_report is None else modified_report,
                      current if modified_current is None else modified_current,
                      baseline if modified_baseline is None else modified_baseline,
                      archived, helper if modified_helper is None else modified_helper, expected_helper,
                      manifest_raw if modified_manifest is None else modified_manifest,
                      patch if modified_patch is None else modified_patch,
                      old_helper if modified_old_helper is None else modified_old_helper,
                      archival_verifier if modified_archive_verifier is None else modified_archive_verifier, manifest_pin)
        except (AssertionError, subprocess.CalledProcessError):
            cases += 1
        else:
            raise AssertionError('Scandir consolidation control accepted: ' + label)

    for path in current:
        rejects('changed current ' + path, modified_current={**current, path: current[path] + b'\n'})
    # A current source and its manifest may be repinned together, but the
    # immutable Git endpoint and the unchanged patch must still reconstruct.
    path = 'compat/bionic/posix_bionic.cc'
    altered = current[path] + b'\n// unqualified source change\n'
    changed_manifest = json.loads(manifest_raw)
    changed_manifest['after_sha256'][path] = sha(altered)
    changed_raw = (json.dumps(changed_manifest, sort_keys=True, indent=2) + '\n').encode()
    modified = copy.deepcopy(report)
    modified['draft_consolidation'] = declaration(expected_helper, sha(changed_raw))
    rejects('repinned changed source', modified_current={**current, path: altered},
            modified_report=modified, modified_manifest=changed_raw, manifest_pin=sha(changed_raw))
    for field in ('baseline', 'native_runtime_approval', 'paths'):
        changed_manifest = json.loads(manifest_raw)
        changed_manifest[field] = {'baseline': '0' * 40, 'native_runtime_approval': True,
                                   'paths': changed_manifest['paths'][1:]}[field]
        changed_raw = (json.dumps(changed_manifest, sort_keys=True, indent=2) + '\n').encode()
        modified = copy.deepcopy(report)
        modified['draft_consolidation'] = declaration(expected_helper, sha(changed_raw))
        rejects('repinned manifest ' + field, modified_report=modified,
                modified_manifest=changed_raw, manifest_pin=sha(changed_raw))
    rejects('changed patch', modified_patch=patch + b'\n')
    rejects('changed immutable endpoint', modified_baseline={**baseline, path: baseline[path] + b'\n'})
    rejects('changed new adapter', modified_helper=helper + b'\n')
    rejects('changed old adapter', modified_old_helper=old_helper + b'\n')
    rejects('changed archive verifier', modified_archive_verifier=archival_verifier + b'\n')
    for field in declaration(expected_helper, MANIFEST_SHA256):
        modified = copy.deepcopy(report)
        del modified['draft_consolidation'][field]
        rejects('missing declaration ' + field, modified_report=modified)
    for command in original.COMMANDS:
        rejects('omitted retained CI command', modified_current={**current,
                original.CI: current[original.CI].replace(command, b'')})
    # Do not replace the old adapter's controls with the new transition tests.
    old_cases = original.negative_controls(report, baseline, archived, old_helper, ORIGINAL_ADAPTER_SHA256)
    return cases, old_cases


def historical_sources(root, report, members, expected_helper):
    live_current = {path: (root / path).read_bytes() for path in report['sources']}
    # Native2 repairs descriptor ownership/relative operations. Its source-only
    # projection restores the exact qualified endpoint before the unchanged
    # whole-directory equality, archive proof and original controls below.
    adapter_source = (root / NATIVE2_ADAPTER).read_bytes()
    require(sha(adapter_source) == NATIVE2_ADAPTER_SHA256,
            'pinned Native2 compatibility adapter')
    # Bind execution to those exact bytes, including during local repinning.
    import types
    native2 = types.ModuleType('scandir_native2_projection')
    exec(compile(adapter_source, str(root / NATIVE2_ADAPTER), 'exec'), native2.__dict__)
    native2_manifest = (root / native2.MANIFEST).read_bytes()
    native2_patch = (root / native2.PATCH).read_bytes()
    native2_baseline = native2.baseline_source(root, live_current, native2_manifest, native2_patch)
    current = native2.project(live_current, native2_baseline, native2_manifest, native2_patch)
    native2.negative_controls(live_current, native2_baseline, native2_manifest, native2_patch)
    baseline = {path: subprocess.check_output(['git', '-C', str(root), 'show', BASELINE + ':' + path])
                for path in report['sources']}
    archived = {path: members['repo/' + path] for path in (original.CI, original.VERIFIER)}
    helper = (root / ADAPTER).read_bytes()
    old_helper = (root / original.ADAPTER).read_bytes()
    manifest_raw, patch = (root / MANIFEST).read_bytes(), (root / PATCH).read_bytes()
    archival_verifier = subprocess.check_output(
        ['git', '-C', str(root), 'show', ARCHIVE_VERIFIER_BASE + ':' + original.VERIFIER])
    args = (report, current, baseline, archived, helper, expected_helper, manifest_raw, patch, old_helper, archival_verifier)
    result = normalize(*args)
    controls, old_controls = negative_controls(*args)
    print(f'SCANDIR_DRAFT_CONSOLIDATION_PASS source_paths={len(current)} negative_controls={controls} '
          f'original_negative_controls={old_controls} native_runtime_approval=false')
    return result
