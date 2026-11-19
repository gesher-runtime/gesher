"""Run CI-only adversarial checks against the actual artifact extractor."""
import hashlib
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import ssl
import sys
import tempfile
import traceback
import types
import unittest
from unittest.mock import patch
import urllib.error
import urllib.request
import zipfile

assert os.environ.get('GITHUB_ACTIONS') == 'true'
spec = importlib.util.spec_from_file_location('surface_artifact', Path(__file__).with_name('download-verified-artifact.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def fixture(root, name, entries):
    path = root / (name + '.zip')
    with zipfile.ZipFile(path, 'w') as archive:
        for entry, content in entries:
            archive.writestr(entry, content)
    data = path.read_bytes()
    return path, hashlib.sha256(data), 'sha256:' + hashlib.sha256(data).hexdigest()

def reject(fn, label):
    try:
        fn()
    except (RuntimeError, OSError, zipfile.BadZipFile):
        print('SURFACE_ARCHIVE_NEGATIVE ' + label + '=rejected', flush=True)
    else:
        raise AssertionError(label + ' accepted')

with tempfile.TemporaryDirectory(prefix='surface-archive-negative-') as temporary:
    root = Path(temporary)
    archive, digest, good = fixture(root, 'valid', [('nested/value.txt', 'actual')])
    reject(lambda: module.extract_verified(archive, 'sha256:' + '0' * 64, root/'wrong', digest), 'wrong-digest')
    assert not (root/'wrong').exists()
    module.extract_verified(archive, good, root/'valid', digest)
    assert (root/'valid/nested/value.txt').read_text() == 'actual'
    reject(lambda: module.extract_verified(archive, good, root/'valid', digest), 'extra-existing-member')
    bad = [('traversal',[('../escape','x')]), ('duplicate',[('A.txt','x'),('a.txt','y')]), ('windows-alias',[('file.txt.','x')]), ('prefix-collision',[('A','x'),('a/child','y')])]
    for device in ('COM', 'LPT'):
        for digit in ('\u00b9', '\u00b2', '\u00b3'):
            label = f'{device.lower()}-superscript-{ord(digit):04x}'
            bad.append((label, [('valid.txt','ok'), (f'folder/{device}{digit}.txt','x')]))
    for label, entries in bad:
        archive, digest, good = fixture(root, label, entries)
        reject(lambda: module.extract_verified(archive, good, root/label, digest), label)
        assert not (root/label).exists()
    reject(lambda: module.extract_verified(root/'absent.zip', good, root/'missing', digest), 'missing-archive')
    reject(lambda: module.HttpsOnly().redirect_request(None, None, 302, '', {}, 'http://example.test/file'), 'insecure-redirect')
    assert not (root/'escape').exists()
    spec2 = importlib.util.spec_from_file_location('surface_assembly', Path(__file__).with_name('assemble-artifacts.py'))
    assembly = importlib.util.module_from_spec(spec2)
    spec2.loader.exec_module(assembly)
    for name in ('baseline', 'refresh', 'unexpected'):
        (root/name).mkdir()
    (root/'baseline/framework.jar').write_text('original')
    (root/'refresh/framework.jar').write_text('new')
    (root/'unexpected/framework.jar').write_text('foreign')
    assembly.assemble(root/'baseline', root/'assembled', root/'owners.json', 'base_framework')
    reject(lambda: assembly.assemble(root/'unexpected', root/'assembled', root/'owners.json', 'base_graphics'), 'unexpected-overlay')
    assert (root/'assembled/framework.jar').read_text() == 'original'
    assembly.assemble(root/'refresh', root/'assembled', root/'owners.json', 'current_framework')
    assert (root/'assembled/framework.jar').read_text() == 'new'
print('SURFACE_ARCHIVE_NEGATIVES complete=15', flush=True)


class DownloadControls(unittest.TestCase):
    token = 'TEST_TOKEN_NEVER_PRINT'
    secret = 'TEST_REMOTE_DETAIL_NEVER_PRINT'
    signed = 'https://storage.example.test/archive.zip?sig=TEST_SIGNED_URL_NEVER_PRINT'

    @staticmethod
    def archive(entries=(('nested/value.txt', 'actual'),)):
        output = io.BytesIO()
        with zipfile.ZipFile(output, 'w') as package:
            for name, value in entries:
                package.writestr(name, value)
        return output.getvalue()

    def simulate(self, *, api=(), storage=(), data=None, expected=None, artifact_id='123',
                 environment=None, extraction_error=None, write_error=None):
        data = self.archive() if data is None else data
        expected = expected or 'sha256:' + hashlib.sha256(data).hexdigest()
        api_plan, storage_plan = list(api), list(storage)
        api_calls, storage_calls, closed, attempted_archives = [], [], [], []
        opened_archives = []
        temporary = tempfile.TemporaryDirectory(prefix='surface-download-test-')
        self.addCleanup(temporary.cleanup)
        destination = Path(temporary.name) / 'output'

        class Response:
            def __init__(self, chunks, label):
                self.chunks, self.label = list(chunks), label
            def read(self, _size):
                value = self.chunks.pop(0) if self.chunks else b''
                if isinstance(value, BaseException):
                    raise value
                return value
            def close(self):
                closed.append(self.label)
            def __enter__(self):
                return self
            def __exit__(self, *_):
                self.close()

        def api_open(request, timeout):
            api_calls.append(request)
            self.assertEqual(timeout, 60)
            self.assertEqual(request.get_header('Authorization'), 'Bearer ' + self.token)
            choice = api_plan.pop(0) if api_plan else 302
            if isinstance(choice, BaseException):
                raise choice
            if choice == 200:
                return Response([], 'api-200')
            code = choice if isinstance(choice, int) else 302
            location = self.signed + '&attempt=' + str(len(api_calls)) if isinstance(choice, int) else choice
            raise urllib.error.HTTPError(request.full_url, code, self.secret,
                                         {'Location': location}, Response([], 'api-' + str(len(api_calls))))

        def storage_open(location, timeout):
            storage_calls.append(location)
            self.assertIsInstance(location, str)  # no API Request/header is reused
            self.assertEqual(timeout, 120)
            choice = storage_plan.pop(0) if storage_plan else data
            if isinstance(choice, BaseException):
                raise choice
            chunks = [choice] if isinstance(choice, bytes) else choice
            return Response(chunks, 'storage-' + str(len(storage_calls)))

        def opener(*handlers):
            self.assertEqual(len(handlers), 1)
            if handlers[0] is module.NoRedirect:
                return types.SimpleNamespace(open=api_open)
            self.assertIs(handlers[0], module.HttpsOnly)
            return types.SimpleNamespace(open=storage_open)

        original_extract = module.extract_verified
        def extract(archive, *args):
            attempted_archives.append(Path(archive))
            if extraction_error is not None:
                raise extraction_error
            return original_extract(archive, *args)

        original_open = Path.open
        def file_open(path, *args, **kwargs):
            output = original_open(path, *args, **kwargs)
            if path.name == 'artifact.zip' and args and args[0] == 'wb':
                opened_archives.append(path)
                if write_error is not None:
                    class FailedOutput:
                        def __enter__(self):
                            return self
                        def __exit__(self, *_):
                            output.close()
                        def write(self, _chunk):
                            raise write_error
                    return FailedOutput()
            return output

        env = dict(GITHUB_ACTIONS='true', GITHUB_REPOSITORY='owner/repository', GH_TOKEN=self.token)
        env.update(environment or {})
        stdout, stderr = io.StringIO(), io.StringIO()
        result, rendered = None, ''
        with patch.dict(os.environ, env), patch.object(module.urllib.request, 'build_opener', side_effect=opener), \
             patch.object(module.time, 'sleep') as sleep, patch.object(module, 'extract_verified', side_effect=extract), \
             patch.object(Path, 'open', new=file_open), \
             patch.object(sys, 'argv', ['download', '--id', artifact_id, '--digest', expected, '--destination', str(destination)]), \
             contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            try:
                module.main()
            except RuntimeError as error:
                result = error
                rendered = ''.join(traceback.format_exception(error))
        logs = stdout.getvalue() + stderr.getvalue() + rendered
        for secret in (self.token, self.secret, 'TEST_SIGNED_URL_NEVER_PRINT', 'TEST_MEMBER_NEVER_PRINT'):
            self.assertNotIn(secret, logs)
        for archive in attempted_archives + opened_archives:
            self.assertFalse(archive.exists(), 'temporary archive survived completion/failure')
        return types.SimpleNamespace(error=result, logs=logs, api=api_calls, storage=storage_calls,
                                     sleeps=sleep.call_args_list, destination=destination, closed=closed,
                                     archives=opened_archives)

    def test_success_and_auth_stays_at_api(self):
        result = self.simulate()
        self.assertIsNone(result.error)
        self.assertEqual((result.destination / 'nested/value.txt').read_text(), 'actual')
        self.assertEqual(len(result.api), 1)
        self.assertEqual(result.sleeps, [])
        self.assertEqual(result.closed, ['api-1', 'storage-1'])
        request = urllib.request.Request(self.signed)
        redirected = module.HttpsOnly().redirect_request(request, None, 302, '', {}, 'https://other.example.test/file')
        self.assertIsNone(redirected.get_header('Authorization'))
        self.assertIsNone(module.NoRedirect().redirect_request(result.api[0], None, 302, '', {}, self.signed))

    def test_transient_api_statuses_retry_with_bound(self):
        for status in (500, 502, 503, 504):
            with self.subTest(status=status):
                result = self.simulate(api=(status, 302))
                self.assertIsNone(result.error)
                self.assertEqual(len(result.api), 2)
                self.assertEqual(len(result.storage), 1)
                self.assertEqual([call.args for call in result.sleeps], [(1,)])
                self.assertIn('reason=api-http-' + str(status), result.logs)
        result = self.simulate(api=(503, 503, 503))
        self.assertIn('reason=api-http-503 attempts=3', str(result.error))
        self.assertEqual(len(result.api), 3)
        self.assertEqual([call.args for call in result.sleeps], [(1,), (3,)])
        self.assertFalse(result.destination.exists())

    def test_permanent_http_statuses_do_not_retry(self):
        for status in (401, 403, 404, 429, 501, 505):
            with self.subTest(status=status):
                result = self.simulate(api=(status,))
                self.assertIn('reason=api-http-' + str(status) + ' attempts=1', str(result.error))
                self.assertEqual(len(result.api), 1)
                self.assertEqual(result.sleeps, [])
                self.assertFalse(result.destination.exists())

    def test_storage_http_retry_fetches_a_fresh_signed_url(self):
        error = urllib.error.HTTPError(self.signed, 503, self.secret, {}, io.BytesIO())
        result = self.simulate(storage=(error, self.archive()))
        self.assertIsNone(result.error)
        self.assertEqual(len(result.api), 2)
        self.assertEqual(len(result.storage), 2)
        self.assertNotEqual(result.storage[0], result.storage[1])
        self.assertIn('reason=storage-http-503', result.logs)

    def test_partial_timeout_restarts_archive_and_digest(self):
        result = self.simulate(storage=([b'partial bytes from failed transfer', TimeoutError(self.secret)], self.archive()))
        self.assertIsNone(result.error)
        self.assertEqual((result.destination / 'nested/value.txt').read_text(), 'actual')
        self.assertEqual(len(result.api), 2)
        self.assertEqual(len(result.storage), 2)
        self.assertNotEqual(result.storage[0], result.storage[1])
        self.assertIn('storage-1', result.closed)
        self.assertIn('reason=storage-timeout', result.logs)
        self.assertEqual(len(result.archives), 2)
        self.assertEqual(len(set(result.archives)), 2)

    def test_storage_timeout_exhaustion_stays_bounded(self):
        result = self.simulate(storage=tuple([b'partial', TimeoutError(self.secret)] for _ in range(3)))
        self.assertIn('reason=storage-timeout attempts=3', str(result.error))
        self.assertEqual(len(result.api), 3)
        self.assertEqual(len(result.storage), 3)
        self.assertEqual([call.args for call in result.sleeps], [(1,), (3,)])
        self.assertFalse(result.destination.exists())

    def test_api_timeout_retries_but_certificate_and_other_transport_errors_do_not(self):
        result = self.simulate(api=(urllib.error.URLError(TimeoutError(self.secret)), 302))
        self.assertIsNone(result.error)
        self.assertEqual(len(result.api), 2)
        for error, reason in ((urllib.error.URLError(ssl.SSLCertVerificationError(self.secret)), 'api-tls-verification-failed'),
                              (urllib.error.URLError(self.secret), 'api-transport-failed'),
                              (RuntimeError(self.secret), 'api-transport-failed')):
            with self.subTest(reason=reason):
                result = self.simulate(api=(error,))
                self.assertIn('reason=' + reason, str(result.error))
                self.assertEqual(len(result.api), 1)
                self.assertEqual(result.sleeps, [])

    def test_storage_auth_failure_does_not_retry(self):
        result = self.simulate(storage=(urllib.error.HTTPError(self.signed, 403, self.secret, {}, io.BytesIO()),))
        self.assertIn('reason=storage-http-403 attempts=1', str(result.error))
        self.assertEqual(len(result.api), 1)
        self.assertEqual(result.sleeps, [])

    def test_redirect_failures_do_not_retry_or_follow_with_auth(self):
        for location in ('http://storage.example.test/file', 'https://user:password@example.test/file', '', 'https://[invalid'):
            with self.subTest(location=location):
                result = self.simulate(api=(location,))
                self.assertIn('reason=api-redirect-unsafe', str(result.error))
                self.assertEqual(result.storage, [])
                self.assertEqual(result.sleeps, [])
        result = self.simulate(api=(200,))
        self.assertIn('reason=api-redirect-missing', str(result.error))
        self.assertIn('api-200', result.closed)
        result = self.simulate(storage=(module.ArtifactError('storage-redirect-unsafe'),))
        self.assertIn('reason=storage-redirect-unsafe', str(result.error))
        self.assertEqual(result.sleeps, [])

    def test_integrity_and_invalid_archives_fail_without_retry(self):
        for data, expected, reason in ((self.archive(), 'sha256:' + '0' * 64, 'archive-digest-mismatch'),
                                       (b'not a ZIP archive', None, 'archive-invalid'),
                                       (self.archive((('ok.txt', 'x'), ('../TEST_MEMBER_NEVER_PRINT', 'x'))), None, 'archive-member-path-unsafe')):
            with self.subTest(reason=reason):
                result = self.simulate(data=data, expected=expected)
                self.assertIn('reason=' + reason, str(result.error))
                self.assertEqual(len(result.api), 1)
                self.assertEqual(result.sleeps, [])
                self.assertFalse(result.destination.exists())

    def test_extraction_timeout_is_not_a_transport_retry(self):
        result = self.simulate(extraction_error=TimeoutError(self.secret))
        self.assertIn('reason=filesystem-failed', str(result.error))
        self.assertEqual(len(result.api), 1)
        self.assertEqual(result.sleeps, [])

    def test_local_write_timeout_is_not_a_transport_retry(self):
        result = self.simulate(write_error=TimeoutError(self.secret))
        self.assertIn('reason=filesystem-failed', str(result.error))
        self.assertEqual(len(result.api), 1)
        self.assertEqual(len(result.storage), 1)
        self.assertEqual(result.sleeps, [])
        self.assertFalse(result.destination.exists())

    def test_invalid_request_is_rejected_before_network(self):
        for kwargs, reason in (({'artifact_id': self.token}, 'request-invalid'),
                               ({'expected': self.secret}, 'request-invalid'),
                               ({'environment': {'GITHUB_ACTIONS': 'false'}}, 'request-invalid'),
                               ({'environment': {'GITHUB_REPOSITORY': 'https://example.test/foreign'}}, 'request-repository-invalid'),
                               ({'environment': {'GH_TOKEN': ''}}, 'request-credential-missing')):
            with self.subTest(reason=reason):
                result = self.simulate(**kwargs)
                self.assertIn('reason=' + reason, str(result.error))
                self.assertEqual(result.api, [])
                self.assertEqual(result.sleeps, [])


if __name__ == '__main__':
    unittest.main()
