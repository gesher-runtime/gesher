#!/usr/bin/env python3
"""Fetch only the two unmodified AOSP files needed by the POSIX component."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import urllib.request


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = (root / "aosp.lock").read_text()
    tags = re.findall(r"^tag (\S+)$", lock, re.MULTILINE)
    if len(tags) != 1 or not re.fullmatch(r"android-[0-9]+\.[0-9]+\.[0-9]+_r[0-9]+", tags[0]):
        raise ValueError("Expected one pinned Android release tag in aosp.lock")
    tag = tags[0]
    records = []
    for project, relative in (
        ("bionic", "libc/kernel/uapi/linux/android/binder.h"),
        ("external/icu", "icu4c/source/data/misc/windowsZones.txt"),
    ):
        url = f"https://android.googlesource.com/platform/{project}/+/refs/tags/{tag}/{relative}?format=TEXT"
        with urllib.request.urlopen(url, timeout=60) as response:
            encoded = response.read(2 * 1024 * 1024 + 1)
        if len(encoded) > 2 * 1024 * 1024:
            raise ValueError("Component input exceeds its size limit")
        data = base64.b64decode(encoded, validate=True)
        destination = args.destination / project / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        records.append({"path": f"{project}/{relative}", "url": url,
                        "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)})
    args.destination.mkdir(parents=True, exist_ok=True)
    (args.destination / "component-inputs.json").write_text(json.dumps(
        {"tag": tag, "aosp_lock_sha256": hashlib.sha256(lock.encode()).hexdigest(), "inputs": records},
        indent=2) + "\n")
    print(json.dumps({"tag": tag, "inputs": records}, indent=2))


if __name__ == "__main__":
    main()
