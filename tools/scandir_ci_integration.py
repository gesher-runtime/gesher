"""Bind the unchanged Scandir archive to its one reviewed supplier-CI merge.

Only the workflow and the report verifier changed after measurement. Require
their complete reviewed endpoints before presenting their archived predecessors
to the original verifier. All other current source bytes pass through unchanged.
"""
import copy
import hashlib
import json

CI = '.github/workflows/ci.yml'
VERIFIER = 'tools/test_scandir_report.py'
ADAPTER = 'tools/scandir_ci_integration.py'
OLD_CI = 'af2513f090a06862285dc1687395084a7dcd2435410711f920bf9e6e0c1f5f4d'
OLD_VERIFIER = '44a9cf647cdf989b2eb6aedb26cb73a8098bc8f754c2393694a507246f9faae2'
NEW_CI = '5484d9c10c388bc1be865392d7b585dd660edd118ce3ff1d0c3df5dbd8cbd0f2'
MAIN_CI = '37c525f6bf01b7a96dcea34f76566c0e1523582441e26d20d342dc2b297b4618'
SOURCE_PINS = 'e0f41f53e795558fcaee0f898425b5b44f0e14c444bdfec2b673a58b5629b1ff'
ARCHIVE = {
    'path': 'docs/research/data/scandir-stable-sort-2026-11-23.tar.gz',
    'sha256': 'cae0558ee56a97d3bee5ff5b2e5a9e816d3fd53707ae30ddab541502c406f770',
    'bytes': 1342719, 'members': 728,
}
COMMANDS = (
    b'          python3 tools/test_memrchr_contracts.py --sanitize --negative-controls\n',
    b'          python3 tools/test_memrchr_report.py\n',
    b'          python3 tools/test_scandir_sort.py --sanitize --negative-controls --output-dir scandir-sort-contracts\n',
    b'          python3 tools/test_scandir_report.py\n',
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(value, label):
    if not value:
        raise AssertionError('Scandir CI integration: ' + label)


def transition(helper_digest):
    return {
        'version': 1,
        'published_feature': '0654cde8fb473b463a0ac5ad29149d6a5b2037c0',
        'incoming_main': '52513f0fc3f5fc657138cf01429cf969b2f21649',
        'canonical_merge_tree': 'e821b52bbaf8f8582ec96313ad1573a1d08ee755',
        'normalized_paths': [CI, VERIFIER],
        'workflow_before_sha256': OLD_CI,
        'workflow_after_sha256': NEW_CI,
        'incoming_workflow_sha256': MAIN_CI,
        'verifier_before_sha256': OLD_VERIFIER,
        'adapter_sha256': helper_digest,
    }


def integrated_verifier(original, helper_digest):
    """The complete original verifier plus exactly two fixed source edits."""
    require(sha(original) == OLD_VERIFIER, 'archived verifier endpoint')
    anchor = b"    for path, expected in report['sources'].items():\n"
    before = b'digest((ROOT / path).read_bytes())'
    require(original.count(anchor) == 1 and original.count(before) == 1, 'verifier insertion anchors')
    addition = (
        b'    from scandir_ci_integration import historical_sources\n'
        + f"    source_bytes = historical_sources(ROOT, report, members, '{helper_digest}')\n".encode()
    )
    return original.replace(anchor, addition + anchor).replace(before, b'digest(source_bytes[path])')


def normalize(report, current, archived, helper, expected_helper):
    helper_digest = sha(helper)
    require(helper_digest == expected_helper, 'pinned adapter bytes')
    pins = json.dumps(report['sources'], sort_keys=True, separators=(',', ':')).encode()
    require(sha(pins) == SOURCE_PINS, 'unchanged complete historical source pins')
    require(report['archive'] == ARCHIVE, 'unchanged historical archive identity')
    require(set(current) == set(report['sources']), 'complete current source set')
    recorded = report.get('supplier_bundle_integration', {}).get('source_transition')
    require(recorded == transition(helper_digest), 'exact declared source transition')
    require(sha(archived[CI]) == OLD_CI, 'archived workflow endpoint')
    require(sha(current[CI]) == NEW_CI, 'current workflow endpoint')
    incoming = current[CI]
    for command in COMMANDS:
        require(incoming.count(command) == 1, 'unique retained feature command')
        incoming = incoming.replace(command, b'')
    require(sha(incoming) == MAIN_CI, 'complete incoming workflow retained')
    require(current[VERIFIER] == integrated_verifier(archived[VERIFIER], helper_digest),
            'complete verifier reconstruction')
    result = dict(current)
    result[CI], result[VERIFIER] = archived[CI], archived[VERIFIER]
    return result


def negative_controls(report, current, archived, helper, expected_helper):
    """Exercise real endpoint checks, including repinned and omitted inputs."""
    cases = 0

    def rejects(label, *, changed_report=None, changed_current=None,
                changed_archived=None, changed_helper=None, pin=None):
        nonlocal cases
        try:
            normalize(report if changed_report is None else changed_report,
                      current if changed_current is None else changed_current,
                      archived if changed_archived is None else changed_archived,
                      helper if changed_helper is None else changed_helper,
                      expected_helper if pin is None else pin)
        except AssertionError:
            cases += 1
        else:
            raise AssertionError('Scandir CI integration control accepted: ' + label)

    for path in (CI, VERIFIER):
        rejects('additional current source ' + path,
                changed_current={**current, path: current[path] + b'\n# undeclared change\n'})
        rejects('changed archived endpoint ' + path,
                changed_archived={**archived, path: archived[path] + b'\n'})
        modified = copy.deepcopy(report)
        modified['sources'][path] = sha(current[path])
        rejects('repinned historical source ' + path, changed_report=modified)
    for command in COMMANDS:
        rejects('omitted retained feature command',
                changed_current={**current, CI: current[CI].replace(command, b'')})
    rejects('old workflow', changed_current={**current, CI: archived[CI]})
    rejects('old verifier', changed_current={**current, VERIFIER: archived[VERIFIER]})
    rejects('changed adapter', changed_helper=helper + b'\n')
    rejects('repinned changed adapter', changed_helper=helper + b'\n', pin=sha(helper + b'\n'))
    rejects('wrong adapter pin', pin='0' * 64)
    modified = copy.deepcopy(report)
    changed_ci = current[CI] + b'\n# undeclared workflow change\n'
    modified['supplier_bundle_integration']['source_transition']['workflow_after_sha256'] = sha(changed_ci)
    rejects('repinned current workflow', changed_report=modified,
            changed_current={**current, CI: changed_ci})
    modified = copy.deepcopy(report)
    del modified['sources']['compat/bionic/posix_bionic.cc']
    rejects('omitted historical source pin', changed_report=modified)
    for field in transition(expected_helper):
        modified = copy.deepcopy(report)
        modified['supplier_bundle_integration']['source_transition'][field] = None
        rejects('changed transition ' + field, changed_report=modified)
    modified = copy.deepcopy(report)
    del modified['supplier_bundle_integration']
    rejects('missing transition', changed_report=modified)
    modified = copy.deepcopy(report)
    modified['archive']['sha256'] = '0' * 64
    rejects('repinned historical archive', changed_report=modified)
    path = 'compat/bionic/posix_bionic.cc'
    result = normalize(report, {**current, path: current[path] + b'\n'}, archived,
                       helper, expected_helper)
    require(result[path] == current[path] + b'\n' and sha(result[path]) != report['sources'][path],
            'unrelated production changes still reach the original rejection')
    cases += 1
    return cases


def historical_sources(root, report, members, expected_helper):
    current = {path: (root / path).read_bytes() for path in report['sources']}
    archived = {path: members['repo/' + path] for path in (CI, VERIFIER)}
    helper = (root / ADAPTER).read_bytes()
    result = normalize(report, current, archived, helper, expected_helper)
    controls = negative_controls(report, current, archived, helper, expected_helper)
    print(f'SCANDIR_CI_INTEGRATION_PASS normalized_paths=2 negative_controls={controls}')
    return result
