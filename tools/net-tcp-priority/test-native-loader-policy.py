#!/usr/bin/env python3
"""Negative controls for PR107's exact source transition; no native execution.

Mutations are in-memory test inputs. The working tree, original OFD receipt,
supplier receipts and production sources are never rewritten by this test.
The tests distinguish accepting an exact reviewed delta from accepting a new
full-file hash, an expanded path list or a changed baseline.
"""
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
spec = importlib.util.spec_from_file_location(
    "selected_policy", Path(__file__).with_name("test-selected-policy.py"))
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)
FOLDER = "tools/net-tcp-priority/qualified/"
MANIFEST = FOLDER + "native-loader-source.json"
PATCH = FOLDER + "native-loader-source.patch"
read_bytes = Path.read_bytes
manifest = json.loads((ROOT / MANIFEST).read_bytes())
transition_patch = (ROOT / PATCH).read_bytes()
checks = 0


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rejected(label, changes, expected, repinned_manifest=None):
    """Require the specified guard, rather than an unrelated failure, to reject."""
    global checks
    overrides = {ROOT / name: data for name, data in changes.items()}
    for path, data in overrides.items():
        assert data != read_bytes(path), "negative control made no change: " + str(path)
    pin = policy.NATIVE_LOADER_TRANSITION_SHA256
    if repinned_manifest is not None:
        raw = (json.dumps(repinned_manifest, indent=2, sort_keys=True) + "\n").encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, "read_bytes", mutated), \
            patch.object(policy, "NATIVE_LOADER_TRANSITION_SHA256", pin), \
            contextlib.redirect_stdout(io.StringIO()):
        try:
            policy.check()
        except AssertionError as error:
            assert expected in str(error), (label, expected, error)
        except subprocess.CalledProcessError as error:
            assert expected == "exact patch application", (label, error)
            assert error.cmd == ["git", "apply", "--reverse", "--unidiff-zero", "--check", "-"], error
            assert error.returncode == 1, error
        else:
            raise AssertionError("source policy accepted " + label)
    checks += 1
    print("PASS native-loader source policy rejects " + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

# A change anywhere in each admitted implementation or coupled header fails.
for name in manifest["paths"]:
    rejected("unqualified bytes in " + name,
             {name: (ROOT / name).read_bytes() + b"\n// unqualified mutation\n"}, name)

# Replacing the new full-file hash is insufficient: the exact original patch
# must still reverse all bytes to the original main file and OFD receipt.
for name in ("compat/bionic/shim.cc", "compat/bionic/shim_internal.h"):
    mutated = (ROOT / name).read_bytes() + b'\n#include "unqualified_native_loader.h"\n'
    changed = copy.deepcopy(manifest)
    # The original namespace endpoint precedes main's later exact memmem edit.
    endpoint = policy.preceding_memmem_source(mutated) if name == "compat/bionic/shim.cc" else mutated
    changed["after_sha256"][name] = sha(endpoint)
    rejected("an undeclared include even with a repinned endpoint: " + name,
             {name: mutated}, "native-loader reverse endpoint", changed)

# Both independently pinned transitions must be present. Their composition
# cannot drop either feature or turn scoped lookup into an aggregate fallback.
shim_path = "compat/bionic/shim.cc"
shim = (ROOT / shim_path).read_bytes()
rejected("the old namespace source without main's memmem change",
         {shim_path: policy.preceding_memmem_source(shim)}, "exact reviewed memmem substitution changed")
only_memmem = subprocess.check_output(["git", "-C", str(ROOT), "show",
                                      policy.NATIVE_LOADER_BASE + ":" + shim_path])
rejected("main's memmem source without the namespace ownership transition", {shim_path: only_memmem}, shim_path)
scoped = b"return LookupOwnedSymbol(name, symbol);"
assert shim.count(scoped) == 1
unscoped = shim.replace(scoped, b"return LookupBionicSymbol(symbol);", 1)
changed = copy.deepcopy(manifest)
changed["after_sha256"][shim_path] = sha(policy.preceding_memmem_source(unscoped))
rejected("aggregate lookup despite a repinned normalized endpoint", {shim_path: unscoped},
         "exact patch application", changed)

# The unchanged dependency is compared with independently named main bytes.
name = policy.NATIVE_LOADER_UNCHANGED_PATHS[0]
mutated = (ROOT / name).read_bytes() + b"\n// changed symbol-table contract\n"
changed = copy.deepcopy(manifest)
changed["unchanged_sha256"][name] = sha(mutated)
rejected("repinning an unchanged coupled header", {name: mutated}, name, changed)

# The transition cannot expand the original protected-file exception.
extra_path = "compat/posix/src/ofd.cc"
changed = copy.deepcopy(manifest)
changed["paths"].append(extra_path)
rejected("an undeclared transition file", {}, "native-loader transition paths changed", changed)
changed = copy.deepcopy(manifest)
changed["protected_paths"].append(extra_path)
rejected("an expanded protected-file list", {}, "native-loader protected paths changed", changed)
changed = copy.deepcopy(manifest)
changed["coupled_paths"].append("compat/bionic/abi_types.h")
rejected("an undeclared coupled header", {}, "native-loader coupled paths changed", changed)

extra_patch = transition_patch + b"diff --git a/unqualified.cc b/unqualified.cc\n"
changed = copy.deepcopy(manifest)
changed["patch_sha256"] = sha(extra_patch)
rejected("an undeclared patch path even with repinned patch and manifest",
         {PATCH: extra_patch}, "native-loader patch paths changed", changed)

# Alter a removed line, keeping a syntactically valid patch and the same paths.
# Reverse application can reconstruct that line, but cannot change main bytes.
line = next(line for line in transition_patch.splitlines(keepends=True)
            if line.startswith(b"-") and not line.startswith(b"---") and line.strip() != b"-")
wrong_before = transition_patch.replace(line, b"-/* unqualified baseline */ " + line[1:], 1)
changed = copy.deepcopy(manifest)
changed["patch_sha256"] = sha(wrong_before)
rejected("a modified before-hunk even with repinned patch and manifest",
         {PATCH: wrong_before}, "native-loader reverse endpoint", changed)

changed = copy.deepcopy(manifest)
changed["baseline"] = "0" * 40
rejected("a repinned baseline identity", {}, "native-loader baseline changed", changed)
changed = copy.deepcopy(manifest)
changed["feature_head"] = "0" * 40
rejected("a repinned feature identity", {}, "native-loader feature checkpoint changed", changed)
changed = copy.deepcopy(manifest)
name = manifest["protected_paths"][0]
changed["before_sha256"][name] = "0" * 64
rejected("a repinned baseline source digest", {}, name, changed)
changed = copy.deepcopy(manifest)
changed["baseline_policy_sha256"] = "0" * 64
rejected("a repinned baseline policy", {}, "native-loader baseline policy changed", changed)

rejected("changed transition metadata without its independent pin",
         {MANIFEST: (ROOT / MANIFEST).read_bytes() + b"\n"}, "native-loader source transition changed")
rejected("changed transition patch without its independent pin",
         {PATCH: transition_patch + b"\n"}, "native-loader transition patch changed")

# These files never enter the native-loader normalization and must continue to
# fail the original exact OFD source qualification, including its header rules.
for name in ("compat/posix/src/ofd.cc", "compat/posix/src/stdio_output.cc",
             "compat/posix/src/fd.cc", "compat/posix/include/gesher/stdio.h"):
    rejected("unrelated protected bytes in " + name,
             {name: (ROOT / name).read_bytes() + b"\n// unqualified mutation\n"}, name)
name = FOLDER + "ofd-stream-source.json"
rejected("a modified original OFD receipt", {name: (ROOT / name).read_bytes() + b"\n"},
         "OFD source qualification changed")
print("NATIVE_LOADER_SOURCE_POLICY_PASS exact_main_round_trip=9 original_protected_digests=71 negative_controls=" + str(checks))
