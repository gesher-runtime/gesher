"""PR API metadata must use the head while artifacts retain their merge source."""
import copy
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from source_identity import source_identity


class SourceIdentityTest(unittest.TestCase):
    def setUp(self):
        self.env = dict(GITHUB_SHA='a' * 40, GITHUB_EVENT_NAME='pull_request',
                        GITHUB_REF='refs/pull/5/merge', GITHUB_BASE_REF='main',
                        GITHUB_HEAD_REF='codex/stability-apps-20261105',
                        GITHUB_REPOSITORY='gesher-runtime/gesher', GITHUB_REPOSITORY_ID='1426153186')
        repo = {'id': 1426153186, 'full_name': 'gesher-runtime/gesher'}
        self.event = {'number': 5, 'pull_request': {'merge_commit_sha': 'a' * 40,
            'base': {'sha': 'c' * 40, 'ref': 'main', 'repo': dict(repo)},
            'head': {'sha': 'b' * 40, 'ref': self.env['GITHUB_HEAD_REF'], 'repo': dict(repo)}}}
        self.parents = ('c' * 40, 'b' * 40)

    def test_merge_source_and_api_head_stay_distinct(self):
        self.assertEqual(source_identity(self.env, self.event, self.parents),
                         {'checkout_sha': 'a' * 40, 'api_head_sha': 'b' * 40,
                          'head_repository_id': self.env['GITHUB_REPOSITORY_ID']})

    def test_push_and_dispatch_use_the_checkout(self):
        for kind in ('push', 'workflow_dispatch'):
            self.assertEqual(source_identity(dict(self.env, GITHUB_EVENT_NAME=kind)),
                             {'checkout_sha': 'a' * 40, 'api_head_sha': 'a' * 40,
                              'head_repository_id': self.env['GITHUB_REPOSITORY_ID']})

    def test_merge_group_binds_event_checkout_ref_and_target(self):
        env = dict(self.env, GITHUB_EVENT_NAME='merge_group',
                   GITHUB_REF='refs/heads/gh-readonly-queue/main/pr-5-abc')
        event = {'action': 'checks_requested', 'merge_group': {
            'head_sha': 'a' * 40, 'head_ref': env['GITHUB_REF'],
            'base_sha': 'c' * 40, 'base_ref': 'refs/heads/main'}}
        self.assertEqual(source_identity(env, event), {
            'checkout_sha': 'a' * 40, 'api_head_sha': 'a' * 40,
            'head_repository_id': env['GITHUB_REPOSITORY_ID']})
        for field, value in [('head_sha', 'b' * 40), ('head_ref', 'refs/heads/main'),
                             ('base_sha', 'invalid'), ('base_ref', 'refs/heads/other')]:
            bad = copy.deepcopy(event)
            bad['merge_group'][field] = value
            with self.subTest(field=field), self.assertRaises(AssertionError):
                source_identity(env, bad)
        bad = copy.deepcopy(event); bad['action'] = 'destroyed'
        with self.assertRaises(AssertionError):
            source_identity(env, bad)

    def test_fork_keeps_target_merge_and_source_repository_distinct(self):
        event = copy.deepcopy(self.event)
        event['pull_request']['head']['repo'] = {'id': 1459959585, 'full_name': 'ztcebuck/gesher'}
        self.assertEqual(source_identity(self.env, event, self.parents),
                         {'checkout_sha': 'a' * 40, 'api_head_sha': 'b' * 40,
                          'head_repository_id': '1459959585'})
        for key, value in (('id', 0), ('id', True), ('id', '1459959585'),
                           ('full_name', '../gesher/extra'), ('full_name', ''),
                           ('full_name', self.env['GITHUB_REPOSITORY'])):
            bad = copy.deepcopy(event)
            bad['pull_request']['head']['repo'][key] = value
            with self.subTest(field=key, value=value), self.assertRaises(AssertionError):
                source_identity(self.env, bad, self.parents)
        for key in ('base', 'head'):
            bad = copy.deepcopy(event)
            bad['pull_request'][key]['sha'] = 'f' * 40
            with self.subTest(changed_parent=key), self.assertRaises(AssertionError):
                source_identity(self.env, bad, self.parents)
        bad = copy.deepcopy(event)
        bad['pull_request']['base']['repo'] = dict(event['pull_request']['head']['repo'])
        with self.assertRaises(AssertionError):
            source_identity(self.env, bad, self.parents)

    def test_wrong_or_missing_pr_identity_is_rejected(self):
        mutants = []
        for field, value in (('number', 6), ('number', True)):
            event = copy.deepcopy(self.event); event[field] = value; mutants.append(event)
        for field, value in (('head', None),):
            event = copy.deepcopy(self.event); event['pull_request'][field] = value; mutants.append(event)
        for key in ('base', 'head'):
            for field, value in (('id', 123), ('full_name', 'foreign/repo')):
                event = copy.deepcopy(self.event); event['pull_request'][key]['repo'][field] = value
                mutants.append(event)
            event = copy.deepcopy(self.event); event['pull_request'][key]['ref'] = 'wrong'; mutants.append(event)
        for key in ('base', 'head'):
            event = copy.deepcopy(self.event); event['pull_request'][key]['sha'] = 'bad'; mutants.append(event)
        for event in mutants:
            with self.subTest(event=event), self.assertRaises((AssertionError, KeyError, TypeError)):
                source_identity(self.env, event, self.parents)
        for key in self.env:
            env = dict(self.env); del env[key]
            with self.subTest(missing=key), self.assertRaises((AssertionError, KeyError)):
                source_identity(env, self.event, self.parents)

    def test_stale_payload_merge_sha_requires_the_actual_parent_pair(self):
        self.event['pull_request']['merge_commit_sha'] = 'd' * 40
        self.assertEqual(source_identity(self.env, self.event, self.parents)['checkout_sha'], 'a' * 40)
        for parents in ((), self.parents[:1], self.parents[::-1], ('d' * 40, 'b' * 40),
                        ('c' * 40, 'd' * 40), self.parents + ('d' * 40,)):
            with self.subTest(parents=parents), self.assertRaises(AssertionError):
                source_identity(self.env, self.event, parents)

    def test_raw_headers_preserve_shallow_checkout_parents(self):
        raw = 'tree ' + '0' * 40 + '\nparent ' + self.parents[0] + '\nparent ' + self.parents[1] + '\n\nmessage\n'
        with patch('source_identity.subprocess.check_output', side_effect=['a' * 40 + '\n', raw]) as git:
            self.assertEqual(source_identity(self.env, self.event)['api_head_sha'], 'b' * 40)
            self.assertEqual(git.call_args_list[1].args[0], ['git', 'cat-file', '-p', 'a' * 40])
        with patch('source_identity.subprocess.check_output', return_value='d' * 40 + '\n'):
            with self.assertRaises(AssertionError):
                source_identity(self.env, self.event)


class SourceIdentityGitHistoryTest(unittest.TestCase):
    """Use real commit ancestry, including a target that advances before checkout."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        previous = os.getcwd()
        self.addCleanup(os.chdir, previous)
        os.chdir(temp.name)
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'CI contract fixture')
        self.git('config', 'user.email', 'ci-fixture@example.invalid')
        self.tree = self.git('hash-object', '-t', 'tree', '--stdin', input='')
        self.base = self.commit('payload base')
        self.tested = self.commit('tested target', self.base)
        self.tip = self.commit('target advanced', self.tested)
        self.head = self.commit('fork head', self.base)
        self.side = self.commit('other branch', self.base)
        self.unrelated = self.commit('unrelated history')
        self.git('update-ref', 'refs/remotes/origin/main', self.tip)
        self.env = dict(GITHUB_EVENT_NAME='pull_request', GITHUB_REF='refs/pull/5/merge',
                        GITHUB_BASE_REF='main', GITHUB_HEAD_REF='contributor/topic',
                        GITHUB_REPOSITORY='gesher-runtime/gesher', GITHUB_REPOSITORY_ID='1426153186')
        self.event = {'number': 5, 'pull_request': {
            'base': {'sha': self.base, 'ref': 'main',
                     'repo': {'id': 1426153186, 'full_name': 'gesher-runtime/gesher'}},
            'head': {'sha': self.head, 'ref': self.env['GITHUB_HEAD_REF'],
                     'repo': {'id': 1459959585, 'full_name': 'ztcebuck/gesher'}}}}
        self.checkout(self.tested, self.head)

    def git(self, *args, input=None):
        return subprocess.check_output(['git', *args], input=input, text=True,
                                       stderr=subprocess.PIPE).strip()

    def commit(self, message, *parents):
        args = ['commit-tree', self.tree, '-m', message]
        for parent in parents:
            args += ['-p', parent]
        return self.git(*args)

    def checkout(self, *parents):
        self.env['GITHUB_SHA'] = self.commit('synthetic PR merge', *parents)
        self.git('checkout', '--quiet', '--detach', self.env['GITHUB_SHA'])

    def test_stale_payload_base_with_exact_or_advanced_target(self):
        for target in (self.tested, self.tip):
            self.git('update-ref', 'refs/remotes/origin/main', target)
            with self.subTest(target=target):
                self.assertEqual(source_identity(self.env, self.event),
                                 {'checkout_sha': self.env['GITHUB_SHA'], 'api_head_sha': self.head,
                                  'head_repository_id': '1459959585'})

    def test_tested_base_on_another_branch_is_rejected(self):
        self.checkout(self.side, self.head)
        with self.assertRaisesRegex(AssertionError, 'Unproved PR target ancestry'):
            source_identity(self.env, self.event)

    def test_payload_base_must_belong_to_tested_history(self):
        self.event['pull_request']['base']['sha'] = self.unrelated
        with self.assertRaisesRegex(AssertionError, 'Unproved PR target ancestry'):
            source_identity(self.env, self.event)

    def test_rewritten_target_is_rejected(self):
        self.git('update-ref', 'refs/remotes/origin/main', self.side)
        with self.assertRaisesRegex(AssertionError, 'Unproved PR target ancestry'):
            source_identity(self.env, self.event)

    def test_revision_expressions_are_not_target_branches(self):
        self.env['GITHUB_BASE_REF'] = 'main~1'
        self.event['pull_request']['base']['ref'] = 'main~1'
        with self.assertRaisesRegex(AssertionError, 'Cannot prove PR target history'):
            source_identity(self.env, self.event)

    def test_missing_target_and_shallow_history_fail_closed(self):
        self.git('update-ref', '-d', 'refs/remotes/origin/main')
        with self.assertRaisesRegex(AssertionError, 'requires fetch-depth: 0'):
            source_identity(self.env, self.event)
        self.git('update-ref', 'refs/remotes/origin/main', self.tip)
        Path('.git/shallow').write_text(self.tested + '\n')
        with self.assertRaisesRegex(AssertionError, 'Unproved PR target ancestry'):
            source_identity(self.env, self.event)

    def test_exact_head_and_two_parents_remain_required(self):
        for parents in ((self.tested, self.side), (self.tested,),
                        (self.tested, self.head, self.side)):
            self.checkout(*parents)
            with self.subTest(parents=parents), self.assertRaisesRegex(AssertionError, 'parent mismatch'):
                source_identity(self.env, self.event)


if __name__ == '__main__':
    unittest.main()
