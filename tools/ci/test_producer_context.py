"""Metadata/transport controls; synthetic payloads are not runtime evidence."""
import copy
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

import producer_context as proof


class RetainedProducer(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.consumer = dict(source='1' * 40, supplier_head='2' * 40, repository='owner/repo',
                             repository_id='7', head_repository_id='8', run='9',
                             attempt='2', event='pull_request')
        self.original = dict(self.consumer, attempt='1')
        self.run = dict(id=9, run_attempt=1, head_sha='2' * 40, name='ci', event='pull_request',
                        repository=dict(id=7, full_name='owner/repo'), head_repository=dict(id=8))
        self.old_job = dict(id=10, name='build', run_id=9, run_attempt=1, head_sha='2' * 40,
                            status='completed', conclusion='success',
                            started_at='2026-11-20T12:01:13Z', completed_at='2026-11-20T12:07:27Z')
        self.new_job = dict(self.old_job, id=20, run_attempt=2)
        self.item = dict(id=30, name='windows-x64', digest='sha256:' + '3' * 64,
                         size_in_bytes=100, expired=False, created_at='2026-11-20T12:06:40Z',
                         workflow_run=dict(id=9, head_sha='2' * 40, repository_id=7, head_repository_id=8))
        self.old_jobs, self.new_jobs = [self.old_job], [self.new_job]
        self.items = [self.item]
        self.run_reads, self.downloads = [], []
        self.download_mutation = None
        self.archive_mutation = None
        self.role = 'production'
        self.write_role(self.role)
        env = patch.dict(os.environ, GITHUB_ACTIONS='true', GITHUB_EVENT_NAME='pull_request',
                         GITHUB_WORKFLOW='ci', GITHUB_RUN_ATTEMPT='2')
        env.start(); self.addCleanup(env.stop)
        for name, callback in (
                ('get_json', self.get_json), ('read_jobs', self.read_jobs),
                ('read_artifacts', lambda *a, **k: copy.deepcopy(self.items)),
                ('check_run', self.check_run), ('check_checkout', lambda context: None),
                ('download', self.download)):
            mocked = patch.object(proof.receipts, name, side_effect=callback)
            mocked.start(); self.addCleanup(mocked.stop)

    def write_role(self, role):
        member, artifact, producer, nested = proof.ROLES[role]
        if isinstance(producer, dict):
            producer = producer['ci']
        self.role = role
        self.item['name'] = artifact
        self.old_job['name'] = self.new_job['name'] = producer
        self.path = self.root / member
        self.path.parent.mkdir(parents=True, exist_ok=True)
        header = dict(self.original)
        payload = header if nested is None else {nested: header}
        payload.update(role=role, fixture_metadata_only=True)
        self.raw = (json.dumps(payload, sort_keys=True) + '\n').encode()
        self.path.write_bytes(self.raw)

    def get_json(self, endpoint):
        self.assertEqual(endpoint, f"repos/owner/repo/actions/runs/9/attempts/{self.original['attempt']}")
        return copy.deepcopy(self.run)

    def read_jobs(self, context):
        rows = self.old_jobs if context['attempt'] == self.original['attempt'] else self.new_jobs
        return dict(total_count=len(rows), jobs=copy.deepcopy(rows))

    def check_run(self, context):
        self.run_reads.append(copy.deepcopy(context))
        self.assertEqual(context['attempt'], self.consumer['attempt'])
        self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], self.consumer['attempt'])

    def download(self, item, destination):
        self.downloads.append(copy.deepcopy(item))
        self.assertEqual(item, self.item)
        destination.mkdir()
        archived = destination / proof.ROLES[self.role][0]
        archived.parent.mkdir(parents=True, exist_ok=True)
        archived.write_bytes(self.raw)
        if self.archive_mutation is not None:
            self.archive_mutation(archived)
        if self.download_mutation is not None:
            self.download_mutation()

    def verify(self):
        return proof.authenticate(self.path, self.consumer, self.role)

    def rejected(self):
        with self.assertRaises((RuntimeError, KeyError, ValueError)):
            self.verify()

    def test_original_execution_and_current_consumer_are_distinct(self):
        self.assertEqual(self.verify(), self.original)
        self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '2')
        self.assertEqual(self.consumer['attempt'], '2')
        self.assertEqual(len(self.downloads), 1)
        self.assertEqual(len(self.run_reads), 3)

    def test_all_current_role_archives_have_explicit_policy(self):
        for role in proof.ROLES:
            with self.subTest(role=role):
                self.write_role(role)
                self.assertEqual(self.verify(), self.original)

    def test_same_attempt_needs_no_historical_artifact_download(self):
        self.consumer['attempt'] = '1'
        self.assertEqual(self.verify(), self.original)
        self.assertEqual(self.downloads, [])
        self.assertEqual(self.run_reads, [])

    def test_new_consumer_roles_cannot_skip_archive_proof_by_claiming_current_attempt(self):
        self.consumer['attempt'] = '1'
        os.environ['GITHUB_RUN_ATTEMPT'] = '1'
        for role in proof.ARCHIVE_REQUIRED:
            with self.subTest(role=role):
                self.write_role(role)
                before = len(self.downloads)
                self.assertEqual(self.verify(), self.original)
                self.assertEqual(len(self.downloads), before + 1)

    def test_expected_supplier_must_be_the_exact_verified_batch_selection(self):
        supplier = copy.deepcopy((self.new_job, self.item))
        self.assertEqual(proof.authenticate(self.path, self.consumer, self.role,
                                           expected_supplier=supplier), self.original)
        for index, key, value in ((0, 'id', 101), (0, 'run_attempt', 1),
                                   (1, 'id', 31), (1, 'digest', 'sha256:' + '5' * 64)):
            with self.subTest(index=index, field=key):
                changed = copy.deepcopy(supplier)
                changed[index][key] = value
                before = len(self.downloads)
                with self.assertRaisesRegex(RuntimeError, 'verified batch supplier'):
                    proof.authenticate(self.path, self.consumer, self.role, expected_supplier=changed)
                self.assertEqual(len(self.downloads), before)

    def test_duplicate_json_keys_and_constants_fail_before_api(self):
        for raw in (self.raw.replace(b'"attempt": "1"', b'"attempt": "2", "attempt": "1"'),
                    self.raw.replace(b'"attempt": "1"', b'"attempt": NaN')):
            with self.subTest(raw=raw):
                self.path.write_bytes(raw)
                self.rejected()
        self.assertEqual(self.run_reads, [])

    def test_nested_context_rejects_extra_keys_on_both_attempt_paths(self):
        for role in (name for name, (_, _, _, nested) in proof.ROLES.items() if nested is not None):
            for attempt in ('1', '2'):
                with self.subTest(role=role, attempt=attempt):
                    self.write_role(role)
                    payload = json.loads(self.raw)
                    payload['context']['unexpected'] = 'not an execution identity field'
                    self.path.write_text(json.dumps(payload))
                    self.consumer['attempt'] = attempt
                    self.rejected()
        self.assertEqual(self.downloads, [])

    def test_every_nonattempt_identity_field_is_still_exact(self):
        for key in proof.CONTEXT_KEYS:
            if key == 'attempt':
                continue
            with self.subTest(key=key):
                original = self.consumer[key]
                self.consumer[key] = 'wrong'
                self.rejected()
                self.consumer[key] = original
        self.assertEqual(self.downloads, [])

    def test_original_attempt_is_not_arbitrary_or_future(self):
        for value in ('0', '-1', '03', '3', 1, None):
            with self.subTest(value=value):
                payload = json.loads(self.raw); payload['attempt'] = value
                self.path.write_text(json.dumps(payload))
                self.rejected()

    def test_unknown_and_mislabelled_role_fail(self):
        self.role = 'not-a-role'; self.rejected()
        self.role = 'query-hook'; self.rejected()

    def test_original_run_identity_and_event_are_required(self):
        for key, value in (('id', 8), ('run_attempt', 2), ('head_sha', '4' * 40),
                           ('name', 'other'), ('event', 'push'),
                           ('repository', dict(id=8, full_name='owner/repo')),
                           ('repository', dict(id=7, full_name='other/repo')),
                           ('head_repository', dict(id=9))):
            with self.subTest(key=key, value=value):
                saved = self.run[key]; self.run[key] = value
                self.rejected(); self.run[key] = saved

    def test_both_job_records_must_be_successful_exact_executions(self):
        for job in (self.old_job, self.new_job):
            for key, value in (('status', 'in_progress'), ('conclusion', 'failure'),
                               ('head_sha', '4' * 40), ('run_id', 8), ('run_attempt', 3),
                               ('name', 'other')):
                with self.subTest(attempt=job['run_attempt'], key=key):
                    saved = job[key]; job[key] = value
                    self.rejected(); job[key] = saved

    def test_missing_or_duplicate_producer_is_rejected(self):
        for name in ('old_jobs', 'new_jobs'):
            saved = getattr(self, name)
            for rows in ([], [*saved, dict(saved[0], id=100)]):
                with self.subTest(name=name, count=len(rows)):
                    setattr(self, name, rows); self.rejected()
            setattr(self, name, saved)

    def test_reexecuted_current_producer_cannot_consume_old_success(self):
        self.new_job.update(started_at='2026-11-20T13:01:13Z', completed_at='2026-11-20T13:07:27Z')
        self.rejected()

    def test_all_archive_identity_expiry_size_and_digest_guards_remain(self):
        for key, value in (('digest', None), ('digest', 'sha256:' + 'x' * 64),
                           ('expired', True), ('size_in_bytes', 0),
                           ('created_at', '2026-11-20T12:00:00Z'),
                           ('created_at', '2026-11-20T12:08:00Z'),
                           ('workflow_run', dict(id=10, head_sha='2' * 40, repository_id=7, head_repository_id=8)),
                           ('workflow_run', dict(id=9, head_sha='4' * 40, repository_id=7, head_repository_id=8)),
                           ('workflow_run', dict(id=9, head_sha='2' * 40, repository_id=8, head_repository_id=8)),
                           ('workflow_run', dict(id=9, head_sha='2' * 40, repository_id=7, head_repository_id=9))):
            with self.subTest(key=key, value=value):
                saved = self.item[key]; self.item[key] = value
                self.rejected(); self.item[key] = saved

    def test_later_same_name_output_and_duplicate_output_are_rejected(self):
        self.items.append(dict(self.item, id=31, created_at='2026-11-20T13:06:40Z'))
        self.rejected()
        self.items[-1]['created_at'] = self.item['created_at']; self.rejected()

    def test_actual_digest_checked_archive_role_bytes_must_match(self):
        self.archive_mutation = lambda path: path.write_bytes(b'forged role')
        self.rejected()
        self.archive_mutation = lambda path: path.unlink()
        self.rejected()

    def test_original_or_reused_record_cannot_change_during_download(self):
        for job in (self.old_job, self.new_job):
            with self.subTest(attempt=job['run_attempt']):
                saved = job['id']
                self.download_mutation = lambda: job.update(id=101)
                self.rejected(); job['id'] = saved

    def test_artifact_digest_cannot_change_during_download(self):
        self.download_mutation = lambda: self.item.update(digest='sha256:' + '4' * 64)
        self.rejected()

    def test_new_current_attempt_during_read_is_rejected(self):
        with patch.object(proof.receipts, 'check_run', side_effect=RuntimeError('attempt advanced')):
            self.rejected()

    def test_local_role_cannot_change_after_its_archive_was_verified(self):
        self.download_mutation = lambda: self.path.write_bytes(b'changed')
        self.rejected()


class CurrentContext(unittest.TestCase):
    def test_pr_merge_checkout_and_head_remain_distinct_without_changing_consumer_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            event = Path(temporary) / 'event.json'
            event.write_text(json.dumps({'pull_request': {'head': {'sha': 'b' * 40, 'repo': {'id': 8}}}}))
            env = dict(GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40, GITHUB_RUN_ID='9', GITHUB_RUN_ATTEMPT='3',
                       GITHUB_REPOSITORY='owner/repo', GITHUB_REPOSITORY_ID='7', GITHUB_WORKFLOW='ci',
                       GITHUB_EVENT_NAME='pull_request', GITHUB_EVENT_PATH=str(event))
            with patch.dict(os.environ, env, clear=True), \
                    patch.object(proof.receipts.subprocess, 'check_output', return_value='a' * 40 + '\n'):
                self.assertEqual(proof.current_context(), dict(source='a' * 40, supplier_head='b' * 40,
                    repository='owner/repo', repository_id='7', head_repository_id='8', run='9',
                    attempt='3', event='pull_request'))
                self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '3')
                os.environ['GITHUB_SHA'] = 'c' * 40
                with self.assertRaisesRegex(RuntimeError, 'Checkout mismatch'):
                    proof.current_context()


class NativeRoleBranches(unittest.TestCase):
    """Run the actual producer/consumer branches with declared TEXT build inputs."""
    def setUp(self):
        root = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location('native_role_branch_under_test',
            root / 'tools/native3-locks/union_payload.py')
        self.native = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.native)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.host = self.root / 'host'; self.host.mkdir()
        self.reference = self.root / 'reference'; self.reference.mkdir()
        (self.root / 'dist').mkdir(); (self.root / 'out').mkdir()
        self.info = dict(source='1' * 40, supplier_head='2' * 40, repository='owner/repo',
                         repository_id='7', head_repository_id='8', run='9',
                         attempt='1', event='pull_request')
        self.compiler = dict(configured_driver='/SYNTHETIC/cxx', launcher=None,
                             sha256='3' * 64, metadata_sha256='4' * 64)
        runtime = b'SYNTHETIC_TEXT_NOT_PE production'
        (self.root / 'dist/dalvikvm.exe').write_bytes(runtime)
        self.production = dict(self.info, role='production', test_hook_compiled=False,
                               compiler=self.compiler, executable_sha256=hashlib.sha256(runtime).hexdigest(),
                               shared_sources={})
        (self.root / 'dist/web3-production-role.json').write_text(json.dumps(self.production))
        for name in self.native.TARGETS:
            (self.host / (name + '.exe')).write_text('SYNTHETIC_TEXT_NOT_PE ' + name)
            (self.host / (name + '.imports.txt')).write_text('Name: KERNEL32.dll\n')
        (self.reference / 'linux-reference.json').write_text('{"metadata_fixture":true}\n')
        commands = {('SYNTHETIC', str(self.root / 'tests/native3-locks' / name), name):
                    ['/SYNTHETIC/cxx', '-Werror', name]
                    for name in ('command_worker.cc', 'duplicate_windows_test.cc')}
        for obj, name, value in (
                (self.native, 'ROOT', self.root),
                (self.native, 'source_inventory', lambda: {}),
                (self.native.host_roles, 'context', lambda: dict(self.info)),
                (self.native.host_roles, 'compiler', lambda _: self.compiler),
                (self.native.host_roles, 'cache', lambda _: {'GESHER_WEB3_QUERY_TESTS': 'OFF'}),
                (self.native.host_roles, 'commands', lambda _: commands),
                (self.native.host_roles, 'sources', lambda: {})):
            mocked = patch.object(obj, name, value); mocked.start(); self.addCleanup(mocked.stop)

    def main(self, mode):
        with patch.object(sys, 'argv', ['union_payload.py', mode, '--host', str(self.host),
                          '--reference', str(self.reference), '--destination', str(self.root / 'merged')]), \
             redirect_stdout(io.StringIO()):
            self.native.main()

    def test_fresh_producer_seals_its_actual_current_context(self):
        self.main('host')
        manifest = json.loads((self.host / 'provenance.json').read_text())
        self.assertEqual(manifest['context'], self.info)
        self.assertEqual(manifest['production_role'], self.production)
        self.assertEqual(set(manifest['outputs']), self.native.HOST_FILES)

    def test_native_manifest_rejects_extra_context_keys(self):
        self.main('host')
        path = self.host / 'provenance.json'
        manifest = json.loads(path.read_text())
        manifest['context']['unexpected'] = 'not allowed'
        path.write_text(json.dumps(manifest))
        with self.assertRaises(AssertionError):
            self.native.check_manifest(self.host, 'native3-common-host-fixture',
                                       self.native.HOST_FILES, self.info, {})

    def test_consumer_compares_embedded_production_to_authenticated_host(self):
        self.main('host')
        host = json.loads((self.host / 'provenance.json').read_text())
        reference = dict(compiler=dict(sha256='5' * 64, version='SYNTHETIC'), command=['SYNTHETIC'],
                         outputs={'linux-reference.json': self.native.host_roles.sha(self.reference / 'linux-reference.json')})
        self.info['attempt'] = '2'
        # Authentication itself is covered above with real selector predicates
        # and separately with the captured real ZIP. This boundary supplies its
        # accepted old producer to the actual remaining consumer/assembly body.
        with patch.object(self.native, 'check_manifest', side_effect=[host, reference]):
            self.main('verify')
        self.assertEqual((self.root / 'merged/native3_lock_command_worker.exe').read_bytes(),
                         (self.host / 'native3_lock_command_worker.exe').read_bytes())

    def test_consumer_rejects_production_from_a_different_execution(self):
        self.main('host')
        host = json.loads((self.host / 'provenance.json').read_text())
        host['production_role']['attempt'] = '2'
        self.info['attempt'] = '2'
        with patch.object(self.native, 'check_manifest', side_effect=[host, {}]):
            with self.assertRaises(AssertionError):
                self.main('verify')


if __name__ == '__main__':
    unittest.main()
