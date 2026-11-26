"""One PR CI run covers every job and native auto-merge retains QA gating."""
import json
import os
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

    def test_ci_required_covers_all_jobs_and_fails_closed(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8'))
        # PyYAML's YAML 1.1 reader treats the unquoted key 'on' as True.
        self.assertIn('pull_request', doc[True])
        self.assertNotIn('merge_group', doc[True])
        jobs = doc['jobs']
        gate = jobs['ci-required']
        self.assertEqual(set(gate['needs']), set(jobs) - {'ci-required'})
        self.assertEqual(gate['if'], 'always()')
        body = gate['steps'][0]['run'].split("<<'PY'\n", 1)[1].rsplit('\nPY', 1)[0]
        for result in ['success', 'failure', 'cancelled', 'skipped']:
            env = dict(os.environ, JOB_RESULTS=json.dumps({'producer': {'result': result}}))
            run = subprocess.run([sys.executable, '-c', body], env=env, capture_output=True)
            self.assertEqual(run.returncode == 0, result == 'success')

    def test_arm64_required_verifies_source_and_builds_on_the_same_runner(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/arm64-windows.yml').read_text(encoding='utf-8'))
        self.assertIn('pull_request', doc[True])
        self.assertNotIn('merge_group', doc[True])
        self.assertEqual(set(doc['jobs']), {'arm64-required'})
        job = doc['jobs']['arm64-required']
        self.assertEqual(job['name'], 'arm64-required')
        self.assertEqual(job['runs-on'], 'ubuntu-24.04')
        self.assertNotIn('if', job)
        self.assertNotIn('continue-on-error', job)
        checkout, verify = job['steps'][:2]
        self.assertTrue(checkout['uses'].startswith('actions/checkout@'))
        self.assertEqual(checkout['with']['ref'], '${{ github.sha }}')
        build = next(step for step in job['steps'] if step.get('run') == 'bash tools/ci/build-arm64-windows.sh')
        for step in (checkout, verify, build):
            self.assertNotIn('if', step)
            self.assertNotIn('continue-on-error', step)
        actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        for source in (actual, '0' * 40):
            run = subprocess.run(['bash', '-eo', 'pipefail', '-c', verify['run']], cwd=ROOT,
                                 env=dict(os.environ, GITHUB_SHA=source), capture_output=True, text=True)
            self.assertEqual(run.returncode == 0, source == actual)
            self.assertEqual(run.stdout, f'SOURCE_IDENTITY source={actual}\n' if source == actual else '')

    def test_hosted_framework_runs_without_waiting_for_runner_authorization(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/framework.yml').read_text())
        route, host, framework = (doc['jobs'][name] for name in ('runner-route', 'host-tools', 'framework'))
        self.assertEqual(route['if'], 'inputs.local-runner')
        # Check the actual configured conditions for skipped optional routing,
        # routing failures and failed host tools. `local-runner` is a boolean.
        conditions = [job['if'].removeprefix('${{').removesuffix('}}') for job in (host, framework)]
        runners = [job['runs-on'].removeprefix('${{').removesuffix('}}') for job in (host, framework)]
        local_runner = {'group': 'gesher-build-trial', 'labels': ['self-hosted', 'linux', 'x64']}
        for local, route_result, host_result, expected_host, expected_framework in (
            (False, 'skipped', 'success', True, True),
            (True, 'failure', 'skipped', False, False),
            (True, 'success', 'failure', True, False),
            (True, 'success', 'success', True, True),
        ):
            with self.subTest(local=local, route=route_result, host=host_result):
                output = json.dumps(local_runner) if route_result == 'success' else ''
                needs = {'runner-route': {'result': route_result, 'outputs': {'runner': output}},
                         'host-tools': {'result': host_result}}
                js = 'const fromJSON = JSON.parse; const cancelled = () => false;'
                js += f'const local = {json.dumps(local)}, needs = {json.dumps(needs)};'
                jobs = ', '.join(f'({condition}) ? ({runner}) : null' for condition, runner in zip(conditions, runners))
                jobs = jobs.replace('inputs.local-runner', 'local').replace('needs.runner-route', "needs['runner-route']")
                jobs = jobs.replace('needs.host-tools', "needs['host-tools']")
                js += f'console.log(JSON.stringify([{jobs}]));'
                runner = local_runner if local else 'ubuntu-24.04'
                self.assertEqual(json.loads(subprocess.check_output(['node', '-e', js])),
                                 [runner if expected_host else None, runner if expected_framework else None])

    def test_cache_prune_schedules_runners_for_cache_writers(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/cache-prune.yml').read_text())
        self.assertNotIn('concurrency', doc)
        self.assertIn('schedule', doc[True])
        self.assertIn('workflow_dispatch', doc[True])
        prune = doc['jobs']['prune']
        self.assertFalse(prune['concurrency']['cancel-in-progress'])
        repo = 'gesher-runtime/gesher'
        cases = [
            ('workflow_run', 'ci', 'pull_request', 'feature', repo, 'true', False),
            ('workflow_run', 'ci', 'pull_request', 'main', repo, 'true', False),
            ('workflow_run', 'ci', 'push', 'main', repo, 'true', True),
            ('workflow_run', 'ci', 'workflow_dispatch', 'main', repo, 'true', True),
            ('workflow_run', 'ci', 'push', 'v1.0.0', repo, 'true', False),
            ('workflow_run', 'CodeQL Advanced', 'pull_request', 'feature', repo, 'true', True),
            ('workflow_run', 'CodeQL Advanced', 'pull_request', 'feature', repo, 'false', False),
            ('workflow_run', 'CodeQL Advanced', 'pull_request', 'feature', 'fork/gesher', 'true', False),
            ('schedule', None, None, None, None, 'false', True),
            ('workflow_dispatch', None, None, None, None, 'false', True),
        ]
        for trigger, workflow, event, branch, head_repo, enabled, expected in cases:
            with self.subTest(trigger=trigger, workflow=workflow, event=event, branch=branch, enabled=enabled):
                payload = {'workflow_run': {'name': workflow, 'event': event, 'head_branch': branch,
                                            'head_repository': {'full_name': head_repo}}} if workflow else {}
                github = dict(event_name=trigger, repository=repo, event=payload)
                vars_ = dict(GESHER_CODEQL_ADVANCED=enabled)
                js = f'const github = {json.dumps(github)}, vars = {json.dumps(vars_)}; console.log({prune["if"]});'
                self.assertEqual(subprocess.check_output(['node', '-e', js], text=True).strip(),
                                 json.dumps(expected))

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
        # The only schedule is recovery of labeled PRs stuck after finished CI.
        self.assertEqual(doc[True]['schedule'], [{'cron': '7,27,47 * * * *'}])
        self.assertIn('workflow_run', doc[True])
        self.assertNotIn('merge_group', doc[True])
        self.assertIn('unlabeled', doc[True]['pull_request_target']['types'])
        script = (ROOT / '.github/scripts/label-automerge.js').read_text()
        self.assertNotIn('pulls.merge(', script)
        self.assertIn('expectedHeadOid: $head', script)
        self.assertNotIn('enablePullRequestAutoMerge(', script)
        self.assertIn('mergePullRequest(input:', script)
        rules = json.loads((ROOT / '.github/automerge-ruleset.json').read_text())
        self.assertNotIn('merge_queue', [rule['type'] for rule in rules['rules']])

    def test_base_advance_keeps_review_but_gate_stays_exact_head(self):
        # GitHub's merge-base dismissal discarded exact-head approvals whenever
        # main advanced; the exact-head QA gate and controller cover pushes.
        rules = json.loads((ROOT / '.github/automerge-ruleset.json').read_text())
        review = next(r['parameters'] for r in rules['rules'] if r['type'] == 'pull_request')
        self.assertIs(review['dismiss_stale_reviews_on_push'], False)
        self.assertEqual(review['required_approving_review_count'], 1)
        checks = next(r['parameters']['required_status_checks'] for r in rules['rules']
                      if r['type'] == 'required_status_checks')
        self.assertIn({'context': 'qa/label-approval', 'integration_id': 15368}, checks)
        script = (ROOT / '.github/scripts/label-automerge.js').read_text()
        self.assertIn("payload.action === 'synchronize'", script)
        self.assertIn('p.dismissStaleApprovals(', script)
        self.assertIn("return {mode: 'blocked', state}", script)
        self.assertIn('deferRelease(', script)

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
                         "github.event.sender.id == 358279050 && github.event.label.name == 'automerge') && "
                         "!(github.event_name == 'pull_request_target' && github.event.action == 'edited' && "
                         "github.event.changes.base == null)")

    def test_app_publication_uses_one_queue_per_pr_across_event_identities(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        concurrency = doc['jobs']['automerge']['concurrency']
        self.assertEqual(concurrency['group'],
                         "merge-approval-${{ github.event.pull_request.number || "
                         "github.event.workflow_run.pull_requests[0].number || "
                         "github.event.client_payload.pull_request_number || inputs.pull_request_number || "
                         "github.event.workflow_run.head_sha || github.run_id }}")
        self.assertEqual(doc[True]['repository_dispatch'], {'types': ['gesher-automerge-reconcile']})
        # No cancellation can interrupt a run after it publishes success but
        # before the final authorization checks and native release mutation.
        self.assertEqual(concurrency['queue'], 'max')
        self.assertFalse(concurrency.get('cancel-in-progress', False))
        # Evaluate the configured OR expression with the event shapes GitHub
        # supplies. Read-only bulk/fork jobs may have fallback keys; their
        # dispatched worker must share the label/review/CI PR-number key.
        import copy
        import os
        base = {'github': {'run_id': 900, 'event': {'pull_request': {},
                'workflow_run': {'pull_requests': [{}]}, 'client_payload': {}}}, 'inputs': {}}
        cases = []
        for kind, number in [('pull', 7), ('ci', 7), ('queued', 7), ('manual', '7'),
                             ('queued', 8), ('bulk', None), ('fork', None)]:
            item = copy.deepcopy(base)
            event = item['github']['event']
            if kind == 'pull': event['pull_request']['number'] = number
            if kind == 'ci': event['workflow_run']['pull_requests'][0]['number'] = number
            if kind == 'queued': event['client_payload']['pull_request_number'] = number
            if kind == 'manual': item['inputs']['pull_request_number'] = number
            if kind == 'fork': event['workflow_run']['head_sha'] = 'a' * 40
            cases.append(item)
        expression = concurrency['group'].split('${{', 1)[1].rsplit('}}', 1)[0]
        script = 'const key = ({github, inputs}) => "merge-approval-" + (' + expression + ');\n'
        script += 'console.log(JSON.stringify(JSON.parse(process.env.QA_QUEUE_CASES).map(key)));'
        result = subprocess.run(['node', '-e', script], capture_output=True, text=True, check=True,
                                env=dict(os.environ, QA_QUEUE_CASES=json.dumps(cases)))
        self.assertEqual(json.loads(result.stdout), ['merge-approval-7'] * 4 +
                         ['merge-approval-8', 'merge-approval-900', 'merge-approval-' + 'a' * 40])

    def test_workflow_credential_is_separate_from_required_status_identity(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        checkout, repo, flag, token, reader, controller = doc['jobs']['automerge']['steps']
        self.assertEqual(checkout['with']['ref'], '${{ github.event.repository.default_branch }}')
        self.assertFalse(checkout['with']['persist-credentials'])
        self.assertEqual(token['if'], "vars.GESHER_BOT_APP_ID != ''")
        # Schedule payloads have no repository object; scope tokens via GITHUB_REPOSITORY.
        self.assertEqual(repo['id'], 'repo')
        self.assertEqual(repo['run'], 'echo "name=${GITHUB_REPOSITORY#*/}" >> "$GITHUB_OUTPUT"')
        self.assertEqual(token['with']['repositories'], '${{ steps.repo.outputs.name }}')
        self.assertEqual(token['with']['owner'], '${{ github.repository_owner }}')
        self.assertEqual({key: value for key, value in token['with'].items()
                          if key.startswith('permission-')},
                         {'permission-contents': 'write', 'permission-issues': 'write',
                          'permission-pull-requests': 'write',
                          'permission-workflows': 'write',
                          'permission-statuses': "${{ steps.app-qa-flag.outputs.enabled == 'true' && 'write' || '' }}"})
        self.assertNotIn('skip-token-revoke', token['with'])
        self.assertEqual(controller['env']['GESHER_BOT_TOKEN'], '${{ steps.gesher-bot.outputs.token }}')
        self.assertEqual(controller['env']['GESHER_BOT_SLUG'], '${{ steps.gesher-bot.outputs.app-slug }}')
        self.assertEqual(controller['env']['GESHER_BOT_APP_ID'], '${{ vars.GESHER_BOT_APP_ID }}')
        self.assertEqual(controller['env']['GESHER_BOT_INSTALLATION_ID'], '${{ steps.gesher-bot.outputs.installation-id }}')
        self.assertEqual(controller['env']['GESHER_APP_QA_STATUS'], '${{ steps.app-qa-flag.outputs.enabled }}')
        self.assertEqual(controller['env']['AUTOMERGE_WORKFLOWS_TOKEN'], '${{ secrets.AUTOMERGE_WORKFLOWS_TOKEN }}')
        # The main github client must remain GITHUB_TOKEN for app-bound statuses.
        self.assertNotIn('github-token', controller['with'])
        self.assertIn('new github.constructor', controller['with']['script'])
        self.assertIn('auth: token', controller['with']['script'])
        self.assertIn('process.env.GESHER_BOT_SLUG !== run.APP_SLUG', controller['with']['script'])
        self.assertIn('prGithub: appToken ? mergeGithub : undefined', controller['with']['script'])
        self.assertIn("appQaStatus: process.env.GESHER_APP_QA_STATUS === 'true'", controller['with']['script'])
        self.assertIn('appId: Number(process.env.GESHER_BOT_APP_ID)', controller['with']['script'])
        self.assertIn('appInstallationId: Number(process.env.GESHER_BOT_INSTALLATION_ID)', controller['with']['script'])

    def test_evidence_reads_use_a_read_only_app_token_with_actions_fallback(self):
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        checkout, repo, flag, token, reader, controller = doc['jobs']['automerge']['steps']
        self.assertEqual(reader['id'], 'gesher-read')
        self.assertEqual(reader['uses'], token['uses'])
        self.assertEqual(reader['if'], "vars.GESHER_BOT_APP_ID != ''")
        # A missing installation permission must not stop the controller.
        self.assertIs(reader['continue-on-error'], True)
        self.assertEqual({key: value for key, value in reader['with'].items()
                          if key.startswith('permission-')},
                         {'permission-actions': 'read', 'permission-checks': 'read',
                          'permission-metadata': 'read',
                          'permission-pull-requests': 'read',
                          'permission-statuses': "${{ steps.app-qa-flag.outputs.enabled == 'true' && 'read' || '' }}"})
        for key in ['app-id', 'private-key', 'owner', 'repositories']:
            self.assertEqual(reader['with'][key], token['with'][key])
        self.assertEqual(controller['env']['GESHER_READ_TOKEN'], '${{ steps.gesher-read.outputs.token }}')
        script = controller['with']['script']
        self.assertIn('auth: readToken', script)
        self.assertIn('run({github, readGithub,', script)
        self.assertIn('repositoryId: Number(process.env.GITHUB_REPOSITORY_ID)', script)

    def test_exact_flag_controls_both_token_permission_and_status_writer(self):
        import os
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        checkout, repo, flag, token, reader, controller = doc['jobs']['automerge']['steps']
        self.assertEqual(flag['id'], 'app-qa-flag')
        self.assertEqual(flag['uses'], 'actions/github-script@v7')
        self.assertEqual(flag['env'], {'GESHER_APP_QA_STATUS': '${{ vars.GESHER_APP_QA_STATUS }}'})
        self.assertEqual(token['with']['permission-statuses'],
                         "${{ steps.app-qa-flag.outputs.enabled == 'true' && 'write' || '' }}")
        self.assertEqual(controller['env']['GESHER_APP_QA_STATUS'], '${{ steps.app-qa-flag.outputs.enabled }}')
        # Execute the actual trusted flag script; Actions string equality is
        # case-insensitive and must see only this canonical boolean output.
        harness = "let result; const core = {setOutput: (name, value) => {result = {[name]: value};}};\n"
        harness += flag['with']['script'] + '\nconsole.log(JSON.stringify(result));'
        for value in [None, '', 'false', 'TRUE', 'True', 'true', ' true', 'true ', 'true\\n']:
            with self.subTest(value=value):
                env = dict(os.environ)
                env.pop('GESHER_APP_QA_STATUS', None)
                if value is not None:
                    env['GESHER_APP_QA_STATUS'] = value
                result = subprocess.run(['node', '-e', harness], env=env, text=True, capture_output=True, check=True)
                self.assertEqual(json.loads(result.stdout), {'enabled': value == 'true'})

    def test_shadow_status_does_not_change_required_sources_or_grant_bypasses(self):
        rules = json.loads((ROOT / '.github/automerge-ruleset.json').read_text())
        self.assertEqual(rules['enforcement'], 'active')
        self.assertEqual(rules['bypass_actors'], [])
        checks = next(r['parameters']['required_status_checks'] for r in rules['rules']
                      if r['type'] == 'required_status_checks')
        self.assertEqual(checks, [
            {'context': 'ci-required', 'integration_id': 15368},
            {'context': 'arm64-required', 'integration_id': 15368},
            {'context': 'qa/label-approval', 'integration_id': 15368},
        ])
        doc = yaml.safe_load((ROOT / '.github/workflows/automerge.yml').read_text())
        self.assertEqual(doc['permissions']['statuses'], 'write')
        # No workflow or repository default silently enables the new writer.
        self.assertNotIn('GESHER_APP_QA_STATUS', doc.get('env', {}))


if __name__ == '__main__':
    unittest.main()
