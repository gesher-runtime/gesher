#!/usr/bin/env python3
"""Reject unreviewed cache changes while retaining both published authorities."""
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cached_stdio_policy", Path(__file__).with_name("test-selected-policy.py"))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
FOLDER = "tools/net-tcp-priority/qualified/"
MANIFEST = FOLDER + "shared-stdio-cache-source.json"
OWNER_PATCH = FOLDER + "shared-stdio-cache.patch"
MAIN_PATCH = FOLDER + "shared-stdio-main-cache.patch"
read_bytes = Path.read_bytes
manifest = json.loads((ROOT / MANIFEST).read_bytes())
checks = 0


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rejected(label, changes, expected, repinned=None):
    global checks
    overrides = {ROOT / name: data for name, data in changes.items()}
    for name, data in overrides.items():
        assert data != read_bytes(name), "negative made no change: " + str(name)
    pin = policy.CACHED_STDIO_TRANSITION_SHA256
    if repinned is not None:
        raw = (json.dumps(repinned, indent=2) + "\n").encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, "read_bytes", mutated), \
            patch.object(policy, "CACHED_STDIO_TRANSITION_SHA256", pin), \
            contextlib.redirect_stdout(io.StringIO()):
        try:
            policy.check()
        except AssertionError as error:
            assert expected in str(error), (label, expected, error)
        except subprocess.CalledProcessError as error:
            assert expected == "exact patch application", (label, error)
            assert error.cmd == ["git", "apply", "--reverse", "--check", "-"], error
            assert error.returncode == 1, error
        else:
            raise AssertionError("source policy accepted " + label)
    checks += 1
    print("PASS shared-owner cache policy rejects " + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

owner = manifest["owner_path"]
source = (ROOT / owner).read_bytes()
rejected("changed owner cache semantics", {owner: source.replace(b"cached.generation != 0", b"cached.generation == 0", 1)}, owner)
changed_source = source + b'\n#include "unqualified_cache_owner.h"\n'
changed = copy.deepcopy(manifest)
changed["owner_after_sha256"] = sha(changed_source)
rejected("an undeclared include with a repinned endpoint", {owner: changed_source}, "cached-stdio reverse endpoint", changed)
marker = b"    InvalidateCachedOwner(stream);\n"
assert source.count(marker) == 2
changed_source = source.replace(marker, b"", 1)
changed = copy.deepcopy(manifest)
changed["owner_after_sha256"] = sha(changed_source)
rejected("missing retirement with a repinned endpoint", {owner: changed_source}, "exact patch application", changed)

for filename, prefix in [(OWNER_PATCH, "owner"), (MAIN_PATCH, "main")]:
    raw = (ROOT / filename).read_bytes()
    rejected("changed patch without its pin: " + filename, {filename: raw + b"\n"}, "cached-stdio patch changed")
    extra = raw + b"diff --git a/unqualified.cc b/unqualified.cc\n"
    changed = copy.deepcopy(manifest)
    changed[prefix + "_patch_sha256"] = sha(extra)
    rejected("an undeclared patch path: " + filename, {filename: extra}, "cached-stdio patch paths changed", changed)
    line = next(line for line in raw.splitlines(keepends=True)
                if line.startswith(b"-") and not line.startswith(b"---") and line.strip() != b"-")
    wrong_before = raw.replace(line, b"-/* unqualified baseline */ " + line[1:], 1)
    changed = copy.deepcopy(manifest)
    changed[prefix + "_patch_sha256"] = sha(wrong_before)
    rejected("a repinned false predecessor: " + filename, {filename: wrong_before}, "cached-stdio reverse endpoint", changed)

for key, value, expected in [
    ("shared_baseline", "0" * 40, "cached-stdio shared baseline changed"),
    ("main_baseline", "0" * 40, "cached-stdio main baseline changed"),
    ("original_ofd_sha256", "0" * 64, "cached-stdio original authority changed"),
    ("main_ofd_sha256", "0" * 64, "cached-stdio main authority changed"),
    ("owner_path", "compat/posix/src/ofd.cc", "cached-stdio paths changed"),
    ("main_path", "compat/bionic/shim.cc", "cached-stdio paths changed"),
    ("shared_policy_sha256", "0" * 64, "cached-stdio policy changed"),
    ("main_policy_sha256", "0" * 64, "cached-stdio policy changed"),
    ("owner_before_sha256", "0" * 64, owner),
    ("owner_after_sha256", "0" * 64, owner),
    ("main_before_sha256", "0" * 64, manifest["main_path"]),
    ("main_after_sha256", "0" * 64, manifest["main_path"]),
]:
    changed = copy.deepcopy(manifest)
    changed[key] = value
    rejected("a repinned " + key, {}, expected, changed)

changed = copy.deepcopy(manifest)
changed["native_execution_approved"] = True
rejected("an invented native approval", {}, "cached-stdio schema changed", changed)
rejected("changed manifest without its pin", {MANIFEST: (ROOT / MANIFEST).read_bytes() + b"\n"}, "cached-stdio transition changed")
print("SHARED_STDIO_CACHE_POLICY_PASS original_and_main_authorities_preserved=1 negative_controls=" + str(checks))
