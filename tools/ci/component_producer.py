"""Stage fixed canonical component roles and authenticate their producer context.

The consumer keeps its real Actions attempt.  Stable artifact names are resolved
by the existing exact producer window and digest-checked batch downloader; an
older manifest is accepted only by producer_context's retained-execution proof.
"""
import argparse
from contextlib import redirect_stdout
import json
import os
from pathlib import Path
import shutil
import sys
from types import SimpleNamespace

import artifact_receipt as receipts
import producer_context as proof


ROOT = Path(__file__).resolve().parents[2]
# All suppliers and paths are source policy.  Neither a manifest nor a caller
# can nominate a workflow, producer job, artifact name or destination.
POLICIES = {
    'tcp-selected-minimum': ('tcp-selected-minimum', 'tcp_selected_artifacts.json',
                             'tcp_selected_download', 'tcp-fixture'),
    'tcp-selected-correlation': ('tcp-selected-minimum', 'tcp_selected_artifacts.json',
                                 'tcp_correlation_download', 'ci-suppliers/tcp_selected'),
    'tsf-format': ('tsf-format', 'tsf_format_artifacts.json',
                   'tsf_format_download', 'tsf-fixture'),
    'native3-blocking': ('native3-blocking', 'native3_blocking_artifacts.json',
                        'native3_blocking_download', 'command-fixture'),
}
CONTEXT_FIELDS = {
    'GITHUB_SHA': 'source', 'GITHUB_RUN_ID': 'run', 'GITHUB_RUN_ATTEMPT': 'attempt',
    'GITHUB_REPOSITORY': 'repository', 'GITHUB_REPOSITORY_ID': 'repository_id',
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def policy(name):
    require(name in POLICIES, 'Unknown canonical component role')
    require(os.environ.get('GITHUB_ACTIONS') == 'true' and
            os.environ.get('GITHUB_WORKFLOW') == 'ci', 'Canonical CI components only')
    return POLICIES[name]


def supplier_context(context):
    """Project an authenticated producer context for existing pure validators."""
    return {field: context[key] for field, key in CONTEXT_FIELDS.items()}


def authenticate(path, role, consumer, *, expected_supplier=None):
    raw = path.read_bytes()
    original = proof.authenticate(path, consumer, role, expected_supplier=expected_supplier)
    require(path.read_bytes() == raw, 'Component manifest changed during authentication')
    manifest = json.loads(raw)
    require(all(manifest.get(key) == value for key, value in original.items()),
            'Component manifest differs from its authenticated producer context')
    return manifest, original


def verify(name, *, path=None, consumer=None):
    role, _, _, destination = policy(name)
    expected = ROOT / destination / 'provenance.json'
    if path is not None:
        require(Path(path).absolute() == expected.absolute(), 'Unexpected component manifest path')
    consumer = proof.current_context() if consumer is None else consumer
    return authenticate(expected, role, consumer)


def stage(name):
    role, manifest, staging, destination = policy(name)
    destination = ROOT / destination
    for parent in (destination, *destination.parents):
        require(not parent.is_symlink() and
                not (hasattr(parent, 'is_junction') and parent.is_junction()),
                'Linked component destination')
    require(not os.path.lexists(destination), 'Component destination already exists')
    consumer = proof.current_context()
    batch = ROOT / 'ci-suppliers' / staging
    selected = receipts.batch(SimpleNamespace(
        batch=ROOT / 'tools/ci' / manifest, destination=batch))
    artifact_name = proof.ROLES[role][1]
    require(set(selected) == {artifact_name}, 'Unexpected component batch role')
    staged = batch / 'artifacts' / 'fixture'
    _, original = authenticate(staged / 'provenance.json', role, consumer,
                               expected_supplier=selected[artifact_name])
    raw = (staged / 'provenance.json').read_bytes()
    # Reserve a new final directory exclusively, including a racing existing
    # destination.  Keep the unchanged runners' flat paths without overwriting
    # any previous payload.  No process runs from a partially published role.
    destination.mkdir()
    try:
        for member in staged.iterdir():
            member.rename(destination / member.name)
        require((destination / 'provenance.json').read_bytes() == raw,
                'Component manifest changed during publication')
    except BaseException:
        shutil.rmtree(destination)
        raise
    job, artifact = receipts.supplier_identity(selected[artifact_name])
    return {'producer_context': original, 'consumer_attempt': consumer['attempt'],
            'producer': job, 'artifact': artifact}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('stage', 'verify'))
    parser.add_argument('role', choices=POLICIES)
    args = parser.parse_args()
    # Authentication diagnostics remain visible, while PowerShell receives one
    # unambiguous JSON value on stdout and can reject a nonzero process exit.
    with redirect_stdout(sys.stderr):
        if args.mode == 'stage':
            result = stage(args.role)
        else:
            _, original = verify(args.role)
            result = {'producer_context': original}
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
