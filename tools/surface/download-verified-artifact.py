"""CI only: hash one exact GitHub artifact archive before isolated extraction."""
import argparse
from email.utils import parsedate_to_datetime
import json
import math
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import ssl
import stat
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

# Retry the whole download, never a partially verified archive. In particular,
# authentication, redirect policy, digest and member validation are not transient.
RETRY_DELAYS = (1, 3)
TRANSIENT_HTTP = frozenset((429, 500, 502, 503, 504))
# Same 15-minute quota wait budget as tools/ci/github_read.py (shared hourly quota).
_MAX_RATE_WAIT_SECONDS = 15 * 60
_RATE_HEADERS = ('date', 'retry-after', 'x-ratelimit-remaining', 'x-ratelimit-reset',
                 'x-ratelimit-limit', 'x-ratelimit-used', 'x-ratelimit-resource')

def _reject_json_constant(value):
    # Python accepts these JavaScript extensions by default; GitHub's response
    # contract is JSON, so NaN/Infinity are parse failures rather than metadata.
    raise ValueError(f'Non-JSON constant: {value}')


def _number(value):
    return int(value) if isinstance(value, str) and re.fullmatch(r'[0-9]{1,20}', value) else None


def rate_limit_info(status, headers, body, failures=0):
    """Recognize GitHub quota responses, never an arbitrary forbidden request.

    This small policy is also kept in the standalone archive downloader, whose
    trusted-source users must not acquire a new unchecked import dependency.
    """
    if status not in (403, 429) or len(body) > 16384:
        return None
    try:
        message = json.loads(body, parse_constant=_reject_json_constant).get('message')
    except (ValueError, UnicodeDecodeError, AttributeError):
        return None
    if not isinstance(message, str):
        return None
    primary = message.startswith('API rate limit exceeded for ')
    secondary = message.startswith('You have exceeded a secondary rate limit')
    if not primary and not secondary:
        return None
    remaining = _number(headers.get('x-ratelimit-remaining'))
    reset = _number(headers.get('x-ratelimit-reset'))
    retry_after = _number(headers.get('retry-after'))
    resource = headers.get('x-ratelimit-resource', '')
    info = dict(kind='primary' if primary else 'secondary', remaining=remaining,
                reset=reset, retry_after=retry_after, delay=None,
                limit=_number(headers.get('x-ratelimit-limit')),
                used=_number(headers.get('x-ratelimit-used')),
                resource=resource if re.fullmatch(r'[a-z_]{1,64}', resource) else None)
    if any(key in headers and number is None for key, number in (
            ('retry-after', retry_after), ('x-ratelimit-remaining', remaining),
            ('x-ratelimit-reset', reset))):
        return info
    delay = 0
    if primary or remaining == 0:
        if remaining != 0 or reset is None:
            return info
        try:
            date = parsedate_to_datetime(headers.get('date', ''))
            if date.tzinfo is None:
                return info
            # Server time avoids retrying early when the runner clock is ahead.
            delay = max(1, math.ceil(reset - date.timestamp() + 1))
        except (ValueError, TypeError, OverflowError):
            return info
    if retry_after is not None:
        delay = max(delay, retry_after, 1)
    elif secondary and remaining != 0:
        delay = 60 * (2 ** failures)
    info['delay'] = delay
    return info



class ArtifactError(RuntimeError):
    """A diagnostic code selected by this module, never remote exception text."""
    def __init__(self, code):
        super().__init__(code)
        self.code = code

class DownloadError(ArtifactError):
    def __init__(self, code, transient, quota=None):
        super().__init__(code)
        self.transient = transient
        self.quota = quota
        self.attempts = 0

def download_error(stage, error):
    if isinstance(error, urllib.error.HTTPError):
        status = error.code if type(error.code) is int else 0
        if stage == 'api' and status in (403, 429):
            headers = {}
            for name, value in error.headers.items():
                key = name.lower()
                if key in _RATE_HEADERS:
                    if key in headers:
                        return DownloadError(f'{stage}-http-{status}', False)
                    headers[key] = value.strip()
            try:
                body = error.read(16385)
            except Exception:
                body = b''
            quota = rate_limit_info(status, headers, body)
            return DownloadError(f'{stage}-http-{status}', quota is not None, quota)
        return DownloadError(f'{stage}-http-{status}', status in TRANSIENT_HTTP)
    reason = error.reason if isinstance(error, urllib.error.URLError) else error
    if isinstance(reason, TimeoutError):
        return DownloadError(f'{stage}-timeout', True)
    if isinstance(reason, ssl.SSLCertVerificationError):
        return DownloadError(f'{stage}-tls-verification-failed', False)
    return DownloadError(f'{stage}-transport-failed', False)

def transport(operation, stage):
    try:
        return operation()
    except ArtifactError:
        raise
    except Exception as error:
        if isinstance(error, urllib.error.HTTPError):
            error.close()
        raise download_error(stage, error) from None

def storage_url(location, code):
    try:
        parsed = urllib.parse.urlparse(location)
        safe = parsed.scheme == 'https' and parsed.hostname and not parsed.username and not parsed.password
    except ValueError:
        safe = False
    if not safe:
        raise ArtifactError(code)
    return location

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

class HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        storage_url(newurl, 'storage-redirect-unsafe')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def extract_verified(archive, expected, root, digest):
    root = Path(root).resolve()
    if root.exists() and any(root.iterdir()):
        raise ArtifactError('destination-not-empty')
    if 'sha256:' + digest.hexdigest() != expected:
        raise ArtifactError('archive-digest-mismatch')
    with zipfile.ZipFile(archive) as package:
        seen = set()
        files = set()
        directories = set()
        for member in package.infolist():
            name = member.filename
            path = PurePosixPath(name)
            raw_parts = name.rstrip('/').split('/')
            if not name or '\\' in name or path.is_absolute() or any(part in ('', '.', '..') or re.search(r'[\x00-\x1f<>:"|?*]', part) or part.endswith((' ', '.')) or re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9\u00b9\u00b2\u00b3]|lpt[1-9\u00b9\u00b2\u00b3])(\..*)?', part) for part in raw_parts):
                raise ArtifactError('archive-member-path-unsafe')
            if stat.S_ISLNK(member.external_attr >> 16):
                raise ArtifactError('archive-member-link')
            key = name.rstrip('/').casefold()
            if key in seen:
                raise ArtifactError('archive-member-duplicate')
            seen.add(key)
            parts = key.split('/')
            for index in range(1, len(parts)):
                directories.add('/'.join(parts[:index]))
            if member.is_dir():
                directories.add(key)
            else:
                files.add(key)
            if not root.joinpath(*path.parts).resolve().is_relative_to(root):
                raise ArtifactError('archive-member-escapes-destination')
        if files & directories:
            raise ArtifactError('archive-member-prefix-collision')
        package.extractall(root)

def artifact_location(api, token):
    request = urllib.request.Request(api, headers={'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'})
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=60):
            pass
    except urllib.error.HTTPError as response:
        try:
            if response.code != 302:
                raise download_error('api', response) from None
            return storage_url(response.headers.get('Location', ''), 'api-redirect-unsafe')
        finally:
            response.close()
    except Exception as error:
        raise download_error('api', error) from None
    raise ArtifactError('api-redirect-missing')

def download_verified(artifact_id, expected, root):
    if os.environ.get('GITHUB_ACTIONS') != 'true' or not re.fullmatch(r'[1-9][0-9]*', artifact_id) or not re.fullmatch(r'sha256:[0-9a-f]{64}', expected):
        raise ArtifactError('request-invalid')
    repository = os.environ.get('GITHUB_REPOSITORY', '')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ArtifactError('request-repository-invalid')
    token = os.environ.get('GH_TOKEN', '')
    if not token:
        raise ArtifactError('request-credential-missing')
    api = f'https://api.github.com/repos/{repository}/actions/artifacts/{artifact_id}/zip'
    rate_wait = 0
    rate_failures = 0
    for attempt in range(1, len(RETRY_DELAYS) + 2):
        try:
            # A new signed URL, stream, file and hash on every attempt. The
            # temporary directory is removed before any retry or extraction error.
            with tempfile.TemporaryDirectory(prefix='surface-artifact-') as temp:
                location = artifact_location(api, token)
                archive = Path(temp) / 'artifact.zip'
                digest = hashlib.sha256()
                opener = urllib.request.build_opener(HttpsOnly)
                # The independent storage opener receives only the URL, without
                # the API's Authorization header, including on storage redirects.
                with transport(lambda: opener.open(location, timeout=120), 'storage') as response, archive.open('wb') as output:
                    while chunk := transport(lambda: response.read(1024 * 1024), 'storage'):
                        digest.update(chunk)
                        output.write(chunk)
                extract_verified(archive, expected, root, digest)
            return
        except DownloadError as error:
            error.attempts = attempt
            delay = RETRY_DELAYS[attempt - 1] if attempt <= len(RETRY_DELAYS) else None
            if error.quota is not None:
                info = error.quota
                delay = info['delay']
                if (delay is not None and info['kind'] == 'secondary'
                        and info['remaining'] != 0 and info['retry_after'] is None):
                    delay *= 2 ** rate_failures
                permitted = (delay is not None and rate_wait + delay <= _MAX_RATE_WAIT_SECONDS
                             and attempt <= len(RETRY_DELAYS))
                print('SURFACE_API_RATE_LIMIT ' + ' '.join(
                    f'{key}={info[key]}' for key in ('kind', 'limit', 'used', 'remaining', 'resource', 'reset', 'retry_after'))
                    + f' action={"wait" if permitted else "fail"}', file=sys.stderr, flush=True)
                if not permitted:
                    raise
                rate_wait += delay
                rate_failures += 1
            if not error.transient or delay is None:
                raise
            print(f'SURFACE_ARCHIVE_RETRY id={artifact_id} attempt={attempt} reason={error.code}', file=sys.stderr, flush=True)
            while delay > 0:
                chunk = min(delay, 60)
                time.sleep(chunk)
                delay -= chunk

def failure_code(error):
    if isinstance(error, ArtifactError):
        return error.code
    if isinstance(error, zipfile.BadZipFile):
        return 'archive-invalid'
    if isinstance(error, NotImplementedError):
        return 'archive-compression-unsupported'
    if isinstance(error, PermissionError):
        return 'filesystem-permission-denied'
    if isinstance(error, FileNotFoundError):
        return 'filesystem-file-missing'
    if isinstance(error, OSError):
        return 'filesystem-failed'
    return 'unexpected-error'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--id', required=True)
    parser.add_argument('--digest', required=True)
    parser.add_argument('--destination', required=True, type=Path)
    a = parser.parse_args()
    try:
        root = a.destination.resolve()
        download_verified(a.id, a.digest, root)
    except Exception as error:
        # Never include remote exception text, signed URLs, tokens or ZIP names.
        display_id = a.id if re.fullmatch(r'[1-9][0-9]*', a.id) else 'invalid'
        attempts = f' attempts={error.attempts}' if isinstance(error, DownloadError) else ''
        raise RuntimeError(f'Artifact {display_id} verification/extraction failed (reason={failure_code(error)}{attempts})') from None
    print(f'SURFACE_VERIFIED_ARCHIVE id={a.id} digest={a.digest} destination={root}', flush=True)


if __name__ == "__main__":
    main()
