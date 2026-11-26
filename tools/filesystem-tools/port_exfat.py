#!/usr/bin/env python3
# Copyright (C) 2026 The Gesher Authors
# SPDX-License-Identifier: Apache-2.0
"""Apply the audited capability-query port to an external exfatprogs source tree.

The repository contains transformation instructions and our independent helper,
not a copy of an upstream GPL function. Every upstream byte comes from the
authenticated original archive, and both the input and output file are pinned.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import tarfile


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def unique(source: bytes, marker: bytes, start: int = 0, end: int | None = None) -> int:
    end = len(source) if end is None else end
    require(source.count(marker, start, end) == 1, f"Port marker changed: {marker!r}")
    return source.index(marker, start, end)


def block_end(source: bytes, opening: int) -> int:
    """Find a C block end, ignoring braces in comments and quoted literals."""
    require(source[opening:opening + 1] == b"{", "Missing port block boundary")
    depth = 1
    cursor = opening + 1
    while cursor < len(source):
        if source[cursor:cursor + 2] == b"/*":
            closing = source.find(b"*/", cursor + 2)
            require(closing >= 0, "Unterminated C comment")
            cursor = closing + 2
        elif source[cursor:cursor + 2] == b"//":
            closing = source.find(b"\n", cursor + 2)
            require(closing >= 0, "Unterminated C line")
            cursor = closing + 1
        elif source[cursor] in (ord('"'), ord("'")):
            quote = source[cursor]
            cursor += 1
            while cursor < len(source) and source[cursor] != quote:
                cursor += 2 if source[cursor] == ord("\\") else 1
            require(cursor < len(source), "Unterminated C literal")
            cursor += 1
        else:
            if source[cursor] == ord("{"):
                depth += 1
            elif source[cursor] == ord("}"):
                depth -= 1
                if depth == 0:
                    return cursor + 1
            cursor += 1
    raise RuntimeError("Unterminated C block")


def transform(source: bytes, spec: dict) -> tuple[bytes, dict]:
    require(sha256(source) == spec["input_sha256"], "exFAT port input SHA256 mismatch")
    edits: list[tuple[int, int, bytes, str]] = []

    def edit(start: int, end: int, replacement: bytes, purpose: str) -> None:
        edits.append((start, end, replacement, purpose))

    include = b'#include "exfat_dir.h"\n'
    position = unique(source, include) + len(include)
    edit(position, position, b'#include "exfat_capabilities.h"\n', "include independent capability helper")

    start = unique(source, b"static int exfat_cmp_kernel_ver(")
    end = block_end(source, source.index(b"{", start))
    require(source[end:end + 2] == b"\n\n", "Changed kernel comparator boundary")
    edit(start, end + 2, b"", "remove kernel release comparator")

    function = unique(source, b"int exfat_get_blk_dev_info(")
    function_end = block_end(source, source.index(b"{", function))
    declaration = re.search(rb"^\tstatic const unsigned short MIN_KERNEL_VER\[[^\n]+\n",
                            source[function:function_end], re.MULTILINE)
    require(declaration is not None, "Missing minimum kernel declaration")
    edit(function + declaration.start(), function + declaration.end(), b"",
         "remove obsolete minimum kernel declaration")

    switch = unique(source, b"\tswitch (exfat_cmp_kernel_ver(", function, function_end)
    comment = source.rfind(b"\n\t/*", function, switch) + 1
    require(comment > function and source[source.index(b"*/", comment) + 2:switch].strip() == b"",
            "Changed kernel gate comment boundary")
    end = block_end(source, source.index(b"{", switch))
    require(source[end:end + 2] == b"\n\n", "Changed kernel gate boundary")
    edit(comment, end + 2, b"", "replace release heuristic with actual opened-target capabilities")

    opened = unique(source, b"\tif (fd < 0) {", function, function_end)
    end = block_end(source, source.index(b"{", opened))
    edit(end, end, b'''\n
\tstruct gesher_exfat_geometry gesher_geometry;
\tif (gesher_exfat_check_target(fd, pathname, PATH_MAX + 1, &gesher_geometry) != 0) {
\t\texfat_err("required target interfaces unavailable: %s\\n", strerror(errno));
\t\tgoto out;
\t}''', "validate the exact successfully opened target before filesystem work")

    position = source.index(b"\tbd->dev_fd = fd;", opened, function_end)
    edit(position, position, b'''\tif (gesher_exfat_recheck_target(fd, &gesher_geometry) != 0) {
\t\texfat_err("target changed or became unavailable: %s\\n", strerror(errno));
\t\tgoto out;
\t}
\tif (gesher_geometry.is_block) {
\t\tif ((uint64_t)blk_dev_size != gesher_geometry.bytes) {
\t\t\terrno = ESTALE;
\t\t\texfat_err("target size disagrees with the opened block object\\n");
\t\t\tgoto out;
\t\t}
\t\tbd->isblk = true;
\t\tbd->ispart = gesher_geometry.partition != 0;
\t\tblk_dev_offset = gesher_geometry.offset_bytes;
\t}

''', "use verified block size and partition geometry; recheck revocation and identity")

    sector = b"\tbd->dev_sector_size = 0;\n"
    start = source.index(sector, position, function_end)
    end = source.index(b"\t\tbd->dev_sector_size = 0;\n", start, function_end)
    end += len(b"\t\tbd->dev_sector_size = 0;\n")
    original = source[start:end]
    replacement = (b"\tif (gesher_geometry.is_block) {\n"
                   b"\t\tbd->dev_sector_size = gesher_geometry.sector_bytes;\n"
                   b"\t} else {\n" + b"".join(b"\t" + line for line in original.splitlines(keepends=True)) +
                   b"\t}\n")
    edit(start, end, replacement, "preserve image-file fallback; use queried block sector size")

    edits.sort()
    cursor = 0
    chunks: list[bytes] = []
    audit = []
    for start, end, replacement, purpose in edits:
        require(cursor <= start <= end <= len(source), "Overlapping port transformations")
        chunks.extend((source[cursor:start], replacement))
        audit.append({"purpose": purpose, "offset": start, "removed_bytes": end - start,
                      "removed_sha256": sha256(source[start:end]), "inserted_bytes": len(replacement),
                      "inserted_sha256": sha256(replacement)})
        cursor = end
    chunks.append(source[cursor:])
    result = b"".join(chunks)
    require(sha256(result) == spec["output_sha256"],
            f"exFAT port output SHA256 mismatch: {sha256(result)}")
    require(b"exfat_cmp_kernel_ver" not in result and b"MIN_KERNEL_VER" not in result,
            "Obsolete kernel heuristic remains")
    encoded_audit = json.dumps(audit, sort_keys=True, separators=(",", ":")).encode()
    return result, {"id": spec["id"], "package": spec["package"], "path": spec["path"],
                    "input_sha256": sha256(source), "output_sha256": sha256(result),
                    "transformation_sha256": sha256(encoded_audit), "edits": audit,
                    "port_files": spec["files"]}


def check_port_files(directory: Path, spec: dict) -> None:
    for name, expected in spec["files"].items():
        require(Path(name).name == name, "Port input must be a sibling source file")
        path = directory / name
        require(path.is_file() and not path.is_symlink(), f"Missing ordinary port source: {name}")
        require(sha256(path.read_bytes()) == expected, f"Port source SHA256 mismatch: {name}")


def original_from_archive(archive: Path, package: dict, spec: dict) -> bytes:
    require(archive.is_file() and not archive.is_symlink() and archive.stat().st_size == package["bytes"],
            "exFAT original archive length/type mismatch")
    require(sha256(archive.read_bytes()) == package["sha256"], "exFAT original archive SHA256 mismatch")
    with tarfile.open(archive) as supplier:
        member = supplier.getmember(package["directory"] + "/" + spec["path"])
        require(member.isfile() and 0 < member.size <= 1024 * 1024, "Unexpected exFAT port source entry")
        stream = supplier.extractfile(member)
        require(stream is not None, "Missing original exFAT port source")
        with stream:
            return stream.read()


def apply(source_root: Path, spec: dict, directory: Path) -> dict:
    check_port_files(directory, spec)
    path = source_root / spec["path"]
    require(path.is_file() and not path.is_symlink(), "Missing ordinary external exFAT source")
    result, receipt = transform(path.read_bytes(), spec)
    helper = path.parent / "exfat_capabilities.h"
    require(not helper.exists(), "External exFAT tree already contains a port helper")
    helper.write_bytes((directory / "exfat_capabilities.h").read_bytes())
    path.write_bytes(result)
    return receipt
