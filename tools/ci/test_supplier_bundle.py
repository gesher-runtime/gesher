"""A core supplier batch received once and consumed by many jobs as one bundle.

The producer side is the real batch path with its ledger. The bundle archive is
the published tree, zipped as upload-artifact would. The consumer side is the
real single-artifact receipt, archive digest/member checks, ledger and per-file
checks and filesystem publish. Only GitHub transport and storage are adapted
(by the ArtifactBatch harness), and it counts every API request.
"""
import copy
import hashlib
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit
import zipfile

import artifact_receipt as receipt
import test_artifact_batch as batch_tests

ROOT = Path(__file__).resolve().parents[2]
BUNDLE_ID = 5000


class SupplierBundle(unittest.TestCase):
    def setUp(self):
        self.h = batch_tests.ArtifactBatch('test_windows_crlf_checkout_keeps_the_same_committed_manifest')
        self.h.setUp()
        self.addCleanup(self.h.doCleanups)
        self.produced = self.h.destination
        # The producer receives the batch exactly as a batch job does.
        receipt.main(['--batch', str(self.h.manifest), '--destination', str(self.produced), '--ledger'])
        self.producer_reads = len(self.h.json_reads) + len(self.h.redirects)
        self.ledger = json.loads((self.produced / receipt.LEDGER).read_text(encoding='utf-8'))
        self.h.jobs.append(dict(id=900, name='core-suppliers', status='completed', conclusion='success',
            head_sha=self.h.head, run_id=7, run_attempt=2, started_at='2026-11-19T12:02:00Z',
            completed_at='2026-11-19T12:03:00Z'))
        self.publish(self.bundle_entries())
        # Each consumer has its own new destination.
        self.h.destination = self.h.root / 'consumer' / 'core'
        self.consumed = self.h.destination
        self.reads_before = len(self.h.json_reads), len(self.h.redirects)

    def bundle_entries(self, ledger=None):
        entries = []
        for path in sorted(self.produced.rglob('*')):
            if path.is_file() and path.name != receipt.LEDGER:
                entries.append((path.relative_to(self.produced).as_posix(), path.read_bytes()))
        raw = ledger if isinstance(ledger, bytes) else json.dumps(ledger or self.ledger).encode()
        return entries + [(receipt.LEDGER, raw)]

    def publish(self, entries):
        data = batch_tests.zip_bytes(entries)
        self.h.archives[BUNDLE_ID] = data
        self.h.items = [item for item in self.h.items if item['id'] != BUNDLE_ID]
        self.h.items.append(dict(id=BUNDLE_ID, name='core-suppliers', expired=False,
            digest='sha256:' + hashlib.sha256(data).hexdigest(), size_in_bytes=len(data),
            created_at='2026-11-19T12:02:30Z', workflow_run={'id': 7, 'head_sha': self.h.head,
                'repository_id': 8, 'head_repository_id': 9}))

    def consume(self, *extra):
        receipt.main(['--name', 'core-suppliers', '--producer', 'core-suppliers',
                      '--bundle-of', str(self.h.manifest), '--destination', str(self.consumed), *extra])

    def reads(self):
        return (self.h.json_reads[self.reads_before[0]:], self.h.redirects[self.reads_before[1]:])

    def rejected(self, message, *, downloaded=True):
        logs = self.h.logs.getvalue()
        with self.assertRaisesRegex(Exception, message):
            self.consume()
        self.assertFalse(self.consumed.exists())
        self.assertFalse(list(self.consumed.parent.glob('core-stage-*')) if self.consumed.parent.exists() else [])
        self.assertEqual(len(self.reads()[1]), 1 if downloaded else 0)
        self.assertNotIn('CI_ARTIFACT_BUNDLE ', self.h.logs.getvalue()[len(logs):])
        self.reads_before = len(self.h.json_reads), len(self.h.redirects)

    def test_one_batch_serves_every_consumer_with_seven_requests_each(self):
        self.assertEqual(self.producer_reads, 20)
        self.assertEqual(len(self.ledger['files']), len(self.h.specs))
        self.consume()
        json_reads, redirects = self.reads()
        # Run, 100-row job page and named artifact page, the bundle archive,
        # then the job, artifact and run revalidation after the download.
        self.assertEqual(len(json_reads), 6)
        self.assertEqual(redirects, ['https://api.github.com/repos/gesher-runtime/gesher/actions/artifacts/5000/zip'])
        self.assertTrue(all('name=core-suppliers' in path for path in json_reads if '/artifacts?' in path))
        produced = receipt.tree_digests(self.produced / 'artifacts')
        self.assertEqual(receipt.tree_digests(self.consumed / 'artifacts'), produced)
        self.assertEqual({entry.name for entry in self.consumed.iterdir()}, {'artifacts'})
        for spec in self.h.specs:
            path = self.consumed / 'artifacts' / spec['destination'] / 'actual' / (spec['destination'] + '.txt')
            self.assertEqual(path.read_text(), spec['name'])
        logs = self.h.logs.getvalue()
        self.assertEqual(logs.count('CI_ARTIFACT_BUNDLE_MEMBER '), len(self.h.specs))
        self.assertIn('CI_ARTIFACT_RECEIPT name=core-suppliers id=5000 ', logs)
        self.assertIn(f'CI_ARTIFACT_BUNDLE version=1 count={len(self.h.specs)} ', logs)
        # A shard batch costs as much as the producer batch: the bundle saves
        # 20 - 7 requests in every shard after the first.
        self.assertEqual(len(json_reads) + len(redirects), 7)

    def test_ledger_records_every_verified_supplier_and_byte(self):
        self.assertEqual([entry['name'] for entry in self.ledger['suppliers']],
                         [spec['name'] for spec in self.h.specs])
        for entry, item in zip(self.ledger['suppliers'], self.h.items):
            self.assertEqual(entry['artifact']['id'], item['id'])
            self.assertEqual(entry['artifact']['digest'], item['digest'])
            self.assertEqual(entry['producer_job']['name'], entry['producer'])
        self.assertEqual((self.ledger['run_id'], self.ledger['attempt'], self.ledger['source'], self.ledger['checkout']),
                         ('7', '2', self.h.head, self.h.checkout))
        self.assertIn('CI_ARTIFACT_LEDGER version=1 count=14 files=14 ', self.h.logs.getvalue())

    def test_bundle_producer_must_be_the_exact_successful_job(self):
        bundle_job = self.h.jobs[-1]
        for key, value in (('conclusion', 'failure'), ('run_attempt', 1), ('head_sha', 'c' * 40),
                           ('status', 'in_progress')):
            with self.subTest(key=key):
                original = bundle_job[key]
                bundle_job[key] = value
                self.rejected('Producer source/attempt/conclusion', downloaded=False)
                bundle_job[key] = original

    def test_bundle_artifact_outside_its_producer_window_is_rejected(self):
        self.h.items[-1]['created_at'] = '2026-11-19T12:04:00Z'
        self.rejected('after the exact producer completed', downloaded=False)

    def test_tampered_bytes_extra_or_missing_members_are_rejected(self):
        entries = self.bundle_entries()
        changed = [(name, b'tampered' if index == 0 else value) for index, (name, value) in enumerate(entries)]
        cases = {'Supplier bundle files differ': changed,
                 'Supplier bundle files differ ': entries + [('artifacts/framework/extra.txt', b'unledgered')],
                 'Supplier bundle members differ': [entry for entry in entries if not entry[0].startswith('artifacts/arm64/')],
                 'Supplier bundle layout differs': entries + [('unexpected.txt', b'x')],
                 'Supplier bundle layout differs ': [entry for entry in entries if entry[0] != receipt.LEDGER]}
        for message, value in cases.items():
            with self.subTest(message=message):
                self.publish(value)
                self.rejected(message.strip())

    def test_ledger_bound_to_another_run_source_manifest_or_later_attempt_is_rejected(self):
        mutations = [('run_id', '8', 'another manifest, run or source'),
                     ('source', 'c' * 40, 'another manifest, run or source'),
                     ('checkout', 'c' * 40, 'another manifest, run or source'),
                     ('manifest_sha256', 'f' * 64, 'another manifest, run or source'),
                     ('manifest', 'tools/ci/web3_query_artifacts.json', 'another manifest, run or source'),
                     ('head_repository_id', '10', 'another manifest, run or source'),
                     ('attempt', '3', 'attempt is invalid'), ('attempt', 2, 'attempt is invalid'),
                     ('version', 2, 'invalid fields'), ('version', True, 'invalid fields'),
                     ('version', 1.0, 'invalid fields')]
        for key, value, message in mutations:
            with self.subTest(key=key, value=value):
                ledger = copy.deepcopy(self.ledger)
                ledger[key] = value
                self.publish(self.bundle_entries(ledger))
                self.rejected(message)

    def test_ledger_suppliers_must_match_the_committed_manifest(self):
        def failed(ledger): ledger['suppliers'][0]['producer_job']['conclusion'] = 'failure'
        def renamed(ledger): ledger['suppliers'][0]['producer'] = 'another-job'
        def reordered(ledger): ledger['suppliers'].reverse()
        def dropped(ledger): ledger['suppliers'].pop()
        def digest(ledger): ledger['suppliers'][0]['artifact']['digest'] = 'sha256:short'
        def unknown(ledger): ledger['suppliers'][0]['extra'] = True
        def files(ledger): ledger['files']['artifacts/extra.txt'] = 'f' * 64
        for mutate, message in ((failed, 'unverified supplier'), (renamed, 'does not match the manifest'),
                                (reordered, 'does not match the manifest'), (dropped, 'does not match the manifest'),
                                (digest, 'unverified supplier'), (unknown, 'does not match the manifest'),
                                (files, 'files differ')):
            with self.subTest(mutation=mutate.__name__):
                ledger = copy.deepcopy(self.ledger)
                mutate(ledger)
                self.publish(self.bundle_entries(ledger))
                self.rejected(message)

    def test_duplicate_ledger_keys_are_rejected(self):
        raw = json.dumps(self.ledger).encode()
        self.publish(self.bundle_entries(b'{"run_id": "8", ' + raw[1:]))
        self.rejected('ledger is not strict JSON: Duplicate')

    def test_partial_rerun_accepts_a_bundle_from_an_earlier_attempt_of_this_run(self):
        ledger = dict(self.ledger, attempt='1')
        self.publish(self.bundle_entries(ledger))
        self.consume()
        self.assertIn('attempt=1 bundle=5000', self.h.logs.getvalue())
        self.assertTrue((self.consumed / 'artifacts').is_dir())

    def test_run_attempt_or_bundle_change_after_download_discards_staging(self):
        original = self.h.api_json
        def changed(field):
            def read(endpoint):
                value = original(endpoint)
                if len(self.h.redirects) > self.reads_before[1]:
                    path = urlsplit(endpoint).path
                    if field == 'run' and path.endswith('/actions/runs/7'):
                        value['run_attempt'] = 3
                    if field == 'artifact' and path.endswith('/artifacts'):
                        for item in value['artifacts']:
                            item['size_in_bytes'] += 1
                return value
            return read
        for field, message in (('run', 'Current run/source/attempt/repository'),
                               ('artifact', 'bundle identity changed')):
            with self.subTest(field=field), patch.object(self.h, 'api_json', side_effect=changed(field)):
                self.rejected(message)

    def test_existing_destination_is_never_overwritten(self):
        self.consumed.mkdir(parents=True)
        (self.consumed / 'foreign.txt').write_text('preserve')
        with self.assertRaisesRegex(RuntimeError, 'already exists'):
            self.consume()
        self.assertEqual((self.consumed / 'foreign.txt').read_text(), 'preserve')
        self.assertEqual(self.reads(), ([], []))

    def test_modified_manifest_fails_exact_checkout_binding_before_api(self):
        changed = copy.deepcopy(self.h.specs)
        changed[0]['producer'] = 'another-successful-job'
        self.h.manifest.write_text(json.dumps(changed))
        with self.assertRaisesRegex(RuntimeError, 'differs from the exact checkout'):
            self.consume()
        self.assertEqual(self.reads(), ([], []))

    def test_bundle_options_cannot_relax_producer_or_mix_modes(self):
        for argv in (['--name', 'core-suppliers', '--producer', 'core-suppliers', '--bundle-of', 'm.json',
                      '--destination', 'x', '--producer-suffix'],
                     ['--name', 'core-suppliers', '--producer', 'core-suppliers', '--bundle-of', 'm.json',
                      '--destination', 'x', '--allow-failed-producer'],
                     ['--batch', 'm.json', '--bundle-of', 'm.json', '--destination', 'x'],
                     ['--name', 'core-suppliers', '--producer', 'core-suppliers', '--destination', 'x', '--ledger']):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                receipt.main(argv)
        self.assertEqual(self.reads(), ([], []))


class SupplierBundleWiring(unittest.TestCase):
    MANIFEST = 'tools/ci/test_shard_artifacts.json'

    def setUp(self):
        import yaml
        self.jobs = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8'))['jobs']

    def runs(self, job):
        return '\n'.join(str(step.get('run', '')) for step in self.jobs[job]['steps'])

    def test_core_suppliers_receives_the_batch_once_and_uploads_the_published_tree(self):
        job = self.jobs['core-suppliers']
        specs = receipt.batch_manifest((ROOT / self.MANIFEST).read_text(encoding='utf-8'))
        self.assertEqual(set(job['needs']), {spec['producer'].split(' / ', 1)[0] for spec in specs})
        self.assertIn(f'artifact_receipt.py --batch {self.MANIFEST} --destination ci-suppliers/core --ledger',
                      self.runs('core-suppliers'))
        uploads = [step for step in job['steps'] if str(step.get('uses', '')).startswith('actions/upload-artifact@')]
        self.assertEqual(len(uploads), 1)
        self.assertEqual(uploads[0]['with']['name'], 'core-suppliers')
        self.assertEqual(uploads[0]['with']['path'], 'ci-suppliers/core/')
        self.assertEqual(uploads[0]['with']['if-no-files-found'], 'error')
        self.assertIs(uploads[0]['with']['include-hidden-files'], True)
        # Shorter than the producers' retention would break reruns of failed shards.
        self.assertNotIn('retention-days', uploads[0]['with'])
        self.assertEqual(job['env']['GH_TOKEN'], '${{ github.token }}')

    def test_every_test_shard_receipts_the_bundle_instead_of_the_batch(self):
        job = self.jobs['test-shards']
        runs = self.runs('test-shards')
        self.assertIn('core-suppliers', job['needs'])
        self.assertIn('artifact_receipt.py --name core-suppliers --producer core-suppliers '
                      f'--bundle-of {self.MANIFEST} --destination ci-suppliers/core', runs)
        self.assertNotIn(f'--batch {self.MANIFEST}', runs)
        for spec in receipt.batch_manifest((ROOT / self.MANIFEST).read_text(encoding='utf-8')):
            self.assertNotIn(f"--name {spec['name']} ", runs)
        self.assertNotIn('if', next(step for step in job['steps'] if 'core-suppliers' in str(step.get('run', ''))))

    def test_bundle_job_is_required(self):
        self.assertIn('core-suppliers', self.jobs['ci-required']['needs'])


if __name__ == '__main__':
    unittest.main()
