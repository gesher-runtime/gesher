#!/usr/bin/env python3
"""Partition this tiny fixture's real ResTable_type density configs, without changing IDs.

The Windows SDK linker currently leaves the requested density split empty. The
fixture must actually remove xxxhdpi from the base to test AssetManager merging.
Layout follows AOSP androidfw/ResourceTypes.h (ResTable_package/type/config).
"""
from pathlib import Path
import struct
import sys
import zipfile

def u16(data, offset): return struct.unpack_from('<H', data, offset)[0]
def u32(data, offset): return struct.unpack_from('<I', data, offset)[0]
def children(data):
    at = u16(data, 2)
    assert u32(data, 4) == len(data)
    while at < len(data):
        size = u32(data, at + 4)
        assert size >= 8 and at + size <= len(data)
        yield at, data[at:at + size]
        at += size
    assert at == len(data)

def package(data, split):
    header = bytearray(data[:u16(data, 2)])
    assert len(header) >= 284
    selected = []
    counts = {}
    moved = 0
    for offset, chunk in children(data):
        if u16(chunk, 0) == 0x0201:  # ResTable_type.config.screenType.density
            assert u16(chunk, 2) >= 36 and u32(chunk, 20) >= 16
            density = u16(chunk, 34)
            if density == 640: moved += 1
            if (density == 640) != split: continue
            counts[chunk[8]] = counts.get(chunk[8], 0) + 1
        selected.append((offset, chunk))
    assert moved > 0, 'Fixture did not contain a real xxxhdpi config'
    body = bytearray()
    mapping = {}
    for offset, chunk in selected:
        mapping[offset] = len(header) + len(body)
        chunk = bytearray(chunk)
        if u16(chunk, 0) == 0x0202 and u16(chunk, 10):
            struct.pack_into('<H', chunk, 10, counts.get(chunk[8], 0))
        body.extend(chunk)
    # Type/key string pool positions are package-relative.
    for field in [268, 276]:
        original = u32(header, field)
        if original: struct.pack_into('<I', header, field, mapping[original])
    struct.pack_into('<I', header, 4, len(header) + len(body))
    return bytes(header + body)

def table(data, split):
    assert u16(data, 0) == 2 and u32(data, 8) == 1
    head = bytearray(data[:u16(data, 2)])
    body = b''.join(package(chunk, split) if u16(chunk, 0) == 0x0200 else chunk
                    for _, chunk in children(data))
    struct.pack_into('<I', head, 4, len(head) + len(body))
    return bytes(head) + body

out = Path(sys.argv[1])
with zipfile.ZipFile(out/'icon-full.apk') as z:
    original = {name: z.read(name) for name in z.namelist()}
with zipfile.ZipFile(out/'icon-density.apk') as z:
    split_manifest = z.read('AndroidManifest.xml')
for split, name in [(False, 'icon-base-unaligned.apk'), (True, 'icon-density-unaligned.apk')]:
    with zipfile.ZipFile(out/name, 'w') as z:
        z.writestr('AndroidManifest.xml', split_manifest if split else original['AndroidManifest.xml'])
        z.writestr('resources.arsc', table(original['resources.arsc'], split))
        for path, data in original.items():
            if path.startswith('res/') and (('mipmap-xxxhdpi' in path) == split):
                z.writestr(path, data)
print('Partitioned real xxxhdpi config into density APK; base has only lower-density PNGs')
