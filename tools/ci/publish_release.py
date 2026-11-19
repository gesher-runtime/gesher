"""Publish one source-bound installer using the protected main workflow."""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import time
from urllib.parse import quote

from github_jobs import get_jobs
from github_read import GitHubReadError, get_json
from release_version import (API, DIGEST, MAX_INSTALLER_SIZE, REPOSITORY, REPOSITORY_ID,
                             installer_name, release_catalog, release_version,
                             stable_version, timestamp, version_string)


CI_PATH = '.github/workflows/ci.yml'
CI_ID = 389851347
PUBLISH_PATH = '.github/workflows/publish-release.yml'
PUBLISH_NAME = 'Publish Gesher release'
PUBLISH_REF = f'{REPOSITORY}/{PUBLISH_PATH}@refs/heads/main'
RELEASE_STEP = 'Validate pushed version tag'
UPLOAD_STEP = 'Upload tested installer artifact'
PUBLISH_STEP = 'Verify and publish the exact tested installer'
ARTIFACT_NAME = 'gesher-setup'
CHECKSUM_NAME = 'SHA256SUMS.txt'
BOT = {'login': 'github-actions[bot]', 'id': 41898282, 'type': 'Bot'}
OID = re.compile(r'[0-9a-f]{40}')
NONCE = re.compile(r'[0-9a-f]{64}')
LOG_PREFIX = re.compile(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z ')
MARKER = '<!-- gesher-release-publisher-v1 '


class ReleaseError(RuntimeError):
    pass


class MutationUnconfirmed(ReleaseError):
    """A mutation was attempted and may already have taken effect."""


class PublicAssetsUnconfirmed(ReleaseError):
    """The exact release is public, but its asset verification is incomplete."""


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def positive(value):
    return type(value) is int and value > 0


def oid(value):
    return isinstance(value, str) and OID.fullmatch(value) is not None


def official(repository):
    return (repository.get('id') == REPOSITORY_ID
            and repository.get('full_name') == REPOSITORY)


def completed(job, name):
    return (job.get('name') == name and job.get('status') == 'completed'
            and job.get('conclusion') == 'success')


def exactly_one(rows, name, message):
    matches = [row for row in rows if row.get('name') == name]
    require(len(matches) == 1, message)
    return matches[0]


class GitHub:
    get = staticmethod(get_json)
    jobs = staticmethod(get_jobs)

    @staticmethod
    def job_log(job_id):
        command = ['gh', 'api', '--method', 'GET', '--allow-escape-sequences',
                   '--', f'{API}/actions/jobs/{job_id}/logs']
        try:
            response = subprocess.run(command, capture_output=True, check=True, timeout=60)
            return response.stdout.decode('utf-8', errors='replace')
        except (OSError, subprocess.SubprocessError):
            raise ReleaseError('Could not authenticate the prior publisher job log') from None

    @staticmethod
    def mutate(method, endpoint, *, body=None, file=None):
        command = ['gh', 'api', '--method', method, '--header', 'Accept: application/vnd.github+json']
        if file is None:
            command += ['--header', 'Content-Type: application/json', '--input', '-']
            request = json.dumps(body).encode('utf-8')
        else:
            command += ['--header', 'Content-Type: application/octet-stream', '--input', str(file)]
            request = None
        try:
            response = subprocess.run(command + ['--', endpoint], input=request,
                                      capture_output=True, check=True, timeout=150)
            return json.loads(response.stdout)
        except (OSError, subprocess.SubprocessError, ValueError):
            # Never blindly repeat POST/PATCH. Do not echo response bodies,
            # credentials, upload URLs, or release notes into the issuer log.
            raise MutationUnconfirmed(f'GitHub {method} response did not confirm the operation') from None


@dataclass(frozen=True)
class Source:
    tag: str
    head: str
    run: int
    attempt: int
    suite: int

    @property
    def version(self):
        return release_version(self.tag)

    @classmethod
    def from_run(cls, run):
        require(run['workflow_id'] == CI_ID and run['event'] == 'push' and completed(run, 'ci')
                and official(run['repository']) and official(run['head_repository'])
                and oid(run['head_sha']) and positive(run['id']) and positive(run['run_attempt'])
                and positive(run['check_suite_id']),
                'The source is not a successful canonical CI push attempt')
        source = cls(run['head_branch'], run['head_sha'], run['id'],
                     run['run_attempt'], run['check_suite_id'])
        release_version(source.tag)
        # REST may append the workflow ref. It must identify this exact tag.
        require(run['path'] in (CI_PATH, f'{CI_PATH}@{source.tag}', f'{CI_PATH}@refs/tags/{source.tag}'),
                'The source workflow path does not identify canonical tag CI')
        return source

    def confirm_current(self, api):
        live = api.get(f'{API}/actions/runs/{self.run}')
        require(Source.from_run(live) == self, 'CI attempt, repository, tag or tested source changed')


def ref_pointer(api, ref):
    result = api.get(f'{API}/git/ref/{ref}')
    value = result['object']
    require(result['ref'] == f'refs/{ref}' and oid(value['sha'])
            and value['type'] in ('commit', 'tag'), 'Invalid or missing exact Git reference')
    return value['type'], value['sha']


def release_pointer(api, source):
    pointer = ref_pointer(api, f'tags/{source.tag}')
    kind, sha = pointer
    for _ in range(5):
        if kind == 'commit':
            require(sha == source.head, 'The release tag no longer points to the tested commit')
            return pointer
        tag = api.get(f'{API}/git/tags/{sha}')
        value = tag['object']
        require(tag['sha'] == sha and oid(value['sha']) and value['type'] in ('commit', 'tag'),
                'Invalid annotated release tag object')
        kind, sha = value['type'], value['sha']
    raise ReleaseError('Release tag nesting is too deep')


def confirm_pointer(api, source, pointer):
    require(ref_pointer(api, f'tags/{source.tag}') == pointer,
            'Release tag changed since CI provenance was verified')


def confirm_main_ancestor(api, head):
    kind, main = ref_pointer(api, 'heads/main')
    require(kind == 'commit', 'Protected main did not resolve to a commit')
    comparison = api.get(f'{API}/compare/{head}...{main}')
    require(comparison['status'] in ('ahead', 'identical')
            and comparison['base_commit']['sha'] == head
            and comparison['merge_base_commit']['sha'] == head,
            'The tested commit is outside current protected main history')


def confirm_ci_workflow(api, source, trusted_ci_blob):
    definition = api.get(f'{API}/contents/{CI_PATH}?ref={source.head}')
    require(definition['type'] == 'file' and definition['path'] == CI_PATH
            and definition['sha'] == trusted_ci_blob,
            'Tested CI definition differs from the protected publisher snapshot')


def job_identity(job, run, attempt, head):
    return (positive(job['id']) and job['run_id'] == run
            and job['run_attempt'] == attempt and job['head_sha'] == head)


def confirm_producer(api, source):
    jobs = api.jobs(f'{API}/actions/runs/{source.run}/attempts/{source.attempt}/jobs')['jobs']
    verified = {}
    for name in ('source-identity', 'ci-required', 'package'):
        job = exactly_one(jobs, name, f'Missing or ambiguous {name} job')
        require(job_identity(job, source.run, source.attempt, source.head) and completed(job, name),
                f'{name} is not a successful job in the exact CI attempt')
        verified[name] = job
    step = exactly_one(verified['source-identity']['steps'], RELEASE_STEP, 'No exact version-tag push evidence')
    require(completed(step, RELEASE_STEP), 'Source CI did not validate an actual pushed tag')
    job = verified['package']
    upload = exactly_one(job['steps'], UPLOAD_STEP, 'Missing or ambiguous installer upload step')
    require(completed(upload, UPLOAD_STEP), 'Exact package job did not upload its tested installer')

    inventory = api.get(f'{API}/actions/runs/{source.run}/artifacts?per_page=100')
    require(inventory['total_count'] == len(inventory['artifacts']), 'Incomplete artifact inventory')
    artifact = exactly_one(inventory['artifacts'], ARTIFACT_NAME, 'Missing or ambiguous installer artifact')
    origin = artifact['workflow_run']
    require(positive(artifact['id']) and positive(artifact['size_in_bytes'])
            and artifact['size_in_bytes'] <= MAX_INSTALLER_SIZE
            and artifact['expired'] is False and DIGEST.fullmatch(artifact['digest'] or '')
            and origin['id'] == source.run and origin['repository_id'] == REPOSITORY_ID
            and origin['head_repository_id'] == REPOSITORY_ID
            and origin['head_branch'] == source.tag and origin['head_sha'] == source.head
            and timestamp(job['started_at']) <= timestamp(artifact['created_at']) <= timestamp(job['completed_at']),
            'Installer artifact does not belong to the successful exact source and package job')
    return job['id'], artifact['id'], artifact['digest'].lower()


@dataclass(frozen=True)
class Asset:
    path: Path
    size: int
    digest: str

    @property
    def name(self):
        return self.path.name

    @classmethod
    def read(cls, path, limit):
        require(path.is_file() and not path.is_symlink(), 'The installer payload is not an ordinary file')
        size = path.stat().st_size
        require(0 < size <= limit, 'Invalid installer payload size')
        with path.open('rb') as stream:
            return cls(path, size, 'sha256:' + hashlib.file_digest(stream, 'sha256').hexdigest())

    def identity(self):
        return {'name': self.name, 'size': self.size, 'digest': self.digest}


def read_payload(directory, source):
    name = installer_name(source.version)
    require({path.name for path in directory.iterdir()} == {name, CHECKSUM_NAME},
            'The verified archive must contain exactly the versioned installer and checksum')
    installer = Asset.read(directory / name, MAX_INSTALLER_SIZE)
    checksum = Asset.read(directory / CHECKSUM_NAME, 512)
    line = f'{installer.digest.removeprefix("sha256:")}  {name}'.encode('ascii')
    require(checksum.path.read_bytes() in (line + b'\r\n', line + b'\n'),
            'SHA256SUMS.txt does not match the tested installer')
    return installer, checksum


def download_artifact(artifact_id, digest, destination):
    verifier = Path(__file__).resolve().parents[1] / 'surface/download-verified-artifact.py'
    try:
        subprocess.run([sys.executable, str(verifier), '--id', str(artifact_id), '--digest', digest,
                        '--destination', str(destination)], check=True, timeout=360)
    except (OSError, subprocess.SubprocessError):
        raise ReleaseError('Could not download and verify the exact installer archive') from None


@dataclass(frozen=True)
class Issuer:
    workflow: int
    run: int
    attempt: int
    head: str

    def validate(self, api):
        run = api.get(f'{API}/actions/runs/{self.run}/attempts/{self.attempt}')
        require(run['id'] == self.run and run['run_attempt'] == self.attempt
                and run['workflow_id'] == self.workflow
                and run['path'] in (PUBLISH_PATH, f'{PUBLISH_PATH}@main', f'{PUBLISH_PATH}@refs/heads/main')
                and run['name'] == PUBLISH_NAME and run['event'] == 'workflow_run'
                and run['head_branch'] == 'main' and run['head_sha'] == self.head
                and official(run['repository']) and official(run['head_repository']),
                'Draft issuer was not the canonical protected publisher')
        return run

    def prove_draft(self, api, claim, release_id):
        prior = self.validate(api)
        require(prior['status'] == 'completed', 'Prior publisher has not completed')
        confirm_main_ancestor(api, self.head)
        jobs = api.jobs(f'{API}/actions/runs/{self.run}/attempts/{self.attempt}/jobs')['jobs']
        job = exactly_one(jobs, 'publish', 'Missing or ambiguous prior publisher job')
        require(job_identity(job, self.run, self.attempt, self.head) and job['status'] == 'completed'
                and exactly_one(job['steps'], PUBLISH_STEP, 'Missing prior publisher step')['status'] == 'completed',
                'Could not authenticate the prior publisher job')
        evidence = f'GESHER_RELEASE_DRAFT id={release_id} claim={claim}'
        log = api.job_log(job['id'])
        require(sum(LOG_PREFIX.sub('', line) == evidence for line in log.splitlines()) == 1,
                'Existing release has no exact authenticated publisher receipt')


@dataclass(frozen=True)
class Claim:
    issuer: Issuer
    nonce: str

    def marker(self, provenance):
        return MARKER + json.dumps({
            'provenance': provenance,
            'issuer': vars(self.issuer),
            'claim': self.nonce,
        }, sort_keys=True, separators=(',', ':')) + ' -->'

    @classmethod
    def from_release(cls, release, provenance, current):
        body = release.get('body')
        lines = [line for line in body.splitlines() if line.startswith(MARKER)] if isinstance(body, str) else []
        require(len(lines) == 1 and lines[0].endswith(' -->'),
                'Existing release has no unambiguous Gesher publisher marker')
        try:
            content = json.loads(lines[0][len(MARKER):-4])
            issuer = Issuer(**content['issuer'])
            nonce = content['claim']
        except (KeyError, TypeError, ValueError):
            raise ReleaseError('Existing release has an invalid publisher marker') from None
        require(issuer.workflow == current.workflow and positive(issuer.run) and positive(issuer.attempt)
                and oid(issuer.head) and isinstance(nonce, str) and NONCE.fullmatch(nonce)
                and cls(issuer, nonce).marker(provenance) == lines[0],
                'Existing release does not match the exact tested provenance')
        return cls(issuer, nonce)


def confirm_release(release, source, marker, expected_id=None):
    author = release['author']
    markers = [line for line in release['body'].splitlines() if line.startswith(MARKER)]
    require(positive(release['id']) and (expected_id is None or release['id'] == expected_id)
            and release['tag_name'] == source.tag and release['prerelease'] is False
            and type(release['draft']) is bool and release['name'] == f'Gesher {version_string(source.version)}'
            and all(author.get(key) == value for key, value in BOT.items())
            and markers == [marker],
            'Release identity, ownership, or provenance changed')


def asset_identities(release, assets, pending=False):
    expected = {asset.name: asset for asset in assets}
    found, seen = {}, set()
    for row in release['assets']:
        name = row['name']
        require(name in expected and name not in seen and positive(row['id']),
                'Release has an unexpected or repeated asset')
        seen.add(name)
        asset = expected[name]
        if pending and row['state'] in ('starter', 'uploaded') and not row.get('digest'):
            require(row['size'] in (0, asset.size), 'Pending release asset size does not match')
            continue
        require(row['state'] == 'uploaded' and row['size'] == asset.size and row['digest'] == asset.digest,
                'A release asset does not match its tested bytes')
        if not release['draft']:
            download_url = f"https://github.com/{REPOSITORY}/releases/download/{release['tag_name']}/{name}"
            require(row['browser_download_url'] == download_url, 'Published release asset URL is not canonical')
        found[name] = row['id']
    require(len(found) == len(set(found.values())), 'Release asset identities are not unique')
    return found


def matching_release(catalog, source):
    matches = [row for row in catalog if stable_version(row.get('tag_name')) == source.version]
    require(len(matches) <= 1, 'Multiple releases or drafts use this version')
    return matches[0] if matches else None


def require_newer(catalog, source):
    require(not any(not row['draft'] and (version := stable_version(row.get('tag_name'))) is not None
                    and version >= source.version for row in catalog),
            'A published release already has this version or a newer stable version number')


def visible_upload(api, endpoint, source, marker, release_id, assets, name, known):
    for attempt in range(6):
        try:
            release = api.get(endpoint)
        except GitHubReadError:
            if attempt == 5:
                raise
        else:
            confirm_release(release, source, marker, release_id)
            require(release['draft'], 'Release changed while uploading assets')
            found = asset_identities(release, assets, pending=True)
            require(all(found.get(name) == identity for name, identity in known.items()),
                    'A previously verified asset changed')
            if name in found:
                return found
        if attempt < 5:
            time.sleep(5)
    raise ReleaseError('Asset upload could not be confirmed; the owned draft may still be incomplete')


def confirmed_public(release, source, marker, release_id, assets, expected):
    confirm_release(release, source, marker, release_id)
    if release['draft']:
        return False
    try:
        require(release.get('published_at') and timestamp(release['published_at'])
                and release.get('html_url') == f'https://github.com/{REPOSITORY}/releases/tag/{source.tag}'
                and asset_identities(release, assets) == expected,
                'Published release metadata differs from the verified draft')
    except (KeyError, TypeError, ValueError, ReleaseError):
        raise PublicAssetsUnconfirmed(f'Release {release_id} is public but its verified assets could not be confirmed') from None
    return True


def confirm_publication(api, response, endpoint, source, marker, release_id, assets, expected):
    public_error = None
    if isinstance(response, dict):
        try:
            if confirmed_public(response, source, marker, release_id, assets, expected):
                return
        except PublicAssetsUnconfirmed as error:
            public_error = error
        except (KeyError, TypeError, ValueError, ReleaseError):
            pass
    for attempt in range(5):
        try:
            if confirmed_public(api.get(endpoint), source, marker, release_id, assets, expected):
                return
        except PublicAssetsUnconfirmed as error:
            public_error = error
        except (GitHubReadError, KeyError, TypeError, ValueError, ReleaseError):
            pass
        if attempt < 4:
            time.sleep(3)
    if public_error:
        raise public_error
    raise ReleaseError(f'Publication outcome uncertain after PATCH for release {release_id}; inspect the exact release')


def publish(api, source, issuer, trusted_ci_blob, directory, download=download_artifact):
    source.confirm_current(api)
    pointer = release_pointer(api, source)
    confirm_main_ancestor(api, source.head)
    confirm_ci_workflow(api, source, trusted_ci_blob)
    package, artifact, digest = confirm_producer(api, source)
    download(artifact, digest, directory)
    assets = read_payload(directory, source)
    provenance = {
        'repository': REPOSITORY_ID, 'tag': source.tag, 'head': source.head,
        'tag_object': pointer[1], 'ci_workflow': CI_ID, 'run': source.run, 'attempt': source.attempt,
        'package_job': package, 'artifact': artifact, 'artifact_digest': digest,
        'assets': [asset.identity() for asset in assets],
    }

    catalog = list(release_catalog(api.get))
    release = matching_release(catalog, source)
    if release is None:
        require_newer(catalog, source)
        claim = Claim(issuer, secrets.token_hex(32))
        marker = claim.marker(provenance)
        confirm_pointer(api, source, pointer)
        release = api.mutate('POST', f'{API}/releases', body={
            'tag_name': source.tag, 'target_commitish': source.head,
            'name': f'Gesher {version_string(source.version)}', 'draft': True,
            'prerelease': False, 'generate_release_notes': True, 'body': marker,
        })
        confirm_release(release, source, marker)
        require(release['draft'] and not release['assets'], 'Create did not return a new empty draft')
        print(f'GESHER_RELEASE_DRAFT id={release["id"]} claim={claim.nonce}', flush=True)
    else:
        claim = Claim.from_release(release, provenance, issuer)
        marker = claim.marker(provenance)
        confirm_release(release, source, marker)
        claim.issuer.prove_draft(api, claim.nonce, release['id'])
        if not release['draft']:
            known = asset_identities(release, assets)
            require(len(known) == len(assets), 'Published release is missing a verified asset')
            confirmed_public(release, source, marker, release['id'], assets, known)
            confirm_pointer(api, source, pointer)
            return f'Publication already confirmed for {source.tag} (release {release["id"]})'
        require_newer(catalog, source)

    release_id = release['id']
    endpoint = f'{API}/releases/{release_id}'
    known = asset_identities(release, assets)
    for asset in assets:
        if asset.name in known:
            continue
        try:
            upload = f'https://uploads.github.com/{endpoint}/assets?name={quote(asset.name)}'
            api.mutate('POST', upload, file=asset.path)
        except MutationUnconfirmed:
            pass
        known = visible_upload(api, endpoint, source, marker, release_id, assets, asset.name, known)

    source.confirm_current(api)
    confirm_main_ancestor(api, source.head)
    catalog = list(release_catalog(api.get))
    draft = matching_release(catalog, source)
    require(draft is not None, 'Owned draft disappeared before publication')
    confirm_release(draft, source, marker, release_id)
    require(draft['draft'] and asset_identities(draft, assets) == known and len(known) == len(assets),
            'Owned draft or verified assets changed before publication')
    require_newer(catalog, source)
    confirm_pointer(api, source, pointer)
    try:
        response = api.mutate('PATCH', endpoint, body={'draft': False, 'make_latest': 'true'})
    except MutationUnconfirmed:
        response = None
    confirm_publication(api, response, endpoint, source, marker, release_id, assets, known)
    try:
        confirm_pointer(api, source, pointer)
    except GitHubReadError:
        return (f'Publication confirmed for {source.tag} (release {release_id}); '
                'a post-publication tag read was unavailable')
    except (KeyError, TypeError, ValueError, ReleaseError):
        raise ReleaseError(f'Release {release_id} was published, but its tag could not be confirmed afterward') from None
    return f'Publication confirmed for {source.tag} (release {release_id})'


def main(environ=os.environ):
    require(environ.get('GITHUB_ACTIONS') == 'true'
            and environ.get('GITHUB_EVENT_NAME') == 'workflow_run'
            and environ.get('GITHUB_WORKFLOW_REF') == PUBLISH_REF
            and environ.get('GITHUB_REPOSITORY') == REPOSITORY
            and environ.get('GITHUB_REPOSITORY_ID') == str(REPOSITORY_ID)
            and environ.get('GITHUB_REF') == 'refs/heads/main',
            'Publisher must run as a protected main workflow_run')
    event = json.loads(Path(environ['GITHUB_EVENT_PATH']).read_text(encoding='utf-8'))
    require(event['action'] == 'completed' and official(event['repository'])
            and event['repository']['default_branch'] == 'main'
            and event['workflow']['id'] == CI_ID and event['workflow']['path'] == CI_PATH,
            'The event is not a canonical CI completion')
    source = Source.from_run(event['workflow_run'])
    head, ci_blob = subprocess.check_output(['git', 'rev-parse', 'HEAD', f'HEAD:{CI_PATH}'], text=True).splitlines()
    require(oid(head) and oid(ci_blob) and head == environ['GITHUB_WORKFLOW_SHA'],
            'Publisher did not check out its exact protected workflow revision')
    api = GitHub()
    publication_workflow = api.get(f'{API}/actions/workflows/publish-release.yml')
    require(publication_workflow['path'] == PUBLISH_PATH and positive(publication_workflow['id']),
            'Canonical publisher workflow identity is unavailable')
    issuer = Issuer(publication_workflow['id'], int(environ['GITHUB_RUN_ID']),
                    int(environ['GITHUB_RUN_ATTEMPT']), head)
    require(positive(issuer.run) and positive(issuer.attempt), 'Invalid publisher job identity')
    issuer.validate(api)
    with tempfile.TemporaryDirectory(prefix='gesher-release-') as temp:
        message = publish(api, source, issuer, ci_blob, Path(temp) / 'installer')
    print(message)
    with open(environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as summary:
        print(message, file=summary)


if __name__ == '__main__':
    try:
        main()
    except (ReleaseError, GitHubReadError, OSError, subprocess.SubprocessError, KeyError, TypeError, ValueError) as error:
        # Remote fields, arbitrary archive names and log text are never emitted.
        message = str(error) if isinstance(error, ReleaseError) else 'Could not verify GitHub publication state'
        sys.exit(message)
