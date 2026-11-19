"""Compute installer versions from canonical push tags or published releases."""
from datetime import datetime
import json
import os
import re
import urllib.request

from github_read import get_json


REPOSITORY = 'gesher-runtime/gesher'
REPOSITORY_ID = 1426153186
API = f'repos/{REPOSITORY}'
DIGEST = re.compile(r'sha256:[0-9a-fA-F]{64}')
MAX_INSTALLER_SIZE = 2147483648
STABLE = re.compile(r'v?(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})')


def stable_version(tag):
    match = STABLE.fullmatch(tag) if isinstance(tag, str) else None
    if match:
        parts = tuple(map(int, match.groups()))
        if max(parts) <= 65535:
            return parts
    return None


def version_string(parts):
    return '.'.join(map(str, parts))


def release_version(tag):
    parts = stable_version(tag)
    if parts is None or not tag.startswith('v') or parts < (0, 2, 0):
        raise ValueError('A release tag must be vX.Y.Z, at least v0.2.0, with components through 65535')
    return parts


def installer_name(parts):
    return f'gesher-setup-{version_string(parts)}.exe'


def timestamp(value):
    result = datetime.fromisoformat(value)
    if result.tzinfo is None:
        raise ValueError('GitHub timestamp has no UTC offset')
    return result


def release_catalog(read):
    seen = set()
    for page in range(1, 101):
        rows = read(f'{API}/releases?per_page=100&page={page}')
        if not isinstance(rows, list) or len(rows) > 100:
            raise ValueError('Invalid release catalog page')
        for row in rows:
            identity = row['id']
            if (type(identity) is not int or identity < 1 or identity in seen
                    or type(row['draft']) is not bool or type(row['prerelease']) is not bool):
                raise ValueError('Invalid or repeated release catalog identity')
            seen.add(identity)
            yield row
        if len(rows) < 100:
            return
    raise ValueError('Incomplete release catalog')


def updater_version(release):
    if release['draft'] or release['prerelease'] or not release.get('published_at'):
        return None
    tag = release.get('tag_name')
    parts = stable_version(tag)
    if parts is None:
        return None
    timestamp(release['published_at'])
    name = installer_name(parts)
    if release.get('html_url') != f'https://github.com/{REPOSITORY}/releases/tag/{tag}':
        return None
    assets = [asset for asset in release['assets'] if asset.get('name') == name]
    if len(assets) != 1:
        return None
    asset = assets[0]
    size = asset.get('size')
    if (asset.get('state') == 'uploaded' and type(size) is int and 0 < size <= MAX_INSTALLER_SIZE
            and DIGEST.fullmatch(asset.get('digest') or '')
            and asset.get('browser_download_url') == f'https://github.com/{REPOSITORY}/releases/download/{tag}/{name}'):
        return parts
    return None


def build_version(environ, read):
    official = (environ['GITHUB_REPOSITORY'] == REPOSITORY
                and environ['GITHUB_REPOSITORY_ID'] == str(REPOSITORY_ID))
    ref = environ['GITHUB_REF']
    if official and environ['GITHUB_EVENT_NAME'] == 'push' and ref.startswith('refs/tags/'):
        return version_string(release_version(ref.removeprefix('refs/tags/')))

    number = environ['GITHUB_RUN_NUMBER']
    if not re.fullmatch(r'[1-9][0-9]{0,4}', number) or int(number) > 65535:
        raise ValueError('CI run number must be between 1 and 65535')
    versions = (updater_version(release) for release in release_catalog(read))
    highest = max((parts for parts in versions if parts is not None), default=(0, 0, 0))
    return f'{version_string(highest)}.{number}'


def read_public(endpoint):
    # A token in a standalone fork belongs to that fork. Its public baseline
    # is still the upstream release feed used by installed Gesher clients.
    request = urllib.request.Request(f'https://api.github.com/{endpoint}', headers={
        'Accept': 'application/vnd.github+json',
        'User-Agent': 'Gesher-CI',
        'X-GitHub-Api-Version': '2022-11-28',
    })
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


if __name__ == '__main__':
    read = get_json if os.environ.get('GITHUB_REPOSITORY') == REPOSITORY else read_public
    print(build_version(os.environ, read))
