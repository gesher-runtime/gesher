#!/usr/bin/env python3
"""Validate source inputs and actual independent QEMU matrix/provenance."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent


def vectors():
    text = (HERE / "vectors.h").read_text()
    body = text.split("ArmFpsrVectors[] = {", 1)[1].split("};", 1)[0]
    output = []
    for row in re.findall(r"\{([^{}]+)\}", body):
        fields = [re.sub(r"UINT(?:32|64)_C\(([^)]+)\)", r"\1", item.strip()) for item in row.split(",")]
        assert len(fields) == 4
        values = tuple(int(item, 0) for item in fields)
        assert all(0 <= item < (1 << (64 if index < 2 else 32)) for index, item in enumerate(values))
        output.append(values)
    assert len(output) == 32
    return output


def inputs():
    cases = vectors()
    for mode, dn, fz in itertools.product(range(4), range(2), range(2)):
        control = (mode << 22) | (dn << 25) | (fz << 24)
        for width, operation in itertools.product((32, 64), range(4)):
            for left64, right64, left32, right32 in cases:
                left, right = (left32, right32) if width == 32 else (left64, right64)
                for seed in (0, 0x08000081):
                    yield operation, width, control, left, right, seed


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", nargs="?")
    parser.add_argument("--qemu")
    parser.add_argument("--elf")
    parser.add_argument("--manifest")
    args = parser.parse_args()
    lock = json.loads((HERE / "qemu.lock.json").read_text())
    expected = list(inputs())
    assert len(expected) == lock["cases"]
    if not args.matrix:
        print("PASS source-only FPSR vector/schema checks", len(expected))
        return
    lines = Path(args.matrix).read_text().splitlines()
    assert lines[0] == "ARM64_FPSR_QEMU_V1"
    assert len(lines) == len(expected) + 1
    for number, (line, source) in enumerate(zip(lines[1:], expected), 1):
        fields = tuple(int(item, 16) for item in line.split())
        assert len(fields) == 8 and fields[:6] == source, (number, "input/schema mismatch")
        operation, width, control, left, right, seed, result, status = fields
        assert 0 <= result < (1 << width) and status & ~0x0800009f == 0
        one = 0x3f800000 if width == 32 else 0x3ff0000000000000
        half = 0x33800000 if width == 32 else 0x3ca0000000000000
        if left == one and right == one:
            two = 0x40000000 if width == 32 else 0x4000000000000000
            zero = (1 << (width - 1)) if (control >> 22) & 3 == 2 else 0
            assert result == (two, zero, one, one)[operation] and status == seed, (number, "normal arithmetic sanity")
        if left == one and right == half and operation in (2, 3):
            reciprocal = 0x4b800000 if width == 32 else 0x4340000000000000
            assert result == (half if operation == 2 else reciprocal) and status == seed, (number, "exact multiply/divide sanity")
    if args.qemu:
        version = subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]
        assert f"version {lock['version']}" in version
        source = (Path(args.qemu).parent / "source-commit.txt").read_text().strip()
        assert source == lock["commit"]
        expected_binary = (Path(args.qemu).parent / "binary.sha256").read_text().split()[0]
        assert digest(args.qemu) == expected_binary
        manifest = dict(lock, qemu_version=version, qemu_sha256=digest(args.qemu),
                        vectors_sha256=digest(HERE / "vectors.h"),
                        reference_source_sha256=digest(HERE / "reference.c"),
                        matrix_sha256=digest(args.matrix), elf_sha256=digest(args.elf))
        Path(args.manifest).write_text(json.dumps(manifest, indent=2) + "\n")
        print("PINNED_QEMU", source, version, manifest["qemu_sha256"])
        print("PINNED_AARCH64_REFERENCE", manifest["reference_source_sha256"], manifest["vectors_sha256"], manifest["elf_sha256"])
        print("ACTUAL_FPSR_MATRIX", manifest["matrix_sha256"])
    print("PASS independent QEMU AArch64 oracle:", len(expected), "numeric/status cases; all modes DN/FZ, both widths, NaN/inf/subnormal pairs")


if __name__ == "__main__":
    main()
