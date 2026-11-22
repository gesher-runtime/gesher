"""Real component receipt/ZIP/consumer paths with synthetic transport and payloads.

These fixtures exercise provenance, publication and nested supplier identity.
They never compile or execute a native binary and do not establish Windows
runtime results.
"""
import copy
from contextlib import chdir
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit
import zipfile

import artifact_receipt as receipts
import component_producer as component
import producer_context as proof
import test_artifact_batch as transport


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/toolchain'))


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ComponentConsumers(unittest.TestCase):
    MANIFEST = 'tools/ci/tcp_selected_artifacts.json'
    checkout_read = transport.ArtifactBatch.checkout_read
    opener = transport.ArtifactBatch.opener

    def setUp(self):
        # Reuse the existing HTTP/storage adapter, while running real batch
        # selection, archive extraction, shared authentication and publication.
        transport.ArtifactBatch.setUp(self)
        self.run['event'] = 'pull_request'
        self.run['repository']['id'] = 1426153186
        os.environ['GITHUB_REPOSITORY_ID'] = '1426153186'
        self.enterContext(patch.object(component, 'ROOT', self.root))
        self.historical_jobs = {}
        self.configure('tcp-selected-minimum')

    def configure(self, name, producer_attempt='1', consumer_attempt='2', payload=None):
        self.name = name
        role, filename, _, destination = component.POLICIES[name]
        self.destination = self.root / destination
        self.specs = json.loads((ROOT / 'tools/ci' / filename).read_text())
        self.manifest = self.root / 'tools/ci' / filename
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(json.dumps(self.specs))
        self.committed_manifest = self.manifest.read_bytes()
        os.environ['GITHUB_RUN_ATTEMPT'] = consumer_attempt
        self.run['run_attempt'] = int(consumer_attempt)
        self.consumer = dict(source=self.checkout, supplier_head=self.head,
            repository=os.environ['GITHUB_REPOSITORY'], repository_id='1426153186',
            head_repository_id='9', run='7', attempt=consumer_attempt, event='pull_request')
        self.original = dict(self.consumer, attempt=producer_attempt)
        producer = self.specs[0]['producer']
        old = dict(id=101, name=producer, status='completed', conclusion='success',
            head_sha=self.head, run_id=7, run_attempt=int(producer_attempt),
            started_at='2026-11-19T12:00:00Z', completed_at='2026-11-19T12:01:00Z')
        self.historical_jobs = {producer_attempt: [old]}
        self.jobs = [dict(old, id=201, run_attempt=int(consumer_attempt))]
        if producer_attempt == consumer_attempt:
            self.jobs = [old]
        self.payload = dict(self.original, role=role, synthetic_text_only=True)
        if payload is not None:
            self.payload.update(payload)
        self.raw = (json.dumps(self.payload, sort_keys=True) + '\n').encode()
        data = transport.zip_bytes([('provenance.json', self.raw),
                                    ('metadata.txt', b'SYNTHETIC_TEXT_NOT_NATIVE')])
        self.archives = {1000: data}
        self.items = [dict(id=1000, name=self.specs[0]['name'], expired=False,
            digest='sha256:' + hashlib.sha256(data).hexdigest(), size_in_bytes=len(data),
            created_at='2026-11-19T12:00:30Z', workflow_run=dict(id=7, head_sha=self.head,
                repository_id=1426153186, head_repository_id=9))]
        self.downloads.clear()

    def api_json(self, endpoint):
        path = urlsplit(endpoint).path
        prefix = 'repos/gesher-runtime/gesher/actions/runs/7/attempts/'
        if path.startswith(prefix):
            attempt, *tail = path[len(prefix):].split('/')
            if not tail:
                return dict(copy.deepcopy(self.run), run_attempt=int(attempt))
            if tail == ['jobs']:
                rows = self.jobs if attempt == os.environ['GITHUB_RUN_ATTEMPT'] else self.historical_jobs.get(attempt, [])
                return dict(total_count=len(rows), jobs=copy.deepcopy(rows))
        return transport.ArtifactBatch.api_json(self, endpoint)

    def invoke(self, command, **kwargs):
        if command[0] == 'gh':
            return transport.ArtifactBatch.invoke(self, command, **kwargs)
        self.assertEqual(command[:2], [sys.executable, 'tools/surface/download-verified-artifact.py'])
        identity = int(command[command.index('--id') + 1])
        digest = command[command.index('--digest') + 1]
        destination = Path(command[command.index('--destination') + 1])
        self.downloads.append(destination)
        transport.archive.download_verified(str(identity), digest, destination)
        return subprocess.CompletedProcess(command, 0)

    def stage(self):
        return component.stage(self.name)

    def test_all_fixed_consumers_stage_original_archive_without_relabeling(self):
        for name in component.POLICIES:
            with self.subTest(name=name):
                self.configure(name)
                result = self.stage()
                self.assertEqual(result['producer_context'], self.original)
                self.assertEqual(result['consumer_attempt'], '2')
                self.assertEqual(result['producer']['run_attempt'], 2)
                self.assertEqual(result['artifact']['id'], 1000)
                self.assertEqual((self.destination / 'provenance.json').read_bytes(), self.raw)
                self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '2')
                self.assertEqual(len(self.downloads), 2)

    def test_repeated_consumer_rerun_and_fresh_full_rerun(self):
        for producer, consumer in (('1', '3'), ('2', '2')):
            with self.subTest(producer=producer, consumer=consumer):
                name = 'tcp-selected-minimum' if producer == '1' else 'tsf-format'
                self.configure(name, producer, consumer)
                result = self.stage()
                self.assertEqual(result['producer_context']['attempt'], producer)
                self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], consumer)
                self.assertEqual(len(self.downloads), 2, 'Fresh manifests also require archive authentication')

    def test_latest_execution_selects_its_archive_and_rejects_older_manifest(self):
        self.configure('native3-blocking', '1', '3')
        stale = self.raw
        old_job = copy.deepcopy(self.historical_jobs['1'])
        stale_item, stale_archive = copy.deepcopy(self.items[0]), self.archives[1000]
        self.configure('native3-blocking', '2', '3')
        self.historical_jobs['1'] = old_job
        for job in (self.historical_jobs['2'][0], self.jobs[0]):
            job.update(started_at='2026-11-19T13:00:00Z', completed_at='2026-11-19T13:01:00Z')
        self.items[0].update(id=1001, created_at='2026-11-19T13:00:30Z')
        self.archives[1001] = self.archives.pop(1000)
        self.archives[1000] = stale_archive
        self.items.append(stale_item)
        result = self.stage()
        self.assertEqual(result['artifact']['id'], 1001)
        self.assertEqual(result['producer_context']['attempt'], '2')
        (self.destination / 'provenance.json').write_bytes(stale)
        with self.assertRaisesRegex(RuntimeError, 'retained original'):
            component.verify(self.name)

    def test_local_attempt_relabel_cannot_skip_archive_authentication(self):
        self.stage()
        path = self.destination / 'provenance.json'
        value = json.loads(path.read_bytes())
        value['attempt'] = '2'
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(RuntimeError, 'bytes differ'):
            component.verify(self.name)
        self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '2')

    def test_manifest_identical_archive_replacement_between_batch_and_auth_fails(self):
        original = component.authenticate
        def swap(*args, **kwargs):
            stream = io.BytesIO(self.archives[1000])
            with zipfile.ZipFile(stream, 'a') as package:
                package.comment = b'replacement with identical members'
            data = stream.getvalue()
            self.items[0].update(id=1001, digest='sha256:' + hashlib.sha256(data).hexdigest(),
                                 size_in_bytes=len(data))
            self.archives[1001] = data
            return original(*args, **kwargs)
        with patch.object(component, 'authenticate', side_effect=swap):
            with self.assertRaises(RuntimeError):
                self.stage()
        self.assertFalse(self.destination.exists())

    def test_missing_failed_wrong_source_ambiguous_and_late_suppliers_fail_closed(self):
        mutations = (
            lambda: self.jobs.clear(),
            lambda: self.jobs[0].update(conclusion='failure'),
            lambda: self.jobs[0].update(head_sha='f' * 40),
            lambda: self.items.append(dict(self.items[0], id=1001)),
            lambda: self.items.append(dict(self.items[0], id=1001, created_at='2026-11-19T14:00:00Z')),
        )
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                self.configure('tsf-format')
                mutate()
                with self.assertRaises(RuntimeError):
                    self.stage()
                self.assertFalse(self.destination.exists())

    def test_no_untrusted_role_path_or_existing_destination_override(self):
        for name in ('other', '../../forged'):
            with self.assertRaises(RuntimeError):
                component.stage(name)
        with self.assertRaisesRegex(RuntimeError, 'Unexpected component manifest path'):
            component.verify(self.name, path=self.root / 'other/provenance.json')
        self.destination.mkdir()
        sentinel = self.destination / 'owned.txt'
        sentinel.write_text('preserve')
        with self.assertRaisesRegex(RuntimeError, 'already exists'):
            self.stage()
        self.assertEqual(sentinel.read_text(), 'preserve')
        self.assertEqual(self.downloads, [])

    def selected_payload(self):
        projection = load('component_selected_projection', 'tools/net-tcp-priority/test-selected-supplier.py')
        authority = json.loads((ROOT / 'tests/net-tcp-priority/minimum-receipt-411350.json').read_bytes())
        supplier, _ = projection.project(authority)
        context = component.supplier_context(self.original)
        supplier['context'] = context
        for key in projection.selected.AUTHORITY_CONTEXT:
            supplier['minimum_authority'][key] = context[key]
            supplier['minimum_authority']['original']['context'][key] = context[key]
        supplier['minimum_authority_sha256'] = projection.selected.authority_digest(supplier['minimum_authority'])
        return projection.selected, supplier

    def test_actual_selected_consumer_uses_authenticated_context_for_all_nested_receipts(self):
        selected, supplier = self.selected_payload()
        self.configure(self.name, payload=dict(supplier=supplier, supplier_selection='selected-minimum', producer_job='compile'))
        self.stage()
        import coherent_tcp_context
        observed = []
        def caller(env, job):
            observed.append((env['GITHUB_RUN_ATTEMPT'], job))
            self.assertEqual(env['GITHUB_RUN_ATTEMPT'], '2')
            return component.supplier_context(self.consumer)
        with patch.object(coherent_tcp_context, 'context', side_effect=caller):
            actual = selected.verify_manifest(self.destination / 'provenance.json')
        self.assertEqual(actual['GITHUB_RUN_ATTEMPT'], '1')
        self.assertEqual(observed, [('2', 'windows')])
        self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '2')

    def test_authenticated_tcp_manifest_does_not_authorize_mismatched_nested_attempt(self):
        selected, supplier = self.selected_payload()
        supplier['context']['GITHUB_RUN_ATTEMPT'] = '2'
        self.configure(self.name, payload=dict(supplier=supplier, supplier_selection='selected-minimum', producer_job='compile'))
        self.stage()
        import coherent_tcp_context
        with patch.object(coherent_tcp_context, 'context', return_value=component.supplier_context(self.consumer)):
            with self.assertRaises(AssertionError):
                selected.verify_manifest(self.destination / 'provenance.json')

    def test_correlation_validates_each_producer_with_its_own_context(self):
        self.configure('tcp-selected-correlation', '2', '3')
        _, supplier = self.selected_payload()
        self.configure(self.name, '2', '3', dict(supplier=supplier,
                       supplier_selection='selected-minimum', producer_job='compile'))
        self.stage()
        correlation = load('component_correlation_consumer', 'tools/toolchain/coherent_supplier_correlation.py')
        projection = load('component_host_projection', 'tools/toolchain/test_selected_link.py')
        historical = json.loads((ROOT / 'tests/net-tcp-priority/minimum-receipt-411350.json').read_bytes())
        full = projection.projection(historical)
        host_producer = dict(self.consumer, attempt='1')
        context = component.supplier_context(host_producer)
        full['context'] = context
        for key in correlation.factory.FIELDS[:4]:
            full['authority'][key] = context[key]
            full['authority']['original']['context'][key] = context[key]
        full['authority_sha256'] = hashlib.sha256(json.dumps(
            full['authority'], sort_keys=True, indent=2).encode()).hexdigest()
        host = self.root / 'ci-suppliers/native3_host'
        host.mkdir()
        receipt = host / correlation.host_roles.receipt_name('native3-common-host-fixture')
        raw = json.dumps(full).encode()
        receipt.write_bytes(raw)
        actual = dict(context=host_producer, compiler=dict(selected_toolchain=dict(
            receipt_sha256=hashlib.sha256(raw).hexdigest())))
        def authenticated_host(root, role, files, consumer, sources):
            self.assertEqual(role, 'native3-common-host-fixture')
            self.assertEqual(consumer['attempt'], '3')
            return copy.deepcopy(actual)
        def current_caller(env, job):
            self.assertEqual(job, 'supplier-correlation')
            self.assertEqual(env['GITHUB_RUN_ATTEMPT'], '3')
            return component.supplier_context(self.consumer)
        with chdir(self.root), \
                patch.object(correlation.coherent_tcp_context, 'context', side_effect=current_caller), \
                patch.object(correlation.host_roles, 'context', return_value=self.consumer), \
                patch.object(correlation.union_payload, 'source_inventory', return_value={}), \
                patch.object(correlation.union_payload, 'check_manifest', side_effect=authenticated_host):
            # The authenticated host boundary is adapted; both complete nested
            # supplier validators and the actual correlation main execute.
            correlation.main()
            full['context']['GITHUB_RUN_ATTEMPT'] = '3'
            receipt.write_text(json.dumps(full))
            with self.assertRaises(AssertionError):
                correlation.main()
        self.assertIn('"host_producer_attempt": "1"', self.logs.getvalue())
        self.assertIn('"tcp_producer_attempt": "2"', self.logs.getvalue())
        self.assertEqual(os.environ['GITHUB_RUN_ATTEMPT'], '3')


class CallerWiring(unittest.TestCase):
    def test_cli_keeps_real_downloader_child_output_out_of_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            downloader = root / 'tools/surface/download-verified-artifact.py'
            downloader.parent.mkdir(parents=True)
            downloader.write_text("print('SURFACE_VERIFIED_ARCHIVE synthetic child transport')\n")
            code = f'''import sys
from pathlib import Path
sys.path.insert(0, {str(ROOT / 'tools/ci')!r})
import component_producer as component
def stage(role):
    component.receipts.download({{'id': 7, 'digest': 'sha256:' + 'a' * 64}}, Path('unused'))
    print('PYTHON_AUTHENTICATION_DIAGNOSTIC synthetic')
    return {{'producer_context': {{'attempt': '1'}}, 'consumer_attempt': '2'}}
component.stage = stage
component.main()
'''
            result = subprocess.run([sys.executable, '-c', code, 'stage', 'tsf-format'],
                                    cwd=root, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout),
                             {'producer_context': {'attempt': '1'}, 'consumer_attempt': '2'})
            self.assertNotIn('SURFACE_VERIFIED_ARCHIVE', result.stdout)
            self.assertIn('SURFACE_VERIFIED_ARCHIVE synthetic child transport', result.stderr)
            self.assertIn('PYTHON_AUTHENTICATION_DIAGNOSTIC synthetic', result.stderr)

    def test_canonical_powershell_checks_context_before_native_execution(self):
        for filename, role, marker in (
                ('tools/windows4/run-tsf-union.ps1', 'tsf-format', 'Add-Type -Path'),
                ('tools/net-tcp-priority/run-component.ps1', 'tcp-selected-minimum', 'function RequireDiagnosticRetirement'),
                ('.github/workflows/native3-locks-blocking.yml', 'native3-blocking', '      - name: Actual Windows filesystem metadata')):
            with self.subTest(filename=filename):
                source = (ROOT / filename).read_text()
                command = 'python tools/ci/component_producer.py stage ' + role
                self.assertLess(source.index(command), source.index(marker))
                self.assertIn('$producerAttempt = $proof.producer_context.attempt', source)
                self.assertNotIn('$env:GITHUB_RUN_ATTEMPT =', source)
                self.assertNotIn('$env:GITHUB_RUN_ATTEMPT=', source)
                self.assertIn('if ($LASTEXITCODE -ne 0) { throw', source[source.index(command):])


if __name__ == '__main__':
    unittest.main()
