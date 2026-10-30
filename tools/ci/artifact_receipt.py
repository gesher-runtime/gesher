"""CI-only current-run supplier receipt and fail-closed archive verification."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
from receipt_identity import (exact_run, failed_installer_allowed, installer_uploaded,
                              identity_record, job_snapshot, run_snapshot)

p = argparse.ArgumentParser()
p.add_argument('--name', required=True)
p.add_argument('--producer', required=True)
p.add_argument('--producer-suffix', action='store_true')
p.add_argument('--destination', required=True)
p.add_argument('--allow-failed-producer', action='store_true')
a = p.parse_args()
if os.environ.get('GITHUB_ACTIONS') != 'true':
    raise RuntimeError('CI only')
if a.allow_failed_producer and not failed_installer_allowed(os.environ.get('GITHUB_WORKFLOW'), os.environ.get('GITHUB_JOB'), a.producer, a.name, a.producer_suffix):
    raise RuntimeError('Failed producer exception is restricted to canonical e2e installer')
repo, run_id, attempt = os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_RUN_ID'], os.environ['GITHUB_RUN_ATTEMPT']
repo_id, checkout = os.environ['GITHUB_REPOSITORY_ID'], os.environ['GITHUB_SHA']
if subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() != checkout:
    raise RuntimeError('Checkout mismatch')
event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
head, head_repo = checkout, repo_id
if os.environ['GITHUB_EVENT_NAME'] == 'pull_request':
    head = event['pull_request']['head']['sha']
    head_repo = str(event['pull_request']['head']['repo']['id'])

def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', f'repos/{repo}/{path}']))

run = api(f'actions/runs/{run_id}')
identity_record('run', {'run_id': run_id, 'attempt': attempt, 'source': head,
                'checkout': checkout, 'repository': repo, 'repository_id': repo_id,
                'head_repository_id': head_repo, 'workflow': os.environ['GITHUB_WORKFLOW']},
                run_snapshot(run), 'observed')
if not exact_run(run, run_id, attempt, head, repo_id, head_repo, repo, os.environ['GITHUB_WORKFLOW']):
    raise RuntimeError('Current run/source/attempt/repository mismatch')
jobs = api(f'actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100')
if jobs['total_count'] > 100:
    raise RuntimeError('Incomplete job inventory')
rows = [x for x in jobs['jobs'] if x['name'] == a.producer or (a.producer_suffix and x['name'].endswith(' / ' + a.producer))]
allowed = {'success', 'failure'} if a.allow_failed_producer else {'success'}
identity_record('producer', {'run_id': run_id, 'attempt': attempt, 'source': head,
                'checkout': checkout, 'producer': a.producer, 'suffix': a.producer_suffix,
                'allowed_conclusions': sorted(allowed), 'required_count': 1,
                'status_policy': 'observed only; original predicate unchanged'},
                {'total_count': jobs.get('total_count'), 'received_count': len(jobs['jobs']),
                 'selected': [job_snapshot(row) for row in rows],
                 'observed_names': [row.get('name') for row in jobs['jobs']]})
if len(rows) != 1:
    raise RuntimeError('Producer job missing or ambiguous')
job = rows[0]
if job['conclusion'] not in allowed or job['head_sha'] != head or str(job['run_id']) != run_id or str(job['run_attempt']) != attempt:
    raise RuntimeError('Producer source/attempt/conclusion mismatch')
if a.allow_failed_producer:
    if not installer_uploaded(job):
        raise RuntimeError('Installer upload did not complete successfully')
inventory = api(f'actions/runs/{run_id}/artifacts?per_page=100')
identity_record('artifact_inventory', {'name': a.name, 'run_id': run_id, 'source': head,
                'repository_id': repo_id, 'head_repository_id': head_repo,
                'total_count_max': 100}, {'total_count': inventory.get('total_count'),
                'received_count': len(inventory.get('artifacts', [])),
                'names': [item.get('name') for item in inventory.get('artifacts', [])]}, 'observed')
if inventory['total_count'] > 100:
    raise RuntimeError('Incomplete artifact inventory')
rows = [x for x in inventory['artifacts'] if x['name'] == a.name]
if len(rows) != 1:
    raise RuntimeError('Artifact missing or ambiguous')
item = rows[0]
identity_record('artifact', {'name': a.name, 'run_id': run_id, 'attempt': attempt, 'source': head,
                'repository_id': repo_id, 'head_repository_id': head_repo, 'expired': False,
                'digest_pattern': 'sha256:[0-9a-f]{64}', 'size_min': 1,
                'producer_id': job['id'], 'upload_not_before': job['started_at'],
                'upload_not_after': job['completed_at']},
                {**{key: item.get(key) for key in ('id', 'name', 'digest', 'expired', 'size_in_bytes', 'created_at')},
                 'workflow_run': {key: (item.get('workflow_run') or {}).get(key) for key in
                                  ('id', 'head_sha', 'repository_id', 'head_repository_id')}}, 'observed')
import re
if item['expired'] or item['size_in_bytes'] <= 0 or not re.fullmatch(r'sha256:[0-9a-f]{64}', item.get('digest') or '') or str(item['workflow_run']['id']) != run_id or item['workflow_run']['head_sha'] != head or str(item['workflow_run']['repository_id']) != repo_id or str(item['workflow_run']['head_repository_id']) != head_repo:
    raise RuntimeError('Artifact identity or digest mismatch')
def timestamp(s):
    return datetime.datetime.fromisoformat(s.replace('Z', '+00:00'))
if not timestamp(job['started_at']) <= timestamp(item['created_at']) <= timestamp(job['completed_at']):
    raise RuntimeError('Artifact outside producer attempt')
subprocess.run([sys.executable, 'tools/surface/download-verified-artifact.py', '--id', str(item['id']), '--digest', item['digest'], '--destination', a.destination], check=True)
print(f"CI_ARTIFACT_RECEIPT name={a.name} id={item['id']} digest={item['digest']} producer={job['name']} producer_id={job['id']} conclusion={job['conclusion']} source={head} checkout={checkout} run={run_id} attempt={attempt}", flush=True)
