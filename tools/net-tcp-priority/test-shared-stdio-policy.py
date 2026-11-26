#!/usr/bin/env python3
"""Exercise PR73's exact source transition; no native execution or approval.

All mutations are in-memory inputs to the actual policy. The working tree,
original OFD receipt, supplier receipts and production files remain intact.
Repinned endpoint controls distinguish an exact reversible transition from
accepting an arbitrary new file digest or broadening its admitted paths.
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
MANIFEST = FOLDER + "shared-stdio-source.json"
PATCH = FOLDER + "shared-stdio-source.patch"
read_bytes = Path.read_bytes
manifest = json.loads((ROOT / MANIFEST).read_bytes())
transition_patch = (ROOT / PATCH).read_bytes()
checks = 0
normalized = policy.preceding_stdio_feature_sources()
shared_input = {name: normalized[name] if name in normalized else read_bytes(ROOT / name)
                for name in manifest["paths"]}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def rejected(label, changes, expected, repinned_manifest=None, normalized_boundary=False):
    """Require the intended guard, rather than an unrelated check, to fail."""
    global checks
    overrides = {ROOT / name: data for name, data in changes.items()}
    for path, data in overrides.items():
        assert data != read_bytes(path), "negative control made no change: " + str(path)
    pin = policy.SHARED_STDIO_TRANSITION_SHA256
    if repinned_manifest is not None:
        raw = (json.dumps(repinned_manifest, indent=2, sort_keys=True) + "\n").encode()
        overrides[ROOT / MANIFEST] = raw
        pin = sha(raw)

    def mutated(path):
        return overrides[path] if path in overrides else read_bytes(path)

    with patch.object(Path, "read_bytes", mutated), \
            patch.object(policy, "SHARED_STDIO_TRANSITION_SHA256", pin), \
            contextlib.redirect_stdout(io.StringIO()):
        try:
            if normalized_boundary:
                qualification = json.loads(read_bytes(ROOT / (FOLDER + "ofd-stream-source.json")))
                current = dict(shared_input)
                current.update({name: data for name, data in changes.items() if name in current})
                policy.preceding_shared_stdio_sources(qualification, current)
            else:
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
    print("PASS shared-stdio source policy rejects " + label)


with contextlib.redirect_stdout(io.StringIO()):
    policy.check()

# Every admitted implementation and header remains exact.
for name in manifest["paths"]:
    rejected("unqualified bytes in " + name,
             {name: (ROOT / name).read_bytes() + b"\n// unqualified mutation\n"}, name)

# A changed after-digest is insufficient: an unlisted addition survives reverse
# application, so the bytes cannot equal the independently named main source.
for name in ("compat/bionic/stdio_bionic.cc", "compat/posix/include/gesher/stdio.h"):
    mutated = (ROOT / name).read_bytes() + b'\n#include "unqualified_stream_owner.h"\n'
    changed = copy.deepcopy(manifest)
    changed["after_sha256"][name] = sha(mutated)
    if name in policy.COMPOSED_STDIO_PATHS:
        rejected("an undeclared include through the full composed stdio transition: " + name,
                 {name: mutated}, name, changed)
        mutated = normalized[name] + b'\n#include "unqualified_stream_owner.h"\n'
        changed["after_sha256"][name] = sha(mutated)
    rejected("an undeclared include even with a repinned endpoint: " + name,
             {name: mutated}, "shared-stdio reverse endpoint", changed,
             normalized_boundary=name in policy.COMPOSED_STDIO_PATHS)

extra_path = "compat/posix/src/ofd.cc"
changed = copy.deepcopy(manifest)
changed["paths"].append(extra_path)
rejected("an undeclared transition file", {}, "shared-stdio transition paths changed", changed)
changed = copy.deepcopy(manifest)
changed["after_sha256"][extra_path] = sha((ROOT / extra_path).read_bytes())
rejected("an undeclared endpoint", {}, "shared-stdio endpoint paths changed", changed)
changed = copy.deepcopy(manifest)
changed["native_execution_approved"] = True
rejected("an invented approval field", {}, "shared-stdio transition schema changed", changed)

extra_patch = transition_patch + b"diff --git a/unqualified.cc b/unqualified.cc\n"
changed = copy.deepcopy(manifest)
changed["patch_sha256"] = sha(extra_patch)
rejected("an undeclared patch path even with repinned patch and manifest",
         {PATCH: extra_patch}, "shared-stdio patch paths changed", changed)

# A syntactically valid changed before-line can apply in reverse, but it must
# still reconstruct the exact baseline source pinned by the original receipt.
line = next(line for line in transition_patch.splitlines(keepends=True)
            if line.startswith(b"-") and not line.startswith(b"---") and line.strip() != b"-")
wrong_before = transition_patch.replace(line, b"-/* unqualified baseline */ " + line[1:], 1)
changed = copy.deepcopy(manifest)
changed["patch_sha256"] = sha(wrong_before)
rejected("a modified before-hunk even with repinned patch and manifest",
         {PATCH: wrong_before}, "shared-stdio reverse endpoint", changed)

changed = copy.deepcopy(manifest)
changed["baseline"] = "0" * 40
rejected("a repinned baseline identity", {}, "shared-stdio baseline changed", changed)
changed = copy.deepcopy(manifest)
changed["feature_head"] = "0" * 40
rejected("a repinned feature identity", {}, "shared-stdio feature checkpoint changed", changed)
changed = copy.deepcopy(manifest)
name = manifest["paths"][0]
changed["before_sha256"][name] = "0" * 64
rejected("a repinned baseline source digest", {}, name, changed)
changed = copy.deepcopy(manifest)
changed["baseline_policy_sha256"] = "0" * 64
rejected("a repinned baseline policy", {}, "shared-stdio baseline policy changed", changed)

rejected("changed transition metadata without its independent pin",
         {MANIFEST: (ROOT / MANIFEST).read_bytes() + b"\n"}, "shared-stdio source transition changed")
rejected("changed transition patch without its independent pin",
         {PATCH: transition_patch + b"\n"}, "shared-stdio transition patch changed")

# Protected sources outside these four files never enter the normalization.
for name in ("compat/posix/src/ofd.cc", "compat/posix/src/fd.cc",
             "compat/bionic/stdio_windows_lock.h", "compat/posix/src/stdio_stream_buffer.h"):
    rejected("unrelated protected bytes in " + name,
             {name: (ROOT / name).read_bytes() + b"\n// unqualified mutation\n"}, name)
name = FOLDER + "ofd-stream-source.json"
rejected("a modified original OFD receipt", {name: (ROOT / name).read_bytes() + b"\n"},
         "OFD source qualification changed")
name = FOLDER + "ofd-stream-source.patch"
rejected("a modified original OFD patch", {name: (ROOT / name).read_bytes() + b"\n"},
         "OFD transition patch changed")
print("SHARED_STDIO_SOURCE_POLICY_PASS exact_main_round_trip=4 original_protected_digests=71 negative_controls=" + str(checks))
