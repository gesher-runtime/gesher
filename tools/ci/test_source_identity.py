"""PR API metadata must use the head while artifacts retain their merge source."""
import copy
import unittest

from source_identity import source_identity


class SourceIdentityTest(unittest.TestCase):
    def setUp(self):
        self.env = dict(GITHUB_SHA='a' * 40, GITHUB_EVENT_NAME='pull_request',
                        GITHUB_REF='refs/pull/5/merge', GITHUB_BASE_REF='main',
                        GITHUB_HEAD_REF='codex/stability-apps-20261105',
                        GITHUB_REPOSITORY='gesher-runtime/gesher', GITHUB_REPOSITORY_ID='1426153186')
        repo = {'id': 1426153186, 'full_name': 'gesher-runtime/gesher'}
        self.event = {'number': 5, 'pull_request': {'merge_commit_sha': 'a' * 40,
            'base': {'ref': 'main', 'repo': dict(repo)},
            'head': {'sha': 'b' * 40, 'ref': self.env['GITHUB_HEAD_REF'], 'repo': dict(repo)}}}

    def test_merge_source_and_api_head_stay_distinct(self):
        self.assertEqual(source_identity(self.env, self.event),
                         {'checkout_sha': 'a' * 40, 'api_head_sha': 'b' * 40})

    def test_push_and_dispatch_use_the_checkout(self):
        for kind in ('push', 'workflow_dispatch'):
            self.assertEqual(source_identity(dict(self.env, GITHUB_EVENT_NAME=kind)),
                             {'checkout_sha': 'a' * 40, 'api_head_sha': 'a' * 40})

    def test_wrong_or_missing_pr_identity_is_rejected(self):
        mutants = []
        for field, value in (('number', 6), ('number', True)):
            event = copy.deepcopy(self.event); event[field] = value; mutants.append(event)
        for field, value in (('merge_commit_sha', 'c' * 40), ('head', None)):
            event = copy.deepcopy(self.event); event['pull_request'][field] = value; mutants.append(event)
        for key in ('base', 'head'):
            for field, value in (('id', 123), ('full_name', 'foreign/repo')):
                event = copy.deepcopy(self.event); event['pull_request'][key]['repo'][field] = value
                mutants.append(event)
            event = copy.deepcopy(self.event); event['pull_request'][key]['ref'] = 'wrong'; mutants.append(event)
        event = copy.deepcopy(self.event); event['pull_request']['head']['sha'] = 'bad'; mutants.append(event)
        for event in mutants:
            with self.subTest(event=event), self.assertRaises((AssertionError, KeyError, TypeError)):
                source_identity(self.env, event)
        for key in self.env:
            env = dict(self.env); del env[key]
            with self.subTest(missing=key), self.assertRaises((AssertionError, KeyError)):
                source_identity(env, self.event)


if __name__ == '__main__':
    unittest.main()
