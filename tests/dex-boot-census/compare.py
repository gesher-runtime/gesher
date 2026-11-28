#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare complete bootstrap class resolution captures for an identical SDK list."""
from collections import Counter
from pathlib import Path
import argparse


def read_capture(path, names):
    observations = {}
    totals = []
    with path.open(encoding="utf-8-sig") as capture:
        for line in capture:
            parts = line.rstrip("\r\n").split("\t")
            if parts[0] == "BOOT_TOTAL":
                totals.append(int(parts[1]))
            if parts[0] != "BOOT_CLASS":
                continue
            _, outcome, name = parts
            if name not in names or name in observations:
                raise ValueError(f"{path}: unexpected or duplicate class {name}")
            if outcome not in ("resolved", "absent", "restricted") and not outcome.startswith("linkage:"):
                raise ValueError(f"{path}: invalid outcome for {name}")
            observations[name] = outcome
    if totals != [len(names)] or observations.keys() != names:
        raise ValueError(f"{path}: incomplete class capture")
    return observations


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("classes", type=Path)
    parser.add_argument("android", type=Path)
    parser.add_argument("windows", type=Path)
    args = parser.parse_args()
    rows = args.classes.read_text(encoding="utf-8").splitlines()
    if not rows or any(not row for row in rows) or len(rows) != len(set(rows)):
        parser.error("expected a nonempty unique SDK class list")
    names = set(rows)
    android = read_capture(args.android, names)
    windows = read_capture(args.windows, names)
    pairs = Counter((android[name], windows[name]) for name in names)
    for (reference, candidate), count in sorted(pairs.items()):
        print(f"Android={reference} Windows={candidate} {count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
