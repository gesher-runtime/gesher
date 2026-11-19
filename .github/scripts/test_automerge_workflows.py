"""One PR CI run covers every job and native auto-merge retains QA gating."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class QueueWorkflows(unittest.TestCase):
    def test_local_controller_pool_requires_main_and_an_explicit_flag(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        job = doc['jobs']['automerge']
        # Routing must not acquire a hosted admission-job dependency: avoiding
        # that hosted queue is the purpose of this dedicated control pool.
        self.assertNotIn('needs', job)
        expression = job['runs-on']
        prefix = "${{ fromJSON(vars.GESHER_AUTOMERGE_LOCAL == 'true' && github.workflow_ref == 'gesher-runtime/gesher/.github/workflows/automerge.yml@refs/heads/main' && '"
        self.assertTrue(expression.startswith(prefix))
        local, hosted = expression[len(prefix):].removesuffix(') }}').split("' || '")
        self.assertTrue(hosted.endswith("'"))
        self.assertEqual(json.loads(hosted[:-1]), 'ubuntu-24.04')
        self.assertEqual(json.loads(local), {'group': 'gesher-automerge',
                                          'labels': ['self-hosted', 'linux', 'x64']})
        self.assertIn('labeled', doc[True]['pull_request_target']['types'])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        self.assertEqual(set(doc[True]['workflow_run']['workflows']), {'ci', 'arm64-windows'})

    def test_both_gates_cover_all_jobs_and_fail_closed(self):
        for workflow, gate in [('ci', 'ci-required'), ('arm64-windows', 'arm64-required')]:
            doc = yaml.safe_load((ROOT / f'.github/workflows/{workflow}.yml').read_text(encoding='utf-8'))
            # PyYAML's YAML 1.1 reader treats the unquoted key 'on' as True.
            self.assertIn('pull_request', doc[True])
            self.assertNotIn('merge_group', doc[True])
            jobs = doc['jobs']
            self.assertEqual(set(jobs[gate]['needs']), set(jobs) - {gate})
            self.assertEqual(jobs[gate]['if'], 'always()')
            body = jobs[gate]['steps'][0]['run'].split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
            for result in ['success', 'failure', 'cancelled', 'skipped']:
                import os
                env = dict(os.environ, JOB_RESULTS=json.dumps({'producer': {'result': result}}))
                run = subprocess.run([sys.executable, '-c', body], env=env, capture_output=True)
                self.assertEqual(run.returncode == 0, result == 'success')

    def test_release_publish_uses_only_protected_workflow_code(self):
        ci = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8'))
        self.assertEqual(ci['permissions'], {'contents': 'read', 'actions': 'read'})
        self.assertFalse(any('write' in job.get('permissions', {}).values() for job in ci['jobs'].values()))
        identity = ci['jobs']['source-identity']['steps'][1]
        self.assertEqual(identity['name'], 'Validate pushed version tag')
        self.assertEqual(identity['if'], "${{ github.repository == 'gesher-runtime/gesher' && "
                         "github.event_name == 'push' && github.ref_type == 'tag' }}")
        publisher = yaml.safe_load((ROOT / '.github/workflows/publish-release.yml').read_text(encoding='utf-8'))
        self.assertEqual(publisher[True], {'workflow_run': {'workflows': ['ci'], 'types': ['completed']}})
        self.assertEqual(publisher['permissions'], {})
        job = publisher['jobs']['publish']
        self.assertEqual(set(publisher['jobs']), {'publish'})
        self.assertIn("publish-release.yml@refs/heads/main'", job['if'])
        self.assertIn("github.event.workflow_run.event == 'push'", job['if'])
        self.assertIn("github.event.workflow_run.conclusion == 'success'", job['if'])
        self.assertEqual(job['permissions'], {'actions': 'read', 'contents': 'write'})
        checkout, invocation = job['steps']
        self.assertRegex(checkout['uses'], r'^actions/checkout@[0-9a-f]{40}$')
        self.assertEqual(checkout['with']['ref'], '${{ github.workflow_sha }}')
        self.assertFalse(checkout['with']['persist-credentials'])
        self.assertEqual(invocation['run'], 'python3 tools/ci/publish_release.py')

    def test_no_controller_polling_or_direct_merging(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        self.assertNotIn('schedule', doc[True])
        self.assertIn('workflow_run', doc[True])
        self.assertNotIn('merge_group', doc[True])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        script = (ROOT / '.github/scripts/label-automerge.js').read_text()
        self.assertNotIn('pulls.merge(', script)
        self.assertIn('expectedHeadOid: $head', script)
        rules = json.loads((ROOT / '.github/automerge-ruleset.json').read_text())
        self.assertNotIn('merge_queue', [rule['type'] for rule in rules['rules']])

    def test_app_write_events_are_skipped_before_the_shared_job_queue(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        self.assertIn('dismissed', doc[True]['pull_request_review']['types'])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        self.assertNotIn('concurrency', doc)
        job = doc['jobs']['automerge']
        self.assertEqual(job['concurrency']['queue'], 'max')
        self.assertIn('github.event.pull_request.number', job['concurrency']['group'])
        self.assertFalse(job['concurrency'].get('cancel-in-progress', False))
        self.assertEqual(' '.join(job['if'].split()),
                         "(github.event_name != 'pull_request_review' || "
                         "(github.event.pull_request.head.repo.id == github.event.pull_request.base.repo.id && "
                         "!(github.event.sender.id == 358279050 && "
                         "(github.event.action == 'submitted' || github.event.action == 'dismissed')))) && "
                         "!(github.event_name == 'pull_request_target' && github.event.action == 'unlabeled' && "
                         "github.event.sender.id == 358279050 && github.event.label.name == 'automerge')")

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
        self.assertEqual(controller['env']['GESHER_BOT_TOKEN'], '${{ steps.gesher-bot.outputs.token }}')
        self.assertEqual(controller['env']['GESHER_BOT_SLUG'], '${{ steps.gesher-bot.outputs.app-slug }}')
        self.assertEqual(controller['env']['AUTOMERGE_WORKFLOWS_TOKEN'], '${{ secrets.AUTOMERGE_WORKFLOWS_TOKEN }}')
        # The main github client must remain GITHUB_TOKEN for app-bound statuses.
        self.assertNotIn('github-token', controller['with'])
        self.assertIn('new github.constructor', controller['with']['script'])
        self.assertIn('auth: token', controller['with']['script'])
        self.assertIn('process.env.GESHER_BOT_SLUG !== run.APP_SLUG', controller['with']['script'])
        self.assertIn('prGithub: appToken ? mergeGithub : undefined', controller['with']['script'])


if __name__ == '__main__':
    unittest.main()
