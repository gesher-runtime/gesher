#!/usr/bin/env python3
"""Verify the full licensed pinned sources; no local native compilation."""
import base64
import hashlib
import json
from pathlib import Path
import sys
import urllib.request

root = Path(__file__).resolve().parent
manifest = json.loads((root / 'sources.json').read_text())
for source in manifest['sources']:
    data = (root / source['path']).read_bytes()
    assert hashlib.sha256(data).hexdigest() == source['sha256'], source['path']
    if '--verify-upstream' in sys.argv:
        upstream = base64.b64decode(urllib.request.urlopen(source['url'], timeout=60).read())
        assert upstream == data, source['path']
    print('PINNED_ARM_FPCR', source['source'], source['sha256'])
print('PASS pinned ARM FPCR: complete licensed source and header digests')
