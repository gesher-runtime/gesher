# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 The Gesher Authors
"""Pure metadata controls; synthetic identities never confer runtime authority."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import fork_diagnostic_context as diagnostic


class ForkDiagnosticContextTest(unittest.TestCase):
    def setUp(self):
        self.sha = 'a' * 40
        self.repo = {'id': 1460486951, 'full_name': 'yom-chadash/gesher', 'fork': True,
                     'source': {'id': 1426153186, 'full_name': 'gesher-runtime/gesher'}}
        self.event = {'repository': copy.deepcopy(self.repo)}
        self.env = dict(GITHUB_ACTIONS='true', GITHUB_SHA=self.sha, GITHUB_WORKFLOW_SHA=self.sha,
                        GITHUB_REPOSITORY=self.repo['full_name'], GITHUB_REPOSITORY_ID=str(self.repo['id']),
                        GITHUB_RUN_ID='45049466398', GITHUB_RUN_ATTEMPT='1', GITHUB_JOB='native2',
                        GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_REF='refs/heads/diagnostic/test',
                        GITHUB_WORKFLOW='ci',
                        GITHUB_WORKFLOW_REF=self.repo['full_name'] + '/.github/workflows/ci.yml@refs/heads/diagnostic/test')
        self.run = dict(id=45049466398, run_attempt=1, head_sha=self.sha, event='workflow_dispatch',
                        repository=copy.deepcopy(self.repo), head_repository=copy.deepcopy(self.repo),
                        name='ci', path='.github/workflows/ci.yml', head_branch='diagnostic/test')

    def validate(self, **changes):
        args = dict(env=self.env, event=self.event, repository=self.repo, run=self.run, checkout=self.sha)
        args.update(changes)
        return diagnostic.validate(**args)

    def test_real_fork_identity_is_retained_and_never_canonical(self):
        value = self.validate(producer=True)
        self.assertEqual(value['repository'], 'yom-chadash/gesher')
        self.assertEqual(value['repository_id'], '1460486951')
        self.assertEqual(value['source'], self.sha)
        self.assertEqual(value['evidence_scope'], 'fork-diagnostic')
        self.assertIs(value['canonical_authority'], False)
        self.assertEqual(value['upstream_repository_id'], '1426153186')
        consumer = dict(self.env, GITHUB_JOB='test-shards')
        self.assertEqual(self.validate(env=consumer), value)
        with self.assertRaises(RuntimeError):
            self.validate(env=consumer, producer=True)

    def test_another_genuine_fork_is_dynamic_not_an_owner_allowlist(self):
        repository = copy.deepcopy(self.repo)
        repository.update(id=17, full_name='another-owner/another-name')
        event = {'repository': copy.deepcopy(repository)}
        env = dict(self.env, GITHUB_REPOSITORY=repository['full_name'], GITHUB_REPOSITORY_ID='17',
                   GITHUB_WORKFLOW_REF=repository['full_name'] + '/.github/workflows/ci.yml@' + self.env['GITHUB_REF'])
        run = dict(self.run, repository=repository, head_repository=repository)
        self.assertEqual(self.validate(env=env, event=event, repository=repository, run=run)['repository_id'], '17')

    def test_direct_native2_dispatch_uses_its_actual_workflow(self):
        env = dict(self.env, GITHUB_WORKFLOW='native2', GITHUB_WORKFLOW_REF=self.repo['full_name'] +
                   '/.github/workflows/native2.yml@' + self.env['GITHUB_REF'])
        run = dict(self.run, name='native2', path='.github/workflows/native2.yml')
        self.assertEqual(self.validate(env=env, run=run)['workflow'], 'native2')
        with self.assertRaises(RuntimeError):
            self.validate(env=dict(env, GITHUB_JOB='test-shards'), run=run)

    def test_missing_or_wrong_environment_fields_fail_closed(self):
        for key in self.env:
            for replacement in (None, '', 'wrong'):
                mutant = dict(self.env)
                if replacement is None:
                    del mutant[key]
                else:
                    mutant[key] = replacement
                with self.subTest(key=key, replacement=replacement), self.assertRaises((RuntimeError, KeyError)):
                    self.validate(env=mutant)

    def test_canonical_name_id_event_ref_and_unrelated_job_are_rejected(self):
        for key, value in [
            ('GITHUB_REPOSITORY', diagnostic.CANONICAL),
            ('GITHUB_REPOSITORY', diagnostic.CANONICAL.upper()),
            ('GITHUB_REPOSITORY_ID', diagnostic.CANONICAL_ID),
            ('GITHUB_REPOSITORY', '../gesher'),
            ('GITHUB_REPOSITORY', 'owner/repo/extra'),
            ('GITHUB_REPOSITORY_ID', '0'), ('GITHUB_RUN_ID', '-1'), ('GITHUB_RUN_ATTEMPT', '01'),
            ('GITHUB_EVENT_NAME', 'push'), ('GITHUB_EVENT_NAME', 'pull_request'),
            ('GITHUB_EVENT_NAME', 'pull_request_target'), ('GITHUB_EVENT_NAME', 'merge_group'),
            ('GITHUB_REF', 'refs/tags/v1.0.0'), ('GITHUB_REF', 'refs/heads/'),
            ('GITHUB_JOB', 'compile'), ('GITHUB_JOB', 'build'),
        ]:
            with self.subTest(key=key, value=value), self.assertRaises((RuntimeError, KeyError)):
                self.validate(env=dict(self.env, **{key: value}))
        with self.assertRaises(RuntimeError):
            self.validate(checkout='b' * 40)

    def test_event_and_api_repository_source_must_agree(self):
        for target in ('event', 'repository'):
            for key, value in [('id', 17), ('id', True), ('id', '1460486951'),
                               ('full_name', 'foreign/repo'), ('fork', False), ('fork', 1)]:
                repo = copy.deepcopy(self.repo)
                repo[key] = value
                changes = {'event': {'repository': repo}} if target == 'event' else {'repository': repo}
                with self.subTest(target=target, key=key, value=value), self.assertRaises(RuntimeError):
                    self.validate(**changes)
        for source in ({}, {'id': 17, 'full_name': diagnostic.CANONICAL},
                       {'id': 1426153186, 'full_name': 'foreign/upstream'},
                       {'id': '1426153186', 'full_name': diagnostic.CANONICAL}):
            repository = dict(self.repo, source=source)
            with self.subTest(source=source), self.assertRaises(RuntimeError):
                self.validate(repository=repository)

    def test_api_run_head_attempt_event_and_workflow_are_bound(self):
        for key, value in [('id', 1), ('run_attempt', 2), ('head_sha', 'b' * 40),
                           ('event', 'push'), ('head_branch', 'unrelated'), ('name', 'other'),
                           ('path', '.github/workflows/native2.yml')]:
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                self.validate(run=dict(self.run, **{key: value}))
        for member in ('repository', 'head_repository'):
            for key, value in [('id', 1426153186), ('full_name', diagnostic.CANONICAL)]:
                run = copy.deepcopy(self.run)
                run[member][key] = value
                with self.subTest(member=member, key=key), self.assertRaises(RuntimeError):
                    self.validate(run=run)

    def test_live_entry_reads_exact_repository_and_run_without_rewriting_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            event_path = Path(temporary) / 'event.json'
            event_path.write_text(json.dumps(self.event))
            env = dict(self.env, GITHUB_EVENT_PATH=str(event_path))
            with patch.dict(os.environ, env, clear=True), \
                    patch.object(diagnostic.subprocess, 'check_output', return_value=self.sha + '\n'), \
                    patch.object(diagnostic, 'get_json', side_effect=[self.repo, self.run]) as api:
                before = dict(os.environ)
                value = diagnostic.current_context(producer=True)
                self.assertEqual(diagnostic.assert_current(value), value)
                self.assertEqual(dict(os.environ), before)
            self.assertEqual([call.args[0] for call in api.call_args_list], [
                'repos/yom-chadash/gesher', 'repos/yom-chadash/gesher/actions/runs/45049466398'])

    def test_changed_authenticated_context_cannot_be_reused(self):
        value = self.validate()
        with patch.dict(os.environ, self.env, clear=True), \
                patch.object(diagnostic.subprocess, 'check_output', return_value=self.sha + '\n'):
            for key in value:
                bad = dict(value)
                bad[key] = 'different'
                with self.subTest(key=key), self.assertRaises(RuntimeError):
                    diagnostic.assert_current(bad)

    def test_invalid_endpoint_components_fail_before_api_access(self):
        for repo in ('../gesher', 'owner/..', './gesher', 'owner/.', 'owner/repo?extra', 'owner/repo/extra'):
            with patch.dict(os.environ, dict(self.env, GITHUB_REPOSITORY=repo), clear=True), \
                    patch.object(diagnostic.subprocess, 'check_output', return_value=self.sha + '\n'), \
                    patch.object(diagnostic, 'get_json') as api:
                with self.subTest(repo=repo), self.assertRaises(RuntimeError):
                    diagnostic.current_context()
                api.assert_not_called()


if __name__ == '__main__':
    unittest.main()
