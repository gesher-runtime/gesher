#!/usr/bin/env python3
"""Resolve separately compiled fixture references against the actual R8 runtime.

Input consumers are real DEX files, JARs or APKs. Only references whose original
definitions belong to gesher-app are checked; Android/framework library members
remain outside this audit. Dynamic reflection is covered separately by the
explicit fixture contract rules and the production R8ContractCheck gate.
"""
from __future__ import annotations

import argparse
from functools import lru_cache
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import java_member_inventory as inventory


def references(data: bytes) -> tuple[set[str], set[tuple[str, tuple[str, str, str]]], set[str]]:
    # Reuse the validated DEX reader before reading reference tables. It checks
    # bounds, table indexes, checksums, descriptors and encoded member data.
    inventory.dex_members(data)
    u16, u32 = inventory.u16, inventory.u32
    strings_size, strings_at = u32(data, 56), u32(data, 60)
    types_size, types_at = u32(data, 64), u32(data, 68)
    protos_size, protos_at = u32(data, 72), u32(data, 76)

    @lru_cache(maxsize=None)
    def string(index: int) -> str:
        if index >= strings_size:
            raise ValueError("Invalid DEX string reference")
        offset = u32(data, strings_at + index * 4)
        units, start = inventory.read_uleb(data, offset)
        end = data.index(b"\0", start)
        return inventory.modified_utf8(data[start:end], units)

    types = [string(u32(data, types_at + index * 4)) for index in range(types_size)]
    protos = []
    for index in range(protos_size):
        at = protos_at + index * 12
        result = types[u32(data, at + 4)]
        parameters = u32(data, at + 8)
        args = ("".join(types[u16(data, parameters + 4 + offset * 2)]
                        for offset in range(u32(data, parameters))) if parameters else "")
        protos.append("(" + args + ")" + result)
    members = set()
    for kind, header in (("field", 80), ("method", 88)):
        count, table = u32(data, header), u32(data, header + 4)
        for index in range(count):
            at = table + index * 8
            owner = types[u16(data, at)]
            if not owner.startswith("Lcom/gesher/"):
                continue
            name = string(u32(data, at + 4))
            descriptor = (types if kind == "field" else protos)[u16(data, at + 2)]
            members.add((owner[1:-1].replace("/", "."), (kind, name, descriptor)))
    classes = {name for descriptor in types for name in inventory.descriptor_classes(descriptor)
               if name.startswith("com.gesher.")}
    # Constant fully qualified Class.forName names are also useful evidence.
    # Prefix/variable construction is deliberately not claimed as covered here.
    names = {string(index) for index in range(strings_size)
             if string(index).startswith("com.gesher.")}
    return classes, members, names


def dex_files(path: Path):
    if path.suffix == ".dex":
        yield path.read_bytes()
    else:
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                if inventory.base.DEX_ENTRY.fullmatch(name):
                    yield archive.read(name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--classes", type=Path, required=True)
    parser.add_argument("--dex", type=Path, required=True)
    parser.add_argument("--consumers", type=Path, required=True, nargs="+")
    args = parser.parse_args()
    original, _ = inventory.archive_members(args.classes, "sdk", "fixture-compile-api")
    actual, _ = inventory.archive_members(args.dex, "boot", "production-r8-runtime")
    before, after = inventory.Resolver(original), inventory.Resolver(actual)
    classes, members, literals = set(), set(), set()
    count = 0
    for path in args.consumers:
        emitted = 0
        for data in dex_files(path):
            new_classes, new_members, new_literals = references(data)
            classes.update(new_classes)
            members.update(new_members)
            literals.update(new_literals)
            emitted += 1
        if not emitted:
            raise ValueError(f"Consumer contains no DEX: {path}")
        count += emitted
    classes = (classes | literals) & original.keys()
    failures = []
    for name in sorted(classes):
        if name not in actual:
            failures.append(f"missing/renamed consumer class: {name}")
            continue
        source_flags, runtime_flags = original[name].flags, actual[name].flags
        if source_flags & inventory.PUBLIC and not runtime_flags & inventory.PUBLIC:
            failures.append(f"consumer class is no longer public: {name}")
        if bool(source_flags & inventory.INTERFACE) != bool(runtime_flags & inventory.INTERFACE):
            failures.append(f"consumer class/interface contract changed: {name}")
        if not source_flags & inventory.FINAL and runtime_flags & inventory.FINAL:
            failures.append(f"consumer class became final: {name}")
        if not source_flags & inventory.ABSTRACT and runtime_flags & inventory.ABSTRACT:
            failures.append(f"consumer class became abstract: {name}")
    checked = 0
    for owner, key in sorted(members):
        if owner not in original:
            continue
        required, _ = before.lookup(owner, key)
        # An inherited member supplied by framework/libcore is not modified by
        # framework R8; only compare declarations actually found in gesher-app.
        if not required:
            continue
        checked += 1
        found, _ = after.lookup(owner, key) if owner in actual else ([], True)
        label = f"{owner}.{key[1]}{key[2]}"
        if not found:
            failures.append(f"missing/renamed consumer {key[0]}: {label}")
        elif not any(not inventory.flag_issues(expected, emitted)
                     for _, expected in required for _, emitted in found):
            failures.append(f"consumer member flags changed: {label}")
    if not classes or not checked:
        failures.append("No gesher-app consumer class/member contracts found")
    for failure in failures[:100]:
        print("R8 fixture contract: " + failure, file=sys.stderr)
    if len(failures) > 100:
        print(f"R8 fixture contract: {len(failures) - 100} more failures", file=sys.stderr)
    print(f"R8 fixture contracts: {count} consumer DEX files, {len(classes)} classes, "
          f"{checked} member references, {len(failures)} failures")
    return int(bool(failures))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (inventory.InvalidInput, OSError, ValueError, zipfile.BadZipFile) as error:
        print(f"R8 fixture contract: {error}", file=sys.stderr)
        raise SystemExit(1)
