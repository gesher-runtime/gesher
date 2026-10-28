"""CI only: hash one exact GitHub artifact archive before isolated extraction."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

parser = argparse.ArgumentParser()
parser.add_argument('--id', required=True)
parser.add_argument('--digest', required=True)
parser.add_argument('--destination', required=True, type=Path)
a = parser.parse_args()
if os.environ.get('GITHUB_ACTIONS') != 'true' or not a.id.isdecimal() or not re.fullmatch(r'sha256:[0-9a-f]{64}', a.digest):
    raise RuntimeError('Invalid CI artifact request')
root = a.destination.resolve()
if root.exists() and any(root.iterdir()):
    raise RuntimeError('Artifact destination must be empty')
root.mkdir(parents=True, exist_ok=True)
api = f"https://api.github.com/repos/{os.environ['GITHUB_REPOSITORY']}/actions/artifacts/{a.id}/zip"
request = urllib.request.Request(api, headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'})
try:
    try:
        urllib.request.build_opener(NoRedirect).open(request, timeout=60)
        raise RuntimeError('Artifact API did not return a redirect')
    except urllib.error.HTTPError as response:
        if response.code != 302:
            raise RuntimeError(f'Artifact API returned HTTP {response.code}') from None
        location = response.headers.get('Location', '')
    parsed = urllib.parse.urlparse(location)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise RuntimeError('Unsafe artifact redirect')
    with tempfile.TemporaryDirectory(prefix='surface-artifact-') as temp:
        archive = Path(temp) / 'artifact.zip'
        digest = hashlib.sha256()
        # No authorization header is sent to the signed storage URL or its redirects.
        with urllib.request.urlopen(location, timeout=120) as response, archive.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if 'sha256:' + digest.hexdigest() != a.digest:
            raise RuntimeError('Artifact archive digest mismatch')
        with zipfile.ZipFile(archive) as package:
            seen = set()
            for member in package.infolist():
                name = member.filename
                path = PurePosixPath(name)
                if not name or '\\' in name or path.is_absolute() or any(part in ('', '.', '..') or re.search(r'[<>:"|?*]', part) or part.endswith((' ', '.')) or re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?', part) for part in path.parts):
                    raise RuntimeError('Unsafe artifact member path')
                if stat.S_ISLNK(member.external_attr >> 16):
                    raise RuntimeError('Artifact contains a link')
                key = name.rstrip('/').casefold()
                if key in seen:
                    raise RuntimeError('Artifact contains duplicate members')
                seen.add(key)
                if not root.joinpath(*path.parts).resolve().is_relative_to(root):
                    raise RuntimeError('Artifact member escapes destination')
            package.extractall(root)
except Exception as error:
    # Avoid exposing a signed URL or token in exception details.
    raise RuntimeError(f'Artifact {a.id} verification/extraction failed ({type(error).__name__})') from None
print(f'SURFACE_VERIFIED_ARCHIVE id={a.id} digest={a.digest} destination={root}', flush=True)

