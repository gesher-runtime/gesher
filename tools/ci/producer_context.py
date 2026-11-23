"""Authenticate a retained successful producer without changing consumer identity.

GitHub copies successful job records into a partial rerun with new job IDs and
run_attempt values, but preserves their original execution window and artifact.
An archived role therefore keeps its original attempt.  A different attempt is
accepted only after proving that exact original execution and archive again.
"""
import json
import os
from pathlib import Path
import re
import tempfile

import artifact_receipt as receipts


CONTEXT_KEYS = ('source', 'repository', 'repository_id', 'run', 'attempt',
                'event', 'supplier_head', 'head_repository_id')
# Names are source policy, never taken from an untrusted role or command line.
ROLES = {
    'production': ('web3-production-role.json', 'windows-x64', 'build', None),
    'query-hook': ('web3-query-hook-role.json', 'web3-query-hook', 'build', None),
    'capture-production': ('media-capture-production-role.json', 'windows-x64', 'build', None),
    'capture-fixture': ('media-capture-fixture-role.json', 'media-capture-test', 'build', None),
    'binder-ordinary-actor': ('binder-host-source.json', 'binder2-sg-host', 'build', None),
    'binder-ordinary-modes': ('source.json', 'binder2-sg-modes', {
        'ci': 'binder2-sg-fixtures / modes-build',
        'binder2-sg-modes-component': 'fixtures / modes-build',
        'binder2-stable-readonly-component': 'fixtures / modes-build',
    }, None),
    'native2-udp': ('net-udp/manifest.json', 'native2', {'ci': 'native2 / native2'}, None),
    'native2-dns': ('net-dns/manifest.json', 'native2', {'ci': 'native2 / native2'}, 'context'),
    'tcp-selected-minimum': ('provenance.json', 'tcp-coherent-selected-minimum',
                             {'ci': 'compile-selected-minimum'}, None),
    'tsf-format': ('provenance.json', 'tsf-format', {'ci': 'union-tsf / compile'}, None),
    'native3-blocking': ('provenance.json', 'native3-locks-blocking',
                         {'ci': 'union-native-blocking / compile'}, None),
    'native3-common-host-fixture': ('provenance.json', 'native3-locks-host', 'build', 'context'),
    'native3-linux-reference': ('provenance.json', 'native3-locks-reference',
                               'native3-locks-reference', 'context'),
}
# These consumers always bind the manifest bytes to the exact selected archive,
# even when the manifest claims the current attempt. Relabelling an old local
# manifest as current must not turn the historical proof into a fast-path bypass.
ARCHIVE_REQUIRED = frozenset(('binder-ordinary-modes', 'native2-udp', 'native2-dns',
                             'tcp-selected-minimum', 'tsf-format', 'native3-blocking'))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def _unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, 'Duplicate producer role key')
        value[key] = item
    return value


def _invalid_constant(_):
    raise RuntimeError('Invalid producer role JSON constant')


def current_context():
    """Read the complete CI identity, including a PR's distinct checkout/head."""
    value = receipts.context()
    return dict(source=value['checkout'], repository=value['repo'], repository_id=value['repo_id'],
                run=value['run_id'], attempt=value['attempt'], event=os.environ['GITHUB_EVENT_NAME'],
                supplier_head=value['head'], head_repository_id=value['head_repo'])


def receipt_context(info, workflow):
    return dict(repo=info['repository'], repo_id=info['repository_id'],
                run_id=info['run'], attempt=info['attempt'], checkout=info['source'],
                head=info['supplier_head'], head_repo=info['head_repository_id'], workflow=workflow)


def snapshot(current, original, artifact_name, producer_name):
    endpoint = f"repos/{current['repo']}/actions/runs/{current['run_id']}"
    # An explicit historical endpoint, not a relabelled latest-attempt record.
    run = receipts.get_json(f"{endpoint}/attempts/{original['attempt']}")
    require(receipts.exact_run(run, original['run_id'], original['attempt'], original['head'],
                              original['repo_id'], original['head_repo'], original['repo'],
                              original['workflow']), 'Original producer run identity mismatch')
    require(run.get('event') == os.environ['GITHUB_EVENT_NAME'], 'Original producer event mismatch')
    old = receipts.select_producer(original, receipts.read_jobs(original), producer_name)
    now = receipts.select_producer(current, receipts.read_jobs(current), producer_name)
    execution = ('name', 'head_sha', 'status', 'conclusion', 'started_at', 'completed_at')
    require(all(old.get(key) == now.get(key) for key in execution),
            'Current producer is not the retained original successful execution')
    inventory = receipts.read_artifacts(f'{endpoint}/artifacts', name=artifact_name)
    old_item = receipts.select_supplier(original, inventory, artifact_name, old)
    item = receipts.select_supplier(current, inventory, artifact_name, now)
    require(old_item == item, 'Original and current producer selected different artifacts')
    # Last API read detects a newly started run attempt during the inventories.
    receipts.check_run(current)
    return old, now, item


def authenticate(path, consumer, role, *, expected_supplier=None):
    """Return the proved producer context; leave GITHUB_RUN_ATTEMPT untouched."""
    require(role in ROLES, 'Unknown producer role')
    member, artifact_name, producer_name, nested = ROLES[role]
    if isinstance(producer_name, dict):
        producer_name = producer_name.get(os.environ.get('GITHUB_WORKFLOW'))
        require(producer_name is not None, 'Producer role is not permitted in this workflow')
    path = Path(path)
    member_parts = Path(member).parts
    require(path.parts[-len(member_parts):] == member_parts and path.is_file() and all(
        not part.is_symlink() and not (hasattr(part, 'is_junction') and part.is_junction())
        for part in (path, *path.parents)),
            'Missing or linked producer role file')
    raw = path.read_bytes()
    payload = json.loads(raw, object_pairs_hook=_unique_object, parse_constant=_invalid_constant)
    require(isinstance(payload, dict), 'Producer role is not an object')
    require(payload.get('role') == role, 'Producer role mismatch')
    header = payload if nested is None else payload[nested]
    require(set(consumer) == set(CONTEXT_KEYS), 'Incomplete consumer context')
    if nested is not None:
        require(set(header) == set(CONTEXT_KEYS), 'Producer context keyset mismatch')
    original = {key: header[key] for key in CONTEXT_KEYS}
    require(all(original[key] == consumer[key] for key in CONTEXT_KEYS if key != 'attempt'),
            'Producer source/checkout/run/repository/event mismatch')
    require(all(isinstance(value, str) and re.fullmatch(r'[1-9][0-9]*', value)
                for value in (original['attempt'], consumer['attempt'])), 'Invalid producer attempt')
    if original['attempt'] == consumer['attempt'] and role not in ARCHIVE_REQUIRED and expected_supplier is None:
        return original
    require(int(original['attempt']) <= int(consumer['attempt']), 'Producer attempt is in the future')
    require(os.environ.get('GITHUB_ACTIONS') == 'true', 'Reused producer proof executes only in CI')
    current = receipt_context(consumer, os.environ['GITHUB_WORKFLOW'])
    earlier = receipt_context(original, current['workflow'])
    receipts.check_checkout(current)
    receipts.check_run(current)
    before = snapshot(current, earlier, artifact_name, producer_name)
    if expected_supplier is not None:
        require(receipts.supplier_identity((before[1], before[2])) ==
                receipts.supplier_identity(expected_supplier), 'Producer differs from the verified batch supplier')
    # Reuse the unchanged digest-checked downloader/ZIP path and member policy.
    # Comparing the entire archived role binds its attempt, compiler, source and
    # output hashes to the exact API-selected archive, not merely local claims.
    with tempfile.TemporaryDirectory(prefix='gesher-producer-context-') as temporary:
        extracted = Path(temporary) / 'artifact'
        receipts.download(before[2], extracted)
        archived = extracted / member
        require(archived.is_file() and not archived.is_symlink() and archived.read_bytes() == raw,
                'Producer role bytes differ from the verified original archive')
    after = snapshot(current, earlier, artifact_name, producer_name)
    if expected_supplier is not None:
        require(receipts.supplier_identity((after[1], after[2])) ==
                receipts.supplier_identity(expected_supplier), 'Producer differs from the verified batch supplier')
    require(receipts.supplier_identity((before[0], before[2])) ==
            receipts.supplier_identity((after[0], after[2])) and
            receipts.supplier_identity((before[1], before[2])) ==
            receipts.supplier_identity((after[1], after[2])),
            'Reused producer identity changed during archive verification')
    receipts.check_checkout(current)
    require(path.read_bytes() == raw, 'Producer role changed during archive verification')
    print('CI_AUTHENTICATED_PRODUCER_CONTEXT ' + json.dumps(dict(
        role=role, source=original['source'], supplier_head=original['supplier_head'],
        run=original['run'], producer_attempt=original['attempt'], consumer_attempt=consumer['attempt'],
        original_producer_id=after[0]['id'], current_producer_id=after[1]['id'],
        artifact=after[2]['id'], digest=after[2]['digest']), sort_keys=True), flush=True)
    return original
