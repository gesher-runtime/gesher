#!/usr/bin/env python3
"""Fetch or verify the exact unmodified AOSP files used by production CMake."""
import argparse
import base64
import hashlib
import http.client
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.request


DOWNLOAD_BACKOFF = (1, 2, 4)


def download(url):
    """Retry temporary transport failures; never accept a partial response."""
    for attempt in range(len(DOWNLOAD_BACKOFF) + 1):
        try:
            with urllib.request.urlopen(url, timeout=45) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            error.close()
            if error.code not in (500, 502, 503, 504):
                raise
            failure = error
        except (urllib.error.URLError, TimeoutError, ConnectionError,
                http.client.IncompleteRead) as error:
            failure = error
        if attempt == len(DOWNLOAD_BACKOFF):
            raise RuntimeError(
                f'pinned source download failed after {attempt + 1} attempts: {url}') from failure
        delay = DOWNLOAD_BACKOFF[attempt]
        print(f'PINNED_SOURCE_RETRY attempt={attempt + 1}/4 delay={delay}s '
              f'url={url} error={failure}', file=sys.stderr, flush=True)
        time.sleep(delay)
    raise AssertionError('unreachable')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('aosp_dir', type=Path)
    parser.add_argument('--fetch', action='store_true', help='download missing pinned files')
    args = parser.parse_args()
    manifest = json.loads(Path(__file__).with_name('sources.json').read_text())
    for source, digest in manifest['files'].items():
        destination = args.aosp_dir / 'bionic' / source
        if not destination.exists() and args.fetch:
            url = f"{manifest['repository']}/+/{manifest['commit']}/{source}?format=TEXT"
            # Decoding and digest verification are outside the transport retry:
            # a successful response with changed content must fail immediately.
            data = base64.b64decode(download(url), validate=True)
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit(f'upstream source digest mismatch: {source}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        if not destination.is_file() or hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'missing or changed pinned source: {source}')
    print(f"Verified {len(manifest['files'])} gdtoa source files at {manifest['commit']}")


if __name__ == '__main__':
    main()
