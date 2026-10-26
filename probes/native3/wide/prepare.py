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
    "stdio/parsefloat.c", "bionic/wcstod.cpp",
]
parts = [(root / "reference-prefix.h").read_text()]


def source_part(file):
    source = (root / "upstream" / file).read_text()
    # Only environment directives are adapted; no function body rewriting.
    source = re.sub(r'^\s*#include[^\n]*$', '', source, flags=re.M)
    source = source.replace('#pragma once', '')
    if file == "stdio/parsefloat.c":
        # Compile only the original narrow parser used by wcstod. Wide scanf
        # and its separate wparsefloat implementation remain outside scope.
        source = source.split("size_t wparsefloat(", 1)[0]
    return f'\n#line 1 "{file}"\n{source}'


parts.extend(source_part(file) for file in files)
# The pinned bionic wchar_l.cpp delegates comparisons to this exact LLVM-libc
# implementation. Compile its unchanged bodies under both target wchar types.
parts.append("""
#define LLVM_LIBC_FUNCTION(R, N, A) R N A
#define LIBC_CRASH_ON_NULLPTR(P) do { if ((P) == nullptr) abort(); } while (false)
""")
for variant in ("signed", "unsigned"):
    parts.append(f"#define LIBC_NAMESPACE_DECL llvm_{variant}")
    if variant == "unsigned":
        parts.append("#define wchar_t uint32_t")
    parts.extend(source_part(f"llvm-libc/src/wchar/{name}.cpp")
                 for name in ("wcscmp", "wcsncmp", "wmemcmp"))
    parts.append("#undef LIBC_NAMESPACE_DECL")
    if variant == "unsigned":
        parts.append("#undef wchar_t")
parts.append((root / "reference-main.cpp").read_text())
Path(sys.argv[1]).write_text("\n".join(parts))
