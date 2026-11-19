"""One PR CI run covers every job and native auto-merge retains QA gating."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class QueueWorkflows(unittest.TestCase):
    def test_both_gates_cover_all_jobs_and_fail_closed(self):
        for workflow, gate in [('ci', 'ci-required'), ('arm64-windows', 'arm64-required')]:
            doc = yaml.safe_load((ROOT / f'.github/workflows/{workflow}.yml').read_text(encoding='utf-8'))
            # PyYAML's YAML 1.1 reader treats the unquoted key 'on' as True.
            self.assertIn('pull_request', doc[True])
            self.assertNotIn('merge_group', doc[True])
            jobs = doc['jobs']
            # Only the tag-only publisher runs after the gate; every other job is gated.
            downstream = {name for name, job in jobs.items()
                          if gate in ([job['needs']] if isinstance(job.get('needs'), str) else job.get('needs', []))}
            if workflow == 'ci':
                self.assertEqual(downstream, {'release'})
                self.assertEqual(jobs['release']['needs'], [gate])
                self.assertEqual(jobs['release']['if'],
                                 "${{ github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v') }}")
            else:
                self.assertEqual(downstream, set())
            self.assertEqual(set(jobs[gate]['needs']), set(jobs) - {gate} - downstream)
            self.assertEqual(jobs[gate]['if'], 'always()')
            body = jobs[gate]['steps'][0]['run'].split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
            for result in ['success', 'failure', 'cancelled', 'skipped']:
                import os
                env = dict(os.environ, JOB_RESULTS=json.dumps({'producer': {'result': result}}))
                run = subprocess.run([sys.executable, '-c', body], env=env, capture_output=True)
                self.assertEqual(run.returncode == 0, result == 'success')

    def test_no_controller_polling_or_direct_merging(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        self.assertNotIn('schedule', doc[True])
        self.assertIn('workflow_run', doc[True])
        self.assertNotIn('merge_group', doc[True])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        script = (ROOT / '.github/scripts/label-automerge.js').read_text()
        self.assertNotIn('pulls.merge(', script)
        self.assertIn('expectedHeadOid: $head', script)
        self.assertIn('pull_request.head.repo.id == github.event.pull_request.base.repo.id', doc['jobs']['automerge']['if'])
        rules = json.loads((ROOT / '.github/automerge-ruleset.json').read_text())
        self.assertNotIn('merge_queue', [rule['type'] for rule in rules['rules']])

    def test_workflow_credential_is_separate_from_required_status_identity(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        checkout, token, controller = doc['jobs']['automerge']['steps']
        self.assertEqual(checkout['with']['ref'], '${{ github.event.repository.default_branch }}')
        self.assertFalse(checkout['with']['persist-credentials'])
        self.assertEqual(token['if'], "vars.GESHER_BOT_APP_ID != ''")
        self.assertEqual(token['with']['repositories'], '${{ github.event.repository.name }}')
        self.assertEqual(token['with']['owner'], '${{ github.repository_owner }}')
        self.assertEqual({key: value for key, value in token['with'].items()
                          if key.startswith('permission-')},
                         {'permission-contents': 'write', 'permission-pull-requests': 'write',
                          'permission-workflows': 'write'})
        self.assertNotIn('skip-token-revoke', token['with'])
        self.assertEqual(controller['env']['AUTOMERGE_WORKFLOWS_TOKEN'],
                         '${{ steps.gesher-bot.outputs.token || secrets.AUTOMERGE_WORKFLOWS_TOKEN }}')
        # The main github client must remain GITHUB_TOKEN for app-bound statuses
        # and github-actions[bot] receipts. Only the native mutation client differs.
        self.assertNotIn('github-token', controller['with'])
        self.assertIn('new github.constructor', controller['with']['script'])
        self.assertIn('auth: token', controller['with']['script'])
        self.assertIn('run({github, context, core, mergeGithub})', controller['with']['script'])


if __name__ == '__main__':
    unittest.main()
