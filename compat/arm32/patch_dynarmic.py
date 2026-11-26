#!/usr/bin/env python3
"""Expose A32 exclusive-read callbacks in the pinned Dynarmic x64 backend.

Ordinary unaligned reads must remain valid, whereas the ARM32 executor must
check exclusive-load alignment. The callback path therefore needs distinct
entry points. Fastmem exclusive accesses bypass these callbacks and must stay
disabled in the executor. A64 and shared backend sources are not modified.

Only exact original files, or this exact patch of them, are accepted. Both
files are validated before either is written; rerunning is harmless. Original
Dynarmic copyright and 0BSD notices remain in the modified source files.
"""

import argparse
import hashlib
import os
from pathlib import Path
import stat
import tempfile


PIN = "9d4582339990d4eae53f1dc7160686920fc2075c"
CONFIG = "src/dynarmic/interface/A32/config.h"
EMITTER = "src/dynarmic/backend/x64/a32_emit_x64_memory.cpp"
ORIGINAL_SHA256 = {
    CONFIG: "a3b54e06be8f22f13271d47fb5e49d1573f6236f08080d18dbdcb1f0ea01c658",
    EMITTER: "f81286ac2eceda8da8b3d8b3b2ebbfb8456f8ce67b240d81c89536b13de4baa5",
}

_CONFIG_ANCHOR = "    // Writes through these callbacks may not be aligned.\n    virtual void MemoryWrite8"
_EXCLUSIVE_CALLBACKS = """    // Gesher A32: exclusive loads have a distinct alignment contract.
    // Defaults preserve existing clients; the executor overrides these with
    // alignment checks. Fastmem exclusive accesses bypass these callbacks.
    virtual std::uint8_t MemoryReadExclusive8(VAddr vaddr) { return MemoryRead8(vaddr); }
    virtual std::uint16_t MemoryReadExclusive16(VAddr vaddr) { return MemoryRead16(vaddr); }
    virtual std::uint32_t MemoryReadExclusive32(VAddr vaddr) { return MemoryRead32(vaddr); }
    virtual std::uint64_t MemoryReadExclusive64(VAddr vaddr) { return MemoryRead64(vaddr); }

"""
REPLACEMENTS = {
    CONFIG: [(_CONFIG_ANCHOR, _EXCLUSIVE_CALLBACKS + _CONFIG_ANCHOR)],
    EMITTER: [
        (
            f"EmitExclusiveReadMemory{suffix}<{bits}, &A32::UserCallbacks::MemoryRead{bits}>",
            f"EmitExclusiveReadMemory{suffix}<{bits}, &A32::UserCallbacks::MemoryReadExclusive{bits}>",
        )
        for bits in (8, 16, 32, 64)
        for suffix in ("", "Inline")
    ],
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def transform(data: bytes, relative: str) -> bytes:
    """Verify the complete file, including on an already-patched checkout."""
    expected = ORIGINAL_SHA256[relative]
    replacements = [(old.encode(), new.encode()) for old, new in REPLACEMENTS[relative]]
    if digest(data) == expected:
        result = data
        for old, new in replacements:
            if result.count(old) != 1:
                raise ValueError(f"{relative}: pinned patch anchor is not unique")
            result = result.replace(old, new, 1)
        return result

    original = data
    for old, new in reversed(replacements):
        if original.count(new) != 1:
            raise ValueError(f"{relative}: not the pinned original or complete Gesher patch")
        original = original.replace(new, old, 1)
    if digest(original) != expected:
        raise ValueError(f"{relative}: patched file does not reconstruct the pinned original")
    return data


def patch_tree(root: Path, check_only: bool = False) -> list[str]:
    # Plan all changes first. A hash error in the second file must not leave
    # the first one patched. Original and patched files may coexist after an
    # interrupted run; each file must still reconstruct its exact original.
    plan = []
    for relative in ORIGINAL_SHA256:
        path = root / relative
        mode = path.lstat().st_mode
        if not stat.S_ISREG(mode):
            raise ValueError(f"{relative}: expected a regular file, not a symlink")
        original = path.read_bytes()
        patched = transform(original, relative)
        if patched != original:
            plan.append((relative, path, stat.S_IMODE(mode), patched))
    if check_only:
        if plan:
            raise ValueError("source is not fully patched; run this script without --check first")
        return []

    staged = []
    try:
        for relative, path, mode, data in plan:
            descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.gesher-", dir=path.parent)
            temporary_path = Path(temporary)
            staged.append((temporary_path, path))
            with os.fdopen(descriptor, "wb") as output:
                output.write(data)
            os.chmod(temporary_path, mode)
        for temporary_path, path in staged:
            os.replace(temporary_path, path)
    finally:
        for temporary_path, _ in staged:
            temporary_path.unlink(missing_ok=True)
    return [relative for relative, _, _, _ in plan]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", type=Path)
    parser.add_argument("--check", action="store_true", help="require both files to be patched; never write")
    args = parser.parse_args()
    try:
        changed = patch_tree(args.source_root, args.check)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Dynarmic A32 patch rejected: {error}\n")
    state = "verified patched files; changes" if args.check else "patched files"
    print(f"Dynarmic {PIN}: {state}: {len(changed)}")


if __name__ == "__main__":
    main()
