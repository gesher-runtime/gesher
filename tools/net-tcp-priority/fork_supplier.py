# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Strict fork-only TCP supplier types; no canonical receipt conversion."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import selected_supplier as selected
from payload_roles import ROLES

sys.path.insert(0, str(selected.ROOT / 'tools/toolchain'))
import fork_tcp_context as identity
import minimum_diagnostic as minimum

ROLE = 'gesher-tcp-supplier-fork-diagnostic-v1'
SCHEMA = 'gesher-tcp-fork-diagnostic-v1'
WORKSPACE = 'tcp-fork-diagnostic-supplier'
FACTORY = 'build-minimum-diagnostic.sh'
FACTORY_LOG = 'fork-diagnostic-factory.log'
ARTIFACT = 'tcp-fork-diagnostic-selected-minimum'


def check_supplier(supplier, descriptor):
    identity.check_descriptor(descriptor)
    identity.check_descriptor(supplier['diagnostic'])
    context = descriptor['context']
    assert supplier['evidence_schema'] == SCHEMA and supplier['diagnostic'] == descriptor
    authority = supplier['minimum_authority']
    identity.check_descriptor(authority['diagnostic'])
    assert authority['evidence_schema'] == minimum.SCHEMA and authority['diagnostic'] == descriptor
    original = authority['original']
    identity.check_descriptor(original['diagnostic'])
    assert original['evidence_schema'] == minimum.SCHEMA and original['diagnostic'] == descriptor
    selected._check_selected_facts(supplier, context, receipt_role=ROLE, authority_role=minimum.ROLE,
        publisher_role=minimum.PUBLISHER_ROLE, workspace_name=WORKSPACE,
        factory_name=FACTORY, factory_log=FACTORY_LOG)


def check_manifest(manifest, descriptor):
    identity.check_descriptor(descriptor)
    identity.check_descriptor(manifest['diagnostic'])
    context = descriptor['context']
    assert manifest['evidence_schema'] == SCHEMA and manifest['diagnostic'] == descriptor
    for name, key in (('source', 'GITHUB_SHA'), ('repository', 'GITHUB_REPOSITORY'),
                      ('repository_id', 'GITHUB_REPOSITORY_ID'), ('run', 'GITHUB_RUN_ID'),
                      ('attempt', 'GITHUB_RUN_ATTEMPT')):
        assert str(manifest[name]) == context[key]
    assert manifest['producer_job'] == 'compile' and manifest['supplier_selection'] == 'selected-minimum'
    assert manifest['scope'] == 'noninline-only'
    check_supplier(manifest['supplier'], descriptor)


def check_artifact(directory, descriptor, sources):
    """Verify the whole downloaded diagnostic member set before correlation."""
    assert directory.is_dir() and not directory.is_symlink()
    members = list(directory.iterdir())
    assert all(p.is_file() and not p.is_symlink() for p in members)
    assert {p.name for p in members} == set(ROLES) | {'provenance.json'}
    manifest = json.loads((directory / 'provenance.json').read_text(encoding='utf-8'))
    check_manifest(manifest, descriptor)
    assert manifest['sources'] == sources and set(manifest['outputs']) == set(ROLES)
    for name, role in ROLES.items():
        record = manifest['outputs'][name]
        assert record['role'] == role
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == record['sha256']
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    descriptor = identity.current('windows')
    check_manifest(json.loads(args.manifest.read_text()), descriptor)
    print('TCP_FORK_DIAGNOSTIC_SUPPLIER_VERIFIED ' + json.dumps(descriptor, sort_keys=True), flush=True)
