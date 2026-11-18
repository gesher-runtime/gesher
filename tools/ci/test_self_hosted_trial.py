"""Authorization failures must prevent PR code reaching the runner."""
import copy
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from self_hosted_trial import REPO, WORKFLOW, main, select_source


class TrialAuthorization(unittest.TestCase):
    def setUp(self):
        self.run = dict(event='pull_request', repository=dict(full_name=REPO),
                        path='.github/workflows/arm64-windows.yml', head_sha='1' * 40)
        self.pull = dict(state='open', base=dict(repo=dict(full_name=REPO), ref='main', sha='2' * 40),
                         head=dict(repo=dict(full_name=REPO), sha='1' * 40),
                         merge_commit_sha='3' * 40)
        self.commit = dict(sha='3' * 40, parents=[dict(sha='2' * 40), dict(sha='1' * 40)])

    def check(self, permission='write'):
        return select_source(self.run, self.pull, permission, self.commit)

    def test_internal_merge(self):
        self.assertEqual(self.check(), '3' * 40)

    def test_fork_rejected_even_with_writer_author(self):
        self.pull['head']['repo']['full_name'] = 'someone/gesher'
        with self.assertRaises(ValueError):
            self.check()

    def test_deleted_fork_rejected(self):
        self.pull['head']['repo'] = None
        with self.assertRaises(ValueError):
            self.check()

    def test_readonly_author_rejected(self):
        for permission in ('read', 'triage', 'none'):
            with self.subTest(permission=permission), self.assertRaises(ValueError):
                self.check(permission)

    def test_stale_head_rejected(self):
        self.run['head_sha'] = '4' * 40
        with self.assertRaises(ValueError):
            self.check()

    def test_wrong_event_workflow_repository_rejected(self):
        original = copy.deepcopy(self.run)
        for key, value in [('event', 'push'), ('path', '.github/workflows/other.yml'),
                           ('repository', dict(full_name='someone/gesher'))]:
            self.run = dict(original, **{key: value})
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check()

    def test_wrong_base_and_closed_pr_rejected(self):
        self.pull['base']['ref'] = 'other'
        with self.assertRaises(ValueError):
            self.check()
        self.pull['base']['ref'] = 'main'
        self.pull['state'] = 'closed'
        with self.assertRaises(ValueError):
            self.check()

    def test_merge_identity_and_parents_rejected(self):
        original = copy.deepcopy(self.commit)
        for bad in [dict(original, sha='4' * 40), dict(original, parents=[]),
                    dict(original, parents=list(reversed(original['parents'])))]:
            self.commit = bad
            with self.subTest(commit=bad), self.assertRaises(ValueError):
                self.check()

    def test_real_entrypoint_outputs_exact_merge(self):
        self.pull.update(number=42, user=dict(login='writer'))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'outputs'
            summary = Path(directory) / 'summary'
            env = dict(GITHUB_REPOSITORY=REPO, GITHUB_REF='refs/heads/main',
                       GITHUB_WORKFLOW_REF=f'{REPO}/{WORKFLOW}@refs/heads/main',
                       SOURCE_RUN_ID='123', GITHUB_OUTPUT=str(output),
                       GITHUB_STEP_SUMMARY=str(summary))
            responses = [self.run, [self.pull], self.pull,
                         dict(permission='write'), self.commit]
            with patch.dict(os.environ, env), patch('self_hosted_trial.get_json', side_effect=responses):
                main()
            self.assertEqual(output.read_text(), 'allowed=true\nsource=' + '3' * 40 + '\npr=42\n')
            self.assertIn('/actions/runs/123', summary.read_text())

    def test_real_entrypoint_never_schedules_fork(self):
        self.pull['head']['repo']['full_name'] = 'external/gesher'
        env = dict(GITHUB_REPOSITORY=REPO, GITHUB_REF='refs/heads/main',
                   GITHUB_WORKFLOW_REF=f'{REPO}/{WORKFLOW}@refs/heads/main', SOURCE_RUN_ID='123')
        with patch.dict(os.environ, env), patch('self_hosted_trial.get_json', side_effect=[self.run, [self.pull]]) as api:
            main()
        self.assertEqual(api.call_count, 2)

    def test_non_main_dispatch_rejected_before_api(self):
        env = dict(GITHUB_REPOSITORY=REPO, GITHUB_REF='refs/heads/untrusted',
                   GITHUB_WORKFLOW_REF=f'{REPO}/{WORKFLOW}@refs/heads/untrusted')
        with patch.dict(os.environ, env), patch('self_hosted_trial.get_json') as api:
            with self.assertRaises(ValueError):
                main()
        api.assert_not_called()


if __name__ == '__main__':
    unittest.main()
