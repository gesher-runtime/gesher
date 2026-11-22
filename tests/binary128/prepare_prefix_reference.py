#!/usr/bin/env python3
"""Expose the existing pinned Bionic parsefloat body for a grammar-only test."""
import hashlib
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
source = root / 'probes/native3/wide/upstream/stdio/parsefloat.c'
data = source.read_bytes()
if hashlib.sha256(data).hexdigest() != 'f5761e9123de860f34ff49135e374774e4e15a71cae794ffc9ddecf0b4e16ac1':
    raise SystemExit('pinned parsefloat source hash mismatch')
# Only the include environment is adapted; no function body is rewritten.
text = re.sub(r'^#include[^\n]*$', '', data.decode(), flags=re.M)
text = text.split('size_t wparsefloat(', 1)[0]
Path(sys.argv[1]).write_text(text)
