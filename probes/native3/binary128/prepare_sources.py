#!/usr/bin/env python3
"""Fetch or verify the exact unmodified AOSP files used by production CMake."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request


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
            with urllib.request.urlopen(url, timeout=45) as response:
                data = base64.b64decode(response.read(), validate=True)
            if hashlib.sha256(data).hexdigest() != digest:
                raise SystemExit(f'upstream source digest mismatch: {source}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        if not destination.is_file() or hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'missing or changed pinned source: {source}')
    print(f"Verified {len(manifest['files'])} gdtoa source files at {manifest['commit']}")


if __name__ == '__main__':
    main()
