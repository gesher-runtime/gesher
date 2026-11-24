"""Synthetic Binder bytes; real manifest, digest/ZIP and provenance predicates.

Only GitHub transport and the current checkout context are adapted. None of the
fixtures are Windows/Android execution evidence.
"""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / 'tools/ci'), str(REPO / 'tools/binder')]
import modes_manifest as modes
import producer_context as proof
import github_jobs
import artifact_candidates
spec = importlib.util.spec_from_file_location('binder_test_zip_downloader', REPO / 'tools/surface/download-verified-artifact.py')
zipper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zipper)


class Scenario:

    def __init__(self, old=1, now=3, workflow='ci'):
        self.temp = tempfile.TemporaryDirectory(prefix='rerun-qa-')
        self.root = Path(self.temp.name)
        self.fixture = self.root / 'fixtures'
        self.fixture.mkdir()
        self.old = old
        self.now = now
        self.workflow = workflow
        self.consumer = dict(source='1' * 40, repository='owner/repo', repository_id='7', run='9', attempt=str(now), event='pull_request', supplier_head='2' * 40, head_repository_id='8')
        self.original = dict(self.consumer, attempt=str(old))
        self.producer = 'binder2-sg-fixtures / modes-build' if workflow == 'ci' else 'fixtures / modes-build'
        self.old_job = dict(id=10, name=self.producer, run_id=9, run_attempt=old, head_sha='2' * 40, status='completed', conclusion='success', started_at='2026-11-20T12:01:13Z', completed_at='2026-11-20T12:07:27Z')
        self.new_job = dict(self.old_job, id=20, run_attempt=now)
        self.old_jobs = [self.old_job]
        self.new_jobs = [self.new_job]
        self.old_run = dict(id=9, run_attempt=old, head_sha='2' * 40, name=workflow, event='pull_request', repository=dict(id=7, full_name='owner/repo'), head_repository=dict(id=8))
        self.new_run = dict(self.old_run, run_attempt=now)
        for name in modes.MEMBERS:
            path = self.fixture / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('SYNTHETIC QA, NOT ANDROID EXECUTION: ' + name).encode())
        for abi in modes.ABIS:
            for role in ('server', 'client'):
                with zipfile.ZipFile(self.fixture / f'binder-sg-{role}-{abi}.apk', 'w') as package:
                    for lib in modes.LIBRARIES:
                        package.writestr(f'lib/{abi}/{lib}', (self.fixture / abi / lib).read_bytes())
                    package.writestr('classes.dex', (self.fixture / 'binder-sg.dex').read_bytes())
        self.manifest = dict(self.original, role=modes.ROLE, files=modes.payload(self.fixture))
        self.write_manifest()
        self.archive = self.root / 'archive.zip'
        self.make_archive()
        self.item = dict(id=30, name='binder2-sg-modes', digest='sha256:' + hashlib.sha256(self.archive.read_bytes()).hexdigest(), size_in_bytes=self.archive.stat().st_size, expired=False, created_at='2026-11-20T12:06:40Z', workflow_run=dict(id=9, head_sha='2' * 40, repository_id=7, head_repository_id=8))
        self.items = [self.item]
        self.downloads = 0
        self.runs = 0
        self.post_download = None

    def close(self):
        self.temp.cleanup()

    def write_manifest(self):
        (self.fixture / 'source.json').write_text(json.dumps(self.manifest, sort_keys=True) + '\n')

    def make_archive(self):
        with zipfile.ZipFile(self.archive, 'w') as package:
            for path in sorted(self.fixture.rglob('*')):
                if path.is_file():
                    package.write(path, path.relative_to(self.fixture).as_posix())

    def run_json(self, endpoint, *args, **kwargs):
        base = f"repos/{self.consumer['repository']}/actions/runs/9"
        self.runs += 1
        if endpoint == base:
            return copy.deepcopy(self.new_run)
        if endpoint == f'{base}/attempts/{self.old}':
            return copy.deepcopy(self.old_run)
        if endpoint == f'{base}/attempts/{self.now}':
            return copy.deepcopy(self.new_run)
        raise RuntimeError('Unexpected QA run request: ' + endpoint)

    def job_json(self, endpoint, *args, **kwargs):
        if f'/attempts/{self.old}/jobs?' in endpoint:
            rows = self.old_jobs
        elif f'/attempts/{self.now}/jobs?' in endpoint:
            rows = self.new_jobs
        else:
            raise RuntimeError('Unexpected QA job request: ' + endpoint)
        return dict(total_count=len(rows), jobs=copy.deepcopy(rows))

    def inventory_json(self, endpoint, *args, **kwargs):
        assert endpoint.startswith(f"repos/{self.consumer['repository']}/actions/runs/9/artifacts?")
        return dict(total_count=len(self.items), artifacts=copy.deepcopy(self.items))

    def inventory(self, endpoint, **kwargs):
        return artifact_candidates.read_artifacts(endpoint, read=self.inventory_json, **kwargs)

    def download(self, item, destination):
        self.downloads += 1
        assert item['id'] == self.item['id']
        digest = hashlib.sha256(self.archive.read_bytes())
        zipper.extract_verified(self.archive, item['digest'], destination, digest)
        if self.post_download:
            self.post_download()

    def check(self):
        return modes.verify(self.fixture, self.consumer['source'])

    def verify(self):
        env = dict(GITHUB_ACTIONS='true', GITHUB_EVENT_NAME='pull_request', GITHUB_WORKFLOW=self.workflow, GITHUB_RUN_ATTEMPT=str(self.now), GITHUB_REPOSITORY=self.consumer['repository'])
        with contextlib.ExitStack() as stack:
            stack.enter_context(patch.dict(os.environ, env))
            stack.enter_context(patch.object(modes.host_roles, 'context', return_value=copy.deepcopy(self.consumer)))
            stack.enter_context(patch.object(proof, 'current_context', return_value=copy.deepcopy(self.consumer)))
            stack.enter_context(patch.object(proof.receipts, 'check_checkout', return_value=None))
            stack.enter_context(patch.object(proof.receipts, 'get_json', side_effect=self.run_json))
            stack.enter_context(patch.object(github_jobs, 'get_json', side_effect=self.job_json))
            stack.enter_context(patch.object(proof.receipts, 'read_artifacts', side_effect=self.inventory))
            stack.enter_context(patch.object(proof.receipts, 'download', side_effect=self.download))
            with contextlib.redirect_stdout(io.StringIO()):
                try:
                    return self.check()
                finally:
                    assert os.environ['GITHUB_RUN_ATTEMPT'] == str(self.now)


class BinderModesProvenance(unittest.TestCase):

    def scenario(self, *args, **kwargs):
        value = Scenario(*args, **kwargs)
        self.addCleanup(value.close)
        return value

    def rejected(self, value):
        with self.assertRaises((RuntimeError, KeyError, ValueError, FileNotFoundError, TypeError)):
            value.verify()

    def test_retained_and_fresh_attempts_with_real_archive(self):
        for old, now in ((1, 2), (1, 3), (2, 3), (3, 3)):
            with self.subTest(producer=old, consumer=now):
                value = self.scenario(old, now)
                self.assertEqual(value.verify(), value.original)
                self.assertEqual(value.downloads, 1)

    def test_relabelling_old_manifest_as_current_cannot_skip_archive_proof(self):
        value = self.scenario(1, 3)
        value.manifest['attempt'] = '3'
        value.write_manifest()
        self.rejected(value)
        self.assertEqual(value.downloads, 1)

    def test_known_component_policies_and_unknown_policy(self):
        for workflow in ('binder2-sg-modes-component', 'binder2-stable-readonly-component'):
            with self.subTest(workflow=workflow):
                self.assertEqual(self.scenario(workflow=workflow).verify()['attempt'], '1')
        self.rejected(self.scenario(workflow='attacker-workflow'))

    def test_all_nonattempt_local_identity_fields(self):
        for field in proof.CONTEXT_KEYS:
            if field == 'attempt':
                continue
            with self.subTest(field=field):
                value = self.scenario()
                value.manifest[field] = 'forged'
                value.write_manifest()
                self.rejected(value)

    def test_future_noncanonical_and_missing_attempt(self):
        for attempt in ('0', '-1', '01', '4', None, 1, True):
            with self.subTest(attempt=attempt):
                value = self.scenario()
                value.manifest['attempt'] = attempt
                value.write_manifest()
                self.rejected(value)
        value = self.scenario()
        del value.manifest['attempt']
        value.write_manifest()
        self.rejected(value)

    def test_strict_manifest_member_schema(self):
        for mutation in ('extra-key', 'duplicate-key', 'invalid-constant', 'path', 'missing-member', 'wrong-case'):
            with self.subTest(mutation=mutation):
                value = self.scenario()
                if mutation == 'extra-key':
                    value.manifest['override_attempt'] = '3'
                if mutation == 'path':
                    value.manifest['files']['../outside'] = value.manifest['files'].pop('binder-sg.dex')
                if mutation == 'missing-member':
                    del value.manifest['files']['binder-sg.dex']
                if mutation == 'wrong-case':
                    value.manifest['files']['BINDER-SG.DEX'] = value.manifest['files'].pop('binder-sg.dex')
                value.write_manifest()
                path = value.fixture / 'source.json'
                if mutation == 'duplicate-key':
                    path.write_text(path.read_text().replace('"attempt": "1"', '"attempt": "3", "attempt": "1"'))
                if mutation == 'invalid-constant':
                    path.write_text(path.read_text().replace('"attempt": "1"', '"attempt": NaN'))
                self.rejected(value)

    def test_all_eleven_tampered_member_bytes(self):
        self.assertEqual(len(modes.MEMBERS), 11)
        for member in modes.MEMBERS:
            with self.subTest(member=member):
                value = self.scenario()
                (value.fixture / member).write_bytes(b'TAMPERED')
                self.rejected(value)

    def test_rehashed_forged_local_manifest_is_bound_to_archive(self):
        value = self.scenario()
        (value.fixture / 'binder-sg.dex').write_bytes(b'TAMPERED')
        value.manifest['files'] = modes.payload(value.fixture)
        value.write_manifest()
        self.rejected(value)

    def test_extra_missing_linked_payloads(self):
        for mutation in ('extra', 'missing', 'symlink'):
            with self.subTest(mutation=mutation):
                value = self.scenario()
                if mutation == 'extra':
                    (value.fixture / 'extra.bin').write_bytes(b'extra')
                else:
                    path = value.fixture / 'binder-sg.dex'
                    old = path.read_bytes()
                    path.unlink()
                    if mutation == 'symlink':
                        outside = value.root / 'outside'
                        outside.write_bytes(old)
                        path.symlink_to(outside)
                self.rejected(value)

    def test_historical_and_current_run_identity(self):
        for side in ('old_run', 'new_run'):
            for field, replacement in (('id', 99), ('run_attempt', 99), ('head_sha', '3' * 40), ('name', 'other'), ('repository', dict(id=99, full_name='owner/repo')), ('repository', dict(id=7, full_name='other/repo')), ('head_repository', dict(id=99))):
                with self.subTest(side=side, field=field):
                    value = self.scenario()
                    getattr(value, side)[field] = replacement
                    self.rejected(value)
        value = self.scenario()
        value.old_run['event'] = 'push'
        self.rejected(value)

    def test_job_record_validation_missing_duplicate_reexecution(self):
        for side in ('old_jobs', 'new_jobs'):
            for mutation in ('missing', 'duplicate', 'id-bool', 'missing-head', 'bad-status', 'bad-conclusion', 'bad-head', 'bad-run', 'bad-attempt', 'reexecuted'):
                with self.subTest(side=side, mutation=mutation):
                    value = self.scenario()
                    rows = getattr(value, side)
                    job = rows[0]
                    if mutation == 'missing':
                        rows.clear()
                    elif mutation == 'duplicate':
                        rows.append(dict(job, id=100))
                    elif mutation == 'id-bool':
                        job['id'] = True
                    elif mutation == 'missing-head':
                        del job['head_sha']
                    elif mutation == 'bad-status':
                        job['status'] = 'in_progress'
                    elif mutation == 'bad-conclusion':
                        job['conclusion'] = 'failure'
                    elif mutation == 'bad-head':
                        job['head_sha'] = '3' * 40
                    elif mutation == 'bad-run':
                        job['run_id'] = 10
                    elif mutation == 'bad-attempt':
                        job['run_attempt'] = 9
                    else:
                        job.update(started_at='2026-11-20T13:01:13Z', completed_at='2026-11-20T13:07:27Z')
                    self.rejected(value)

    def test_artifact_inventory_and_metadata_validation(self):
        for mutation in ('missing', 'duplicate', 'id-bool', 'bad-name', 'bad-digest', 'no-digest', 'expired', 'empty', 'early', 'late', 'missing-time', 'wrong-run', 'wrong-head', 'wrong-repo', 'wrong-head-repo'):
            with self.subTest(mutation=mutation):
                value = self.scenario()
                item = value.item
                if mutation == 'missing':
                    value.items.clear()
                elif mutation == 'duplicate':
                    value.items.append(dict(item, id=31))
                elif mutation == 'id-bool':
                    item['id'] = True
                elif mutation == 'bad-name':
                    item['name'] = 'wrong'
                elif mutation == 'bad-digest':
                    item['digest'] = 'sha256:' + 'x' * 64
                elif mutation == 'no-digest':
                    del item['digest']
                elif mutation == 'expired':
                    item['expired'] = True
                elif mutation == 'empty':
                    item['size_in_bytes'] = 0
                elif mutation == 'early':
                    item['created_at'] = '2026-11-20T11:06:40Z'
                elif mutation == 'late':
                    item['created_at'] = '2026-11-20T13:06:40Z'
                elif mutation == 'missing-time':
                    del item['created_at']
                else:
                    item['workflow_run'][{'wrong-run': 'id', 'wrong-head': 'head_sha', 'wrong-repo': 'repository_id', 'wrong-head-repo': 'head_repository_id'}[mutation]] = 99
                self.rejected(value)

    def test_real_zip_digest_and_archived_manifest(self):
        for mutation in ('archive-bytes', 'archive-manifest'):
            with self.subTest(mutation=mutation):
                value = self.scenario()
                if mutation == 'archive-bytes':
                    value.archive.write_bytes(value.archive.read_bytes() + b'tampered trailing bytes')
                else:
                    with zipfile.ZipFile(value.archive, 'w') as package:
                        package.writestr('source.json', b'forged')
                    value.item['digest'] = 'sha256:' + hashlib.sha256(value.archive.read_bytes()).hexdigest()
                    value.item['size_in_bytes'] = value.archive.stat().st_size
                self.rejected(value)

    def test_post_download_change_rejected(self):
        for mutation in ('advance-run', 'old-job-id', 'new-job-id', 'digest', 'local-manifest'):
            with self.subTest(mutation=mutation):
                value = self.scenario()
                actions = {'advance-run': lambda: value.new_run.update(run_attempt=4), 'old-job-id': lambda: value.old_job.update(id=101), 'new-job-id': lambda: value.new_job.update(id=101), 'digest': lambda: value.item.update(digest='sha256:' + '4' * 64), 'local-manifest': lambda: (value.fixture / 'source.json').write_text('{}')}
                value.post_download = actions[mutation]
                self.rejected(value)

    def test_seal_preserves_apk_native_and_dex_checks(self):
        value = self.scenario()
        with patch.object(modes.host_roles, 'context', return_value=value.original), contextlib.redirect_stdout(io.StringIO()):
            modes.seal(value.fixture)
        for member in ('classes.dex', 'lib/x86_64/libbindersg.so', 'lib/x86_64/libc++_shared.so'):
            with self.subTest(member=member):
                value = self.scenario()
                apk = value.fixture / 'binder-sg-client-x86_64.apk'
                with zipfile.ZipFile(apk, 'w') as package:
                    for name, data in [('classes.dex', (value.fixture / 'binder-sg.dex').read_bytes()), ('lib/x86_64/libbindersg.so', (value.fixture / 'x86_64/libbindersg.so').read_bytes()), ('lib/x86_64/libc++_shared.so', (value.fixture / 'x86_64/libc++_shared.so').read_bytes())]:
                        package.writestr(name, b'altered' if name == member else data)
                with patch.object(modes.host_roles, 'context', return_value=value.original), self.assertRaises(RuntimeError):
                    modes.seal(value.fixture)
if __name__ == '__main__':
    unittest.main()
