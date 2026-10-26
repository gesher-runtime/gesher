#!/usr/bin/env python3
"""Check the exact pinned reference output before using it as an oracle."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
expected = json.loads((root / 'reference.json').read_text(encoding='utf-8'))
lock = (root / 'Cargo.lock').read_bytes()
if hashlib.sha256(lock).hexdigest() != expected['lock_sha256']:
    raise SystemExit('FAIL Unicode reference dependency lock changed')
output = Path(sys.argv[1]).read_text(encoding='utf-8')
digests = re.findall(r'^REFERENCE ([a-z]+) ([a-f0-9]{16})$', output, re.M)
if len(digests) != 14 or dict(digests) != expected['properties']:
    raise SystemExit('FAIL Unicode reference whole-code-point digests changed or are incomplete')
samples = [{'codepoint': int(cp, 16), 'values': json.loads(values)}
           for cp, values in re.findall(r'^SAMPLE ([a-f0-9]{8}) (\[[0-9, ]+\])$', output, re.M)]
if samples != expected['samples']:
    raise SystemExit('FAIL Unicode reference boundary rows changed or are incomplete')
for name, digest in digests:
    print(f'PASS  pinned Unicode reference: {name} {digest}')
print('PASS  pinned Unicode reference: 11 boundary rows and exact Cargo lock')
