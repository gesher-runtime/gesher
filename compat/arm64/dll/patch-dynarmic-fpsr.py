#!/usr/bin/env python3
"""Hash-check and patch the pinned x64 compiler's guest-MXCSR boundary.

Only source rewriting/hashing runs locally. Compilation and all native
execution belong to CI. Preserve the original dynarmic 0BSD source/license.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import sys

PIN = "9d4582339990d4eae53f1dc7160686920fc2075c"
RELATIVE = "src/dynarmic/backend/x64/a64_interface.cpp"
ORIGINAL_SHA256 = "4757e74b518304a2e996ac98f80bed3483319ac6463b55bae29e05f984bc01ec"
MARKER = "Gesher FPSR: compilation is not guest arithmetic."
OLD_BLOCK = """        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        constexpr size_t MINIMUM_REMAINING_CODESIZE"""
NEW_BLOCK = """        if (auto block = emitter.GetBasicBlock(current_location))
            return block->entrypoint;

        // Gesher FPSR: compilation is not guest arithmetic. Dispatcher cache
        // misses can enter here with live guest MXCSR. Keep compiler math
        // independent of guest rounding and restore every saved status bit.
        const unsigned compiler_mxcsr = _mm_getcsr();
        SCOPE_EXIT { _mm_setcsr(compiler_mxcsr); };
        _mm_setcsr(0x1f80);

        constexpr size_t MINIMUM_REMAINING_CODESIZE"""
REPLACEMENTS = [("#include <mutex>\n", "#include <mutex>\n#include <xmmintrin.h>\n"),
                (OLD_BLOCK, NEW_BLOCK)]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def transform(data):
    text = data.decode("utf-8")
    if MARKER in text:
        original = text
        for old, new in reversed(REPLACEMENTS):
            if original.count(new) != 1:
                raise ValueError("partial or changed compiler patch")
            original = original.replace(new, old, 1)
        if digest(original.encode()) != ORIGINAL_SHA256:
            raise ValueError("patched source does not reconstruct the pinned original")
        return data
    if digest(data) != ORIGINAL_SHA256:
        raise ValueError("compiler source is not the exact pinned original")
    for old, new in REPLACEMENTS:
        if text.count(old) != 1:
            raise ValueError("pinned compiler patch anchor changed")
        text = text.replace(old, new, 1)
    return text.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", nargs="?")
    parser.add_argument("--verify-input-json", action="store_true")
    args = parser.parse_args()
    if args.verify_input_json:
        item = json.load(sys.stdin)
        if isinstance(item, list):
            if len(item) != 1:
                raise ValueError("expected the single pinned compiler source")
            item = item[0]
        data = base64.b64decode(item["base64"])
        patched = transform(data)
        assert transform(patched) == patched
        print("PASS source-only compiler patch provenance, anchors and idempotence", digest(patched))
        return
    if not args.source_root:
        parser.error("source_root is required")
    path = Path(args.source_root) / RELATIVE
    data = path.read_bytes()
    patched = transform(data)
    path.write_bytes(patched)
    print("PINNED_DYNARMIC", PIN, RELATIVE, ORIGINAL_SHA256)
    print("PASS dynarmic compiler MXCSR boundary patch", digest(patched))


if __name__ == "__main__":
    main()
