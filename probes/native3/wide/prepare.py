"""CI-only include adapters for the exact checked-in bionic source reference.

The function bodies remain verbatim. The Linux host supplies size_t, wchar_t
(32-bit), integer widths and errno; the adapter supplies bionic's opaque
8-byte mbstate_t and its fixed ASCII ctype dependencies, rather than glibc's
different state and undefined out-of-range ctype table indexing.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent
manifest = json.loads((root / "sources.json").read_text())
for source, digest in manifest["sha256"].items():
    actual = hashlib.sha256((root / "upstream" / source).read_bytes()).hexdigest()
    if actual != digest:
        raise SystemExit(f"source digest mismatch: {source}")
    print("PINNED_SOURCE", source, digest)

files = [
    "private/bionic_mbstate.h",
    "bionic/mbrtoc32.cpp", "bionic/c32rtomb.cpp",
    "bionic/mbrtoc16.cpp", "bionic/c16rtomb.cpp", "bionic/wchar.cpp",
    "upstream-openbsd/lib/libc/locale/mbrlen.c", "bionic/mblen.cpp",
    "upstream-openbsd/lib/libc/locale/mbtowc.c",
    "upstream-openbsd/lib/libc/locale/wctomb.c",
    "upstream-openbsd/lib/libc/locale/mbstowcs.c",
    "upstream-openbsd/lib/libc/locale/wcstombs.c",
    "bionic/strtol.cpp",
]
parts = [(root / "reference-prefix.h").read_text()]
for file in files:
    source = (root / "upstream" / file).read_text()
    # Only environment directives are adapted; no function body rewriting.
    source = re.sub(r'^\s*#include[^\n]*$', '', source, flags=re.M)
    source = source.replace('#pragma once', '')
    parts.append(f'\n#line 1 "{file}"\n{source}')
parts.append((root / "reference-main.cpp").read_text())
Path(sys.argv[1]).write_text("\n".join(parts))
