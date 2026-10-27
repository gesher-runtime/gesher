#!/usr/bin/env python3
"""Check the separately frozen, pinned bionic character-width reference."""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
unsigned = len(sys.argv) > 2 and sys.argv[2] == '--unsigned'
reference = 'width-unsigned-reference.json' if unsigned else 'width-reference.json'
prefix = 'WIDTH_UNSIGNED' if unsigned else 'WIDTH'
expected = json.loads((root / reference).read_text(encoding='utf-8'))
if hashlib.sha256((root / 'Cargo.lock').read_bytes()).hexdigest() != expected['lock_sha256']:
    raise SystemExit('FAIL width reference dependency lock changed')
output = Path(sys.argv[1]).read_text(encoding='utf-8')
digests = re.findall(rf'^{prefix}_REFERENCE wcwidth ([a-f0-9]{{16}})$', output, re.M)
samples = [{'codepoint': int(c, 16), 'width': int(w)} for c, w in
           re.findall(rf'^{prefix}_SAMPLE ([a-f0-9]{{8}}) (-?[0-9]+)$', output, re.M)]
if digests != [expected['wcwidth']] or samples != expected['samples']:
    raise SystemExit('FAIL pinned width reference digest or boundary rows changed')
label = 'unsigned width' if unsigned else 'width'
print(f'PASS  pinned {label} reference: wcwidth {digests[0]}, 33 rows and exact Cargo lock')
