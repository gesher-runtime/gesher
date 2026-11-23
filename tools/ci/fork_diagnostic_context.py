# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Authenticate an explicitly diagnostic DNS run without canonical authority.

This does not authorize a selected toolchain or change artifact acceptance.
The canonical DNS context remains separate. Only manual branch runs in a real
fork of Gesher may create this distinct, noncanonical evidence type.
"""
import json
import os
from pathlib import Path
import re
import subprocess

from github_read import get_json
from receipt_identity import exact_run

ROOT = Path(__file__).resolve().parents[2]
CANONICAL = 'gesher-runtime/gesher'
CANONICAL_ID = '1426153186'
SCOPE = 'fork-diagnostic'
WORKFLOWS = {'ci': '.github/workflows/ci.yml', 'native2': '.github/workflows/native2.yml'}
ENV_FIELDS = {
    'source': 'GITHUB_SHA', 'run': 'GITHUB_RUN_ID', 'attempt': 'GITHUB_RUN_ATTEMPT',
    'repository': 'GITHUB_REPOSITORY', 'repository_id': 'GITHUB_REPOSITORY_ID',
    'event': 'GITHUB_EVENT_NAME', 'ref': 'GITHUB_REF', 'workflow': 'GITHUB_WORKFLOW',
    'workflow_ref': 'GITHUB_WORKFLOW_REF', 'workflow_sha': 'GITHUB_WORKFLOW_SHA',
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def envelope(env, checkout, *, producer=False):
    require(env.get('GITHUB_ACTIONS') == 'true', 'Fork diagnostics require Actions')
    require(all(isinstance(env.get(key), str) and env[key] for key in ENV_FIELDS.values()),
            'Incomplete fork diagnostic environment')
    value = {name: env[key] for name, key in ENV_FIELDS.items()}
    repo, repo_id = value['repository'], value['repository_id']
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo) is not None and
            all(part not in ('.', '..') for part in repo.split('/')),
            'Invalid fork repository name')
    require(repo.casefold() != CANONICAL.casefold() and repo_id != CANONICAL_ID,
            'Canonical repository cannot issue fork diagnostic evidence')
    require(all(re.fullmatch(r'[1-9][0-9]*', value[key]) for key in ('repository_id', 'run', 'attempt')),
            'Invalid fork repository/run/attempt ID')
    require(re.fullmatch(r'[0-9a-f]{40}', value['source']) is not None and
            checkout == value['source'] == value['workflow_sha'],
            'Fork checkout/workflow source mismatch')
    require(value['event'] == 'workflow_dispatch', 'Fork DNS diagnostics require manual dispatch')
    require(value['ref'].startswith('refs/heads/') and len(value['ref']) > len('refs/heads/'),
            'Fork DNS diagnostics require a branch ref')
    path = WORKFLOWS.get(value['workflow'])
    require(path is not None and value['workflow_ref'] == repo + '/' + path + '@' + value['ref'],
            'Fork diagnostic workflow identity mismatch')
    jobs = {'native2'} if producer or value['workflow'] == 'native2' else {'native2', 'test-shards'}
    require(env.get('GITHUB_JOB') in jobs, 'Unexpected fork DNS producer/consumer job')
    return dict(value, evidence_scope=SCOPE, canonical_authority=False,
                upstream_repository=CANONICAL, upstream_repository_id=CANONICAL_ID)


def validate(env, event, repository, run, checkout, *, producer=False):
    """Pure identity controls; the live entry point supplies GitHub API records."""
    value = envelope(env, checkout, producer=producer)
    for observed in (event['repository'], repository):
        require(type(observed.get('id')) is int and str(observed['id']) == value['repository_id'] and
                observed.get('full_name') == value['repository'] and observed.get('fork') is True,
                'Event/API fork repository identity mismatch')
    upstream = repository.get('source', {})
    require(type(upstream.get('id')) is int and str(upstream['id']) == CANONICAL_ID and
            upstream.get('full_name') == CANONICAL,
            'Repository is not a fork of the canonical source')
    require(exact_run(run, value['run'], value['attempt'], value['source'], value['repository_id'],
                      value['repository_id'], value['repository'], value['workflow']),
            'Fork API run/source/attempt/repository mismatch')
    require(run.get('event') == value['event'] and
            run.get('head_branch') == value['ref'][len('refs/heads/'):] and
            run.get('path') == WORKFLOWS[value['workflow']] and
            run.get('head_repository', {}).get('full_name') == value['repository'],
            'Fork API event/branch/workflow/head repository mismatch')
    return value


def current_context(*, producer=False):
    checkout = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    # Validate the endpoint components before asking the unchanged exact GET helper.
    value = envelope(os.environ, checkout, producer=producer)
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8-sig'))
    repository = get_json('repos/' + value['repository'])
    run = get_json('repos/' + value['repository'] + '/actions/runs/' + value['run'])
    return validate(os.environ, event, repository, run, checkout, producer=producer)


def assert_current(value):
    """Keep the authenticated identity attached during one build/verify invocation."""
    checkout = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    require(value == envelope(os.environ, checkout), 'Fork diagnostic context changed during execution')
    return value
