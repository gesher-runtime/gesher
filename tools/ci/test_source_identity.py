"""PR API metadata must use the head while artifacts retain their merge source."""
import copy
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
                         {'checkout_sha': 'a' * 40, 'api_head_sha': 'b' * 40})

    def test_push_and_dispatch_use_the_checkout(self):
        for kind in ('push', 'workflow_dispatch'):
            self.assertEqual(source_identity(dict(self.env, GITHUB_EVENT_NAME=kind)),
                             {'checkout_sha': 'a' * 40, 'api_head_sha': 'a' * 40})

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


if __name__ == '__main__':
    unittest.main()
