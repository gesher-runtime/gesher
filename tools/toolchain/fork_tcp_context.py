# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Authenticate manual fork TCP diagnostics, without selected canonical authority."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ci'))
from github_read import get_json
from receipt_identity import exact_run

CANONICAL = 'gesher-runtime/gesher'
CANONICAL_ID = '1426153186'
WORKFLOW = 'ci'
WORKFLOW_PATH = '.github/workflows/ci.yml'
SCHEMA = 'gesher-tcp-fork-context-v1'
JOBS = ('compile', 'linux-reference', 'windows', 'supplier-correlation')
FIELDS = ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT',
          'GITHUB_REPOSITORY', 'GITHUB_REPOSITORY_ID')
EXTRA = dict(event='GITHUB_EVENT_NAME', ref='GITHUB_REF', workflow='GITHUB_WORKFLOW',
             workflow_ref='GITHUB_WORKFLOW_REF', workflow_sha='GITHUB_WORKFLOW_SHA',
             authorized_source='GESHER_COHERENT_TCP_AUTHORIZED_SOURCE',
             policy_ref='GESHER_COHERENT_TCP_POLICY_REF')


def require(value, message):
    if not value:
        raise RuntimeError(message)


def envelope(env, checkout, job):
    require(job in JOBS and env.get('GITHUB_JOB') == job, 'Wrong fork TCP job')
    require(env.get('GITHUB_ACTIONS') == 'true', 'Fork TCP diagnostics require Actions')
    require(all(isinstance(env.get(k), str) and env[k] for k in (*FIELDS, *EXTRA.values())),
            'Incomplete fork TCP identity')
    context = {k: env[k] for k in FIELDS}
    repo = context['GITHUB_REPOSITORY']
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo) is not None and
            all(part not in ('.', '..') for part in repo.split('/')), 'Invalid fork repository name')
    require(repo.casefold() != CANONICAL.casefold() and
            context['GITHUB_REPOSITORY_ID'] != CANONICAL_ID,
            'Canonical repository cannot issue fork TCP diagnostics')
    require(all(re.fullmatch(r'[1-9][0-9]*', context[k]) for k in FIELDS[1:3] + FIELDS[4:]),
            'Invalid fork repository/run/attempt ID')
    source = context['GITHUB_SHA']
    require(re.fullmatch(r'[0-9a-f]{40}', source) is not None and
            checkout == source == env['GITHUB_WORKFLOW_SHA'] == env['GESHER_COHERENT_TCP_AUTHORIZED_SOURCE'],
            'Fork TCP checkout/workflow/authorized source differs')
    require(env['GITHUB_EVENT_NAME'] == 'workflow_dispatch', 'Fork TCP requires manual dispatch')
    ref = env['GITHUB_REF']
    require(ref.startswith('refs/heads/') and len(ref) > len('refs/heads/'), 'Fork TCP requires a branch')
    require(env['GITHUB_WORKFLOW'] == WORKFLOW and
            env['GITHUB_WORKFLOW_REF'] == repo + '/' + WORKFLOW_PATH + '@' + ref and
            env['GESHER_COHERENT_TCP_POLICY_REF'] == repo + '/' + WORKFLOW_PATH + '@' + source,
            'Fork TCP workflow/policy identity differs')
    if job != 'linux-reference':
        require(env.get('TCP_SUPPLIER_ROLE') == 'selected-minimum', 'Wrong fork TCP supplier recipe')
    return dict(schema=SCHEMA, scope='fork-diagnostic', canonical_authority=False,
                context=context, upstream_repository=CANONICAL, upstream_repository_id=CANONICAL_ID,
                **{k: env[v] for k, v in EXTRA.items()})


def check_descriptor(value):
    """Pure shape/type check; live callers separately authenticate the API records."""
    require(type(value) is dict and type(value.get('context')) is dict and
            value.get('canonical_authority') is False, 'Wrong fork TCP descriptor type')
    env = dict(value['context'], **{v: value[k] for k, v in EXTRA.items()},
               GITHUB_ACTIONS='true', GITHUB_JOB='windows', TCP_SUPPLIER_ROLE='selected-minimum')
    require(value == envelope(env, value['context']['GITHUB_SHA'], 'windows'),
            'Invalid fork TCP descriptor')


def validate(env, event, repository, run, checkout, job):
    value = envelope(env, checkout, job)
    context = value['context']
    for record in (event['repository'], repository):
        require(type(record.get('id')) is int and str(record['id']) == context['GITHUB_REPOSITORY_ID'] and
                record.get('full_name') == context['GITHUB_REPOSITORY'] and record.get('fork') is True,
                'Fork TCP event/API repository differs')
    source = repository.get('source', {})
    require(type(source.get('id')) is int and str(source['id']) == CANONICAL_ID and
            source.get('full_name') == CANONICAL, 'Fork TCP repository has a different upstream')
    require(exact_run(run, context['GITHUB_RUN_ID'], context['GITHUB_RUN_ATTEMPT'], context['GITHUB_SHA'],
                      context['GITHUB_REPOSITORY_ID'], context['GITHUB_REPOSITORY_ID'],
                      context['GITHUB_REPOSITORY'], WORKFLOW), 'Fork TCP API run identity differs')
    require(run.get('event') == value['event'] and run.get('path') == WORKFLOW_PATH and
            run.get('head_branch') == value['ref'][len('refs/heads/'):] and
            run.get('head_repository', {}).get('full_name') == context['GITHUB_REPOSITORY'],
            'Fork TCP API event/workflow/branch/head repository differs')
    return value


def checkout():
    source = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    subprocess.run(['git', 'diff', '--exit-code', 'HEAD', '--'], cwd=ROOT, check=True)
    return source


def current(job=None):
    job = os.environ.get('GITHUB_JOB') if job is None else job
    source = checkout()
    value = envelope(os.environ, source, job)
    ctx = value['context']
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8-sig'))
    repository = get_json('repos/' + ctx['GITHUB_REPOSITORY'])
    run = get_json('repos/' + ctx['GITHUB_REPOSITORY'] + '/actions/runs/' + ctx['GITHUB_RUN_ID'])
    return validate(os.environ, event, repository, run, source, job)


def assert_current(value, job):
    require(value == envelope(os.environ, checkout(), job), 'Fork TCP identity changed during execution')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--job', choices=JOBS)
    args = parser.parse_args()
    print('FORK_TCP_DIAGNOSTIC_CONTEXT ' + json.dumps(current(args.job), sort_keys=True), flush=True)
