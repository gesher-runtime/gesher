"""Exercise real receipt selection and ZIP extraction across a staged batch.

Only GitHub transport and the remote storage boundary are adapted. Selectors,
producer/source predicates, archive digest/member checks and filesystem publish
run as production code. The fake endpoint counts actual requested pages.
"""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import urllib.error
from urllib.parse import parse_qs, urlsplit
import zipfile

import artifact_receipt as receipt
from test_github_read import response

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('batch_archive', ROOT / 'tools/surface/download-verified-artifact.py')
archive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(archive)


def zip_bytes(entries):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w') as package:
        for name, value in entries: package.writestr(name, value)
    return stream.getvalue()


class ArtifactBatch(unittest.TestCase):
    MANIFEST = 'tools/ci/test_shard_artifacts.json'

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = self.root / 'manifest.json'
        self.specs = json.loads((ROOT / self.MANIFEST).read_text())
        self.manifest.write_text(json.dumps(self.specs))
        self.committed_manifest = self.manifest.read_bytes()
        self.destination = self.root / 'suppliers' / 'core'
        self.event = self.root / 'event.json'
        self.head, self.checkout = 'b' * 40, 'a' * 40
        self.event.write_text(json.dumps({'pull_request': {'head': {'sha': self.head, 'repo': {'id': 9}}}}))
        self.env = {'GITHUB_ACTIONS': 'true', 'GITHUB_REPOSITORY': 'gesher-runtime/gesher',
            'GITHUB_RUN_ID': '7', 'GITHUB_RUN_ATTEMPT': '2', 'GITHUB_REPOSITORY_ID': '8',
            'GITHUB_SHA': self.checkout, 'GITHUB_EVENT_PATH': str(self.event),
            'GITHUB_EVENT_NAME': 'pull_request', 'GITHUB_WORKFLOW': 'ci',
            'GITHUB_JOB': 'test-shards', 'GH_TOKEN': 'TEST_LOCAL_TRANSPORT_ONLY'}
        self.run = {'id': 7, 'run_attempt': 2, 'head_sha': self.head, 'name': 'ci',
            'repository': {'id': 8, 'full_name': 'gesher-runtime/gesher'}, 'head_repository': {'id': 9}}
        names = list(dict.fromkeys(spec['producer'] for spec in self.specs))
        self.jobs = [dict(id=100 + index, name=name, status='completed', conclusion='success',
            head_sha=self.head, run_id=7, run_attempt=2, started_at='2026-11-19T12:00:00Z',
            completed_at='2026-11-19T12:01:00Z') for index, name in enumerate(names)]
        self.jobs += [dict(id=100 + index, name='unrelated-' + str(index), status='in_progress')
                      for index in range(len(self.jobs), 73)]
        self.archives, self.items = {}, []
        for index, spec in enumerate(self.specs):
            identity = index + 1000
            data = zip_bytes([('actual/' + spec['destination'] + '.txt', spec['name'])])
            self.archives[identity] = data
            self.items.append(dict(id=identity, name=spec['name'], expired=False,
                digest='sha256:' + hashlib.sha256(data).hexdigest(), size_in_bytes=len(data),
                created_at='2026-11-19T12:00:30Z', workflow_run={'id': 7, 'head_sha': self.head,
                    'repository_id': 8, 'head_repository_id': 9}))
        self.final_run, self.final_jobs, self.final_items = None, None, None
        self.json_reads, self.redirects, self.storage_reads, self.downloads = [], [], [], []
        self.snapshots = 0
        self.logs = io.StringIO()
        self.failure_after_download = None
        self.checkout_reads = 0
        self.race_destination = False
        self.error_artifact = None
        self.enterContext(patch.dict(os.environ, self.env, clear=True))
        self.enterContext(patch('subprocess.run', side_effect=self.invoke))
        self.enterContext(patch.object(receipt, 'ROOT', self.root))
        self.enterContext(patch('subprocess.check_output', side_effect=self.checkout_read))
        self.enterContext(patch.object(archive.urllib.request, 'build_opener', side_effect=self.opener))
        self.enterContext(patch('github_jobs.time.sleep'))
        self.enterContext(contextlib.redirect_stdout(self.logs))
        self.enterContext(contextlib.redirect_stderr(self.logs))

    def checkout_read(self, *args, **kwargs):
        if args[0][:2] == ['git', 'show']:
            return self.committed_manifest
        self.checkout_reads += 1
        if self.race_destination and self.checkout_reads == 2:
            self.destination.mkdir()
            (self.destination / 'foreign.txt').write_text('preserve')
        return self.checkout + '\n'

    def api_json(self, endpoint):
        parsed = urlsplit(endpoint)
        query = parse_qs(parsed.query)
        final = len(self.downloads) == len(self.specs)
        if parsed.path.endswith('/actions/runs/7'):
            self.snapshots += 1
            return copy.deepcopy(self.final_run if final and self.final_run is not None else self.run)
        page = int(query.get('page', ['1'])[0])
        size = int(query.get('per_page', ['100'])[0])
        if parsed.path.endswith('/attempts/2/jobs'):
            rows = self.final_jobs if final and self.final_jobs is not None else self.jobs
            return {'total_count': len(rows), 'jobs': copy.deepcopy(rows[(page-1)*size:page*size])}
        if parsed.path.endswith('/artifacts'):
            rows = self.final_items if final and self.final_items is not None else self.items
            if 'name' in query: rows = [item for item in rows if item['name'] == query['name'][0]]
            return {'total_count': len(rows), 'artifacts': copy.deepcopy(rows[(page-1)*size:page*size])}
        raise AssertionError('Unexpected or altered endpoint: ' + endpoint)

    def invoke(self, command, **kwargs):
        if command[0] == 'gh':
            self.assertEqual(command[:6], ['gh', 'api', '--method', 'GET', '--include', '--'])
            self.json_reads.append(command[-1])
            return response(self.api_json(command[-1]))
        self.assertEqual(command[:2], [sys.executable, 'tools/surface/download-verified-artifact.py'])
        identity = int(command[command.index('--id') + 1])
        digest = command[command.index('--digest') + 1]
        destination = Path(command[command.index('--destination') + 1])
        self.assertFalse(self.destination.exists(), 'Batch became visible before every archive and revalidation')
        self.downloads.append(destination)
        archive.download_verified(str(identity), digest, destination)
        if self.failure_after_download == len(self.downloads):
            raise subprocess.CalledProcessError(1, command)
        return subprocess.CompletedProcess(command, 0)

    def opener(self, handler):
        if handler is archive.NoRedirect:
            def redirect(request, timeout):
                self.assertEqual(request.get_header('Authorization'), 'Bearer TEST_LOCAL_TRANSPORT_ONLY')
                self.redirects.append(request.full_url)
                identity = int(request.full_url.split('/')[-2])
                if self.error_artifact == identity:
                    raise urllib.error.HTTPError(request.full_url, 403, 'Forbidden', {}, io.BytesIO())
                raise urllib.error.HTTPError(request.full_url, 302, '',
                    {'Location': f'https://storage.test/{identity}'}, io.BytesIO())
            return types.SimpleNamespace(open=redirect)
        self.assertIs(handler, archive.HttpsOnly)
        def storage(location, timeout):
            self.assertIsInstance(location, str)
            self.storage_reads.append(location)
            return io.BytesIO(self.archives[int(location.rsplit('/', 1)[1])])
        return types.SimpleNamespace(open=storage)

    def execute(self):
        receipt.main(['--batch', str(self.manifest), '--destination', str(self.destination)])

    def unpublished(self):
        self.assertFalse(self.destination.exists())
        self.assertFalse(list(self.destination.parent.glob('core-stage-*')))
        self.assertNotIn('CI_ARTIFACT_RECEIPT ', self.logs.getvalue())

    def test_complete_real_batch_has_20_api_requests_and_only_publishes_verified_members(self):
        self.execute()
        # One run + one 100-row job page + one artifact page, twice.
        self.assertEqual(len(self.json_reads), 6)
        self.assertEqual(len(self.redirects), 14)
        self.assertEqual(len(self.storage_reads), 14)
        self.assertEqual(len(self.json_reads) + len(self.redirects), 20)
        for spec in self.specs:
            path = self.destination / 'artifacts' / spec['destination'] / 'actual' / (spec['destination'] + '.txt')
            self.assertEqual(path.read_text(), spec['name'])
        self.assertFalse(list(self.destination.parent.glob('core-stage-*')))
        self.assertEqual(self.logs.getvalue().count('CI_ARTIFACT_RECEIPT '), 14)

    def test_paginated_inventory_can_grow_with_an_unrelated_artifact(self):
        self.items += [dict(id=2000 + i, name='unrelated-' + str(i)) for i in range(89)]
        self.final_items = copy.deepcopy(self.items) + [dict(id=4000, name='later-unrelated')]
        self.execute()
        self.assertEqual(len(self.items), 103)
        self.assertEqual(len(self.json_reads), 8)
        self.assertEqual(len(self.redirects), 14)
        self.assertEqual(sum('/artifacts?per_page=100&page=2' in p for p in self.json_reads), 2)
        self.assertEqual(len(list((self.destination / 'artifacts').iterdir())), 14)

    def test_failed_or_wrong_source_producer_never_downloads(self):
        for key, value in (('conclusion', 'failure'), ('run_attempt', 1), ('head_sha', 'c'*40)):
            with self.subTest(key=key):
                original = self.jobs[0][key]
                self.jobs[0][key] = value
                with self.assertRaisesRegex(RuntimeError, 'Producer source/attempt/conclusion'):
                    self.execute()
                self.jobs[0][key] = original
                self.assertEqual(self.downloads, [])
                self.unpublished()

    def test_expired_ambiguous_or_wrong_digest_metadata_never_downloads(self):
        first = copy.deepcopy(self.items[0])
        for key, value in (('expired', True), ('digest', 'bad'), ('size_in_bytes', 0)):
            with self.subTest(key=key):
                self.items[0][key] = value
                with self.assertRaises(RuntimeError): self.execute()
                self.items[0] = copy.deepcopy(first)
                self.assertEqual(self.downloads, [])
                self.unpublished()
        self.items.append(dict(first, id=4000))
        with self.assertRaisesRegex(RuntimeError, 'ambiguous'): self.execute()
        self.assertEqual(self.downloads, [])
        self.unpublished()

    def test_new_run_attempt_or_head_during_download_discards_all_staging(self):
        self.final_run = dict(self.run, run_attempt=3)
        with self.assertRaisesRegex(RuntimeError, 'Current run/source/attempt/repository'):
            self.execute()
        self.assertEqual(len(self.downloads), 14)
        self.unpublished()

    def test_selected_producer_or_artifact_change_after_download_is_rejected(self):
        self.final_jobs = copy.deepcopy(self.jobs)
        self.final_jobs[0]['id'] = 999
        with self.assertRaisesRegex(RuntimeError, 'supplier identity changed'):
            self.execute()
        self.unpublished()

    def test_selected_digest_change_after_download_is_rejected(self):
        self.final_items = copy.deepcopy(self.items)
        self.final_items[0]['digest'] = 'sha256:' + 'f' * 64
        with self.assertRaisesRegex(RuntimeError, 'supplier identity changed'):
            self.execute()
        self.unpublished()

    def test_expiry_or_late_same_name_on_revalidation_cannot_publish(self):
        self.final_items = copy.deepcopy(self.items)
        self.final_items.append(dict(self.items[0], id=4000, created_at='2026-11-19T12:02:00Z'))
        with self.assertRaisesRegex(RuntimeError, 'after the exact producer'):
            self.execute()
        self.unpublished()

    def test_partial_download_api_failure_has_no_receipts_or_published_batch(self):
        self.error_artifact = 1001
        with self.assertRaises(archive.DownloadError): self.execute()
        self.assertEqual(len(self.downloads), 2)
        self.unpublished()

    def test_corrupt_or_traversing_archive_is_not_published(self):
        bad = zip_bytes([('../escape.txt', 'forbidden')])
        self.archives[1001] = bad
        self.items[1]['digest'] = 'sha256:' + hashlib.sha256(bad).hexdigest()
        with self.assertRaisesRegex(archive.ArtifactError, 'archive-member-path-unsafe'):
            self.execute()
        self.unpublished()
        self.assertFalse((self.root / 'escape.txt').exists())

    def test_partial_verified_download_then_subprocess_failure_is_cleaned(self):
        self.failure_after_download = 2
        with self.assertRaises(subprocess.CalledProcessError): self.execute()
        self.unpublished()

    def test_existing_or_racing_destination_is_never_overwritten(self):
        self.destination.mkdir(parents=True)
        sentinel = self.destination / 'foreign.txt'; sentinel.write_text('preserve')
        with self.assertRaisesRegex(RuntimeError, 'already exists'): self.execute()
        self.assertEqual(sentinel.read_text(), 'preserve')
        self.assertEqual(self.json_reads, [])
        sentinel.unlink(); self.destination.rmdir()
        self.race_destination = True
        with self.assertRaises(FileExistsError): self.execute()
        self.assertEqual(sentinel.read_text(), 'preserve')
        self.assertFalse(list(self.destination.parent.glob('core-stage-*')))
        self.assertNotIn('CI_ARTIFACT_RECEIPT ', self.logs.getvalue())

    def test_rename_failure_removes_our_empty_container_and_staging(self):
        with patch.object(receipt.os, 'rename', side_effect=OSError('injected rename failure')):
            with self.assertRaisesRegex(OSError, 'injected rename'): self.execute()
        self.unpublished()

    def test_manifest_unknown_duplicate_fields_and_windows_collisions_fail_before_api(self):
        cases = [self.specs + [dict(self.specs[0])],
                 [dict(self.specs[0], unknown=True)],
                 [dict(self.specs[0], destination='../escape')],
                 [dict(self.specs[0], destination='CON.txt')],
                 [dict(self.specs[0], destination='folder/subdir')],
                 [dict(self.specs[0], destination='folder\\subdir')],
                 [dict(self.specs[0]), dict(self.specs[1], destination=self.specs[0]['destination'].upper())],
                 [dict(self.specs[0], allow_failed_producer=True)]]
        for value in cases:
            with self.subTest(value=value):
                self.manifest.write_text(json.dumps(value))
                with self.assertRaises(ValueError): self.execute()
                self.assertEqual(self.json_reads, [])
                self.unpublished()
        self.manifest.write_text('[{"name":"first","name":"second","producer":"build","destination":"one"}]')
        with self.assertRaisesRegex(ValueError, 'Duplicate batch manifest key'): self.execute()
        self.assertEqual(self.json_reads, [])

    def test_batch_cannot_use_the_installer_failed_producer_exception(self):
        with self.assertRaises(SystemExit):
            receipt.main(['--batch', str(self.manifest), '--destination', str(self.destination), '--allow-failed-producer'])
        self.assertEqual(self.json_reads, [])
        self.unpublished()

    def test_validly_shaped_but_modified_manifest_fails_exact_checkout_binding(self):
        changed = copy.deepcopy(self.specs)
        changed[0]['producer'] = 'another-successful-job'
        self.manifest.write_text(json.dumps(changed))
        with self.assertRaisesRegex(RuntimeError, 'differs from the exact checkout'):
            self.execute()
        self.assertEqual(self.json_reads, [])
        self.unpublished()

    def test_symlinked_destination_parent_is_rejected_before_api(self):
        target = self.root / 'elsewhere'
        target.mkdir()
        try:
            self.destination.parent.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest('Host cannot create directory symlinks')
        with self.assertRaisesRegex(RuntimeError, 'linked path component'):
            self.execute()
        self.assertEqual(self.json_reads, [])
        self.assertEqual(list(target.iterdir()), [])

    def test_windows_crlf_checkout_keeps_the_same_committed_manifest(self):
        self.committed_manifest = json.dumps(self.specs, indent=2).encode() + b'\n'
        self.manifest.write_bytes(self.committed_manifest.replace(b'\n', b'\r\n'))
        self.execute()
        self.assertEqual(len(self.redirects), 14)
        self.assertTrue((self.destination / 'artifacts').is_dir())



    def _complete(self, manifest):
        """Run a committed CI manifest through the real batch path."""
        self.doCleanups()
        self.MANIFEST = manifest
        self.setUp()
        self.execute()
        count = len(self.specs)
        # One run + one 100-row job page + one artifact page, twice, plus one zip per member.
        self.assertEqual(len(self.json_reads), 6)
        self.assertEqual(len(self.redirects), count)
        for spec in self.specs:
            path = self.destination / 'artifacts' / spec['destination'] / 'actual' / (spec['destination'] + '.txt')
            self.assertEqual(path.read_text(), spec['name'])
        self.assertEqual(self.logs.getvalue().count('CI_ARTIFACT_RECEIPT '), count)

    def test_web3_query_batch_replaces_eleven_single_receipts(self):
        self._complete('tools/ci/web3_query_artifacts.json')

    def test_package_batch_replaces_seven_single_receipts(self):
        self._complete('tools/ci/package_artifacts.json')

    def test_binder2_batch_replaces_six_single_receipts(self):
        self._complete('tools/ci/binder2_sg_artifacts.json')


class BatchManifestWiring(unittest.TestCase):
    """Each batch job downloads exactly its committed manifest from producers it waits for."""
    JOBS = {'test-shards': ('tools/ci/test_shard_artifacts.json', 'ci-suppliers/core'),
            'web3-query-regression': ('tools/ci/web3_query_artifacts.json', 'ci-suppliers/web3'),
            'package': ('tools/ci/package_artifacts.json', 'ci-suppliers/package'),
            'binder2-sg-regression': ('tools/ci/binder2_sg_artifacts.json', 'ci-suppliers/binder2')}
    # How many leading members are assembled into dist, and their ledger.
    # The remaining members are placed whole at the paths their consumers read.
    ASSEMBLED = {'binder2-sg-regression': (3, 'binder-full-owners.json')}
    PLACED = {'binder2-sg-regression': {'binder2_sg_host': 'ci-suppliers/binder2_sg_host',
                                        'binder2_sg_modes': 'fixtures', 'boot_image': 'boot-image'}}
    # YAML removes the block indentation of `run: |`.
    ASSEMBLE = ('  python tools/surface/assemble-artifacts.py --source "{destination}/artifacts/$owner" '
                '--destination dist --ledger {ledger} --owner $owner\n'
                '  if ($LASTEXITCODE -ne 0) {{ exit $LASTEXITCODE }}\n')

    def setUp(self):
        import yaml
        self.yaml = yaml
        self.workflow = yaml.safe_load((ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8'))

    def producer_exists(self, producer):
        """`job` is a ci job; `caller / job` is a job inside the reusable workflow `caller` calls."""
        caller, _, inner = producer.partition(' / ')
        job = self.workflow['jobs'][caller]
        if not inner:
            return 'uses' not in job
        uses = job.get('uses', '')
        pinned = re.fullmatch(r'gesher-runtime/gesher/(\.github/workflows/[\w.-]+)@([0-9a-f]{40})', uses)
        if uses.startswith('./.github/workflows/'):
            text = (ROOT / uses[2:]).read_text(encoding='utf-8')
        elif pinned:
            # A reusable workflow pinned to an exact commit of this repository.
            text = subprocess.check_output(['git', 'show', f'{pinned[2]}:{pinned[1]}'], cwd=ROOT, text=True)
        else:
            return False
        called = self.yaml.safe_load(text)
        return any(key == inner or value.get('name') == inner for key, value in called['jobs'].items())

    def test_jobs_use_their_manifest_and_assemble_every_member_from_the_batch(self):
        for job_name, (manifest, destination) in self.JOBS.items():
            with self.subTest(job=job_name):
                job = self.workflow['jobs'][job_name]
                runs = chr(10).join(str(step.get('run', '')) for step in job['steps'])
                specs = receipt.batch_manifest((ROOT / manifest).read_text(encoding='utf-8'))
                self.assertIn(f'artifact_receipt.py --batch {manifest} --destination {destination}', runs)
                for spec in specs:
                    self.assertIn(spec['producer'].split(' / ', 1)[0], set(job['needs']), spec)
                    self.assertTrue(self.producer_exists(spec['producer']), spec)
                if job_name == 'test-shards':
                    continue
                # Single receipts for batch members would repeat the API reads.
                for spec in specs:
                    self.assertNotIn(f"--name {spec['name']} ", runs)
                # Assembly order decides collisions: it must be the manifest order,
                # and each iteration must stop on a failed assembly.
                count, ledger = self.ASSEMBLED.get(job_name, (len(specs), 'ci-owners.json'))
                owners = ', '.join("'" + spec['destination'] + "'" for spec in specs[:count])
                loop = (f'foreach ($owner in {owners}) {{\n' +
                        self.ASSEMBLE.format(destination=destination, ledger=ledger) + '}\n')
                self.assertIn(loop, runs)
                placed = self.PLACED.get(job_name, {})
                self.assertEqual(set(placed), {spec['destination'] for spec in specs[count:]})
                for member, target in placed.items():
                    self.assertIn(f"@('{member}', '{target}')", runs)
                if placed:
                    self.assertIn('Move-Item -LiteralPath "' + destination +
                                  '/artifacts/$($pair[0])" -Destination $pair[1]', runs)
                    self.assertIn('if (Test-Path -LiteralPath $pair[1]) { throw', runs)


if __name__ == '__main__':
    unittest.main()
