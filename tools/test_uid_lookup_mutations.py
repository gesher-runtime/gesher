#!/usr/bin/env python3
"""Require real PMS API contracts to reject isolated complete-source UID mutations."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PMS = "app/java/com/gesher/server/PackageManagerService.java"
VISIBILITY = "app/java/com/gesher/server/PackageVisibility.java"
MUTATIONS = (
    ("exact-uid", VISIBILITY,
     "if (entry.uid == uid && entry.apk != null) return entry.apk;",
     "if (entry.apk != null) return entry.apk;", "Reused UID inherited an old owner or shared name"),
    ("generation", PMS,
     "!snapshot.uidLookupSuitable || snapshot.packages != mPackages",
     "!snapshot.uidLookupSuitable || false", "Retired UID remained in identity metadata"),
    ("pending-owner", PMS,
     "snapshot.removing != mRemoving || !snapshot.removing.isEmpty()",
     "snapshot.removing != mRemoving", "Pending removal prematurely removed an admitted UID owner"),
    ("registration-order", VISIBILITY,
     "mPackagesByAppId.finish();", "/* negative control: leave prepended groups reversed */",
     "Wrong exact ordered UID owners"),
    ("collision-probe", VISIBILITY,
     "Entry get(int key) { return heads[slot(key)]; }",
     "Entry get(int key) { return heads[(key ^ (key >>> 16)) & (heads.length - 1)]; }",
     "Growing app-ID index lost an exact UID"),
    ("uniform-first-owner", PMS,
     "if (pkg.uid() == uniformUid) { name = packageUidName(pkg, uniformUid, true); break; }",
     "if (pkg.uid() == uniformUid) { name = packageUidName(pkg, uniformUid, true); }",
     "Cold repeated UID batch lost the first owner or core names"),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig", "output-dir"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--baseline-ref", required=True)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    if output.exists(): parser.error("Choose a fresh output directory to retain earlier controls")
    output.mkdir(parents=True)
    sources = {path: (ROOT / path).read_text() for path in (PMS, VISIBILITY)}
    command = [sys.executable, str(ROOT / "tools/test_package_visibility.py")]
    for name in ("java-home", "framework-classes", "core-classes", "app-classes", "apksig"):
        command += ["--" + name, str(getattr(args, name.replace("-", "_")).resolve())]
    receipts = []
    for name, path, before, after, expected in MUTATIONS:
        if sources[path].count(before) != 1: raise AssertionError("Mutation target changed: " + name)
        tree = output / name
        for relative, source in sources.items():
            destination = tree / relative; destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(source.replace(before, after, 1) if relative == path else source)
        result = subprocess.run(command + ["--source-dir", str(tree), "--uid-contract-check"],
                                text=True, capture_output=True, timeout=180)
        log = output / (name + ".log"); log.write_text(result.stdout + result.stderr)
        text = log.read_text()
        if result.returncode == 0 or "java.lang.AssertionError:" not in text or expected not in text:
            raise AssertionError("Mutation did not fail at its actual API contract: " + str(log))
        receipts.append(dict(name=name, changed_source=path, expected_failure=expected,
                source_sha256=hashlib.sha256((tree / path).read_bytes()).hexdigest(),
                log_sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
        print("Rejected actual-source mutation: " + name, flush=True)
    result = subprocess.run(command + ["--source-ref", args.baseline_ref, "--uid-work-check"],
                            text=True, capture_output=True, timeout=180)
    log = output / "unchanged-baseline-work.log"; log.write_text(result.stdout + result.stderr)
    if result.returncode == 0 or "Cold UID batch revisited registry records:" not in log.read_text():
        raise AssertionError("Unchanged source did not fail the actual registry-work budget: " + str(log))
    receipts.append(dict(name="unchanged-baseline-work", source_ref=args.baseline_ref,
                         log_sha256=hashlib.sha256(log.read_bytes()).hexdigest()))
    (output / "receipts.json").write_text(json.dumps(dict(
        production_sha256={path: hashlib.sha256(source.encode()).hexdigest() for path, source in sources.items()},
        controls=receipts), indent=2) + "\n")
    print("All six complete-source API mutations and unchanged baseline work control rejected", flush=True)


if __name__ == "__main__":
    main()
