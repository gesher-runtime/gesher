#!/usr/bin/env python3
"""Check runtime DEX definitions against gesher-app's external Java contracts.

BootLinkCheck checks javac's output. R8 runs afterwards, so its actual DEX must
also retain the ABI used by framework.jar, native JNI, separate APKs and dynamic
dispatch. Internal implementation classes are deliberately outside this check.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import java_member_inventory as inventory

PUBLIC, PRIVATE, PROTECTED, STATIC, FINAL, NATIVE, INTERFACE, ABSTRACT = 1, 2, 4, 8, 16, 0x100, 0x200, 0x400

# Non-native Java callbacks looked up by FindClass/GetMethodID. Native methods
# are selected independently below. Other members of these classes may refer to
# internal implementation types whose names can legitimately change.
JNI_CALLBACKS = {
    "com.gesher.app.accessibility.UiaBridge": {"node", "nodeAt", "focused", "perform"},
    "com.gesher.app.input.WindowInput": {"onWindowEvent", "onTextEvent", "onDocumentEdit"},
    "com.gesher.app.webview.WebView2Host": {"onEvent", "onNavigationStarting", "onResourceRequest",
                                         "onBridgeCall", "onBridgeRequest", "onGlobal"},
    "com.gesher.app.window.WindowSession": {"onNativeDragEvent"},
    "com.gesher.server.AudioService": {"recordAudioAllowed"},
    "com.gesher.server.CameraFrames": {"deliver", "isReader"},
    "com.gesher.server.LegacyCamera": {
        "count", "info", "setup", "release", "check", "getParameters", "setParameters",
        "setPreviewSurface", "setPreviewTexture", "setHasPreviewCallback", "addCallbackBuffer",
        "previewEnabled", "startPreview", "stopPreview", "autoFocus", "cancelAutoFocus",
        "takePicture", "startSmoothZoom", "startFaceDetection", "setAudioRestriction", "getAudioRestriction",
    },
    "com.gesher.server.Zygote": {"fromWindowsPath", "windowsPath"},
}
ENTRY_POINTS = {
    "com.gesher.app.Main", "com.gesher.app.Launcher",
    "com.gesher.app.shell.AppShell", "com.gesher.server.SystemServer",
    "com.gesher.server.CameraSelection", "com.gesher.server.RuntimeCommands",
    "com.gesher.server.MicrogAddons",
}
SPEECH_APIS = ("com.gesher.server.WindowsSpeech", "com.gesher.server.WindowsSpeechRecognition")
PROVIDERS = ("com.gesher.app.AndroidKeyStore", "com.gesher.app.Digests")
OVERLAYS = ("android.", "org.ccil.cowan.tagsoup.",
            "com.android.internal.location.", "com.android.net.module.")


def family(name: str, roots: tuple[str, ...]) -> bool:
    return any(name == root or name.startswith(root + "$") for root in roots)


def fixture_contracts(path: Path) -> tuple[str, ...]:
    """Read the deliberately simple, reviewable fixture boundary keep list."""
    patterns = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"-keep(?:,includedescriptorclasses)? class ([\w.$*]+) \{ \*; \}", line)
        if not match:
            raise ValueError(f"unsupported fixture contract at {path}:{number}")
        pattern = match.group(1)
        if "*" in pattern and not (pattern.endswith((".**", "$**")) and "*" not in pattern[:-2]):
            raise ValueError(f"unsupported fixture class pattern at {path}:{number}")
        patterns.append(pattern)
    if not patterns:
        raise ValueError(f"empty fixture contract list: {path}")
    return tuple(patterns)


def fixture_class(name: str, patterns: tuple[str, ...]) -> bool:
    return any(name.startswith(pattern[:-2]) if pattern.endswith("**") else name == pattern
               for pattern in patterns)


def required_members(cls: inventory.Class, fixture_patterns: tuple[str, ...] = ()) -> dict:
    """Return only members whose unrenamed declarations are external contracts."""
    name = cls.name
    all_members = (name.startswith(OVERLAYS) or name.startswith("com.gesher.server.stub.")
                   or name.startswith("org.chromium.support_lib_glue.")
                   or fixture_class(name, fixture_patterns))
    public_api = family(name, SPEECH_APIS)
    delegates = bool(cls.flags & PUBLIC) and name.startswith("com.gesher.app.webview.SupportLib$")
    provider = family(name, PROVIDERS) and bool(cls.flags & PUBLIC)
    selected = {}
    for key, member in cls.members.items():
        if member.name == "<clinit>":
            continue
        keep = (all_members
                or public_api and bool(member.flags & (PUBLIC | PROTECTED))
                or delegates and member.kind == "method" and bool(member.flags & PUBLIC)
                or provider and key == ("method", "<init>", "()V")
                or name in ENTRY_POINTS and key == ("method", "main", "([Ljava/lang/String;)V")
                or member.kind == "method" and member.name in JNI_CALLBACKS.get(name, ())
                or name.startswith("com.gesher.") and bool(member.flags & NATIVE))
        if keep:
            selected[key] = member
    return selected


def check_contracts(before: dict, after: dict, fixture_patterns: tuple[str, ...] = ()) -> tuple[list[str], int, int]:
    failures, class_count, member_count = [], 0, 0
    contracts = {}
    for name, original in sorted(before.items()):
        required = required_members(original, fixture_patterns)
        class_contract = (name.startswith(OVERLAYS) or name.startswith("com.gesher.server.stub.")
                          or name.startswith("org.chromium.support_lib_glue.")
                          or family(name, SPEECH_APIS) or fixture_class(name, fixture_patterns))
        if not required and not class_contract:
            continue
        contracts[name] = required
    # A preserved signature also exposes its program-defined types to external
    # consumers. Keep their identity/hierarchy, without freezing their internal
    # methods. This matches includedescriptorclasses in the boundary rules.
    descriptor_types = {
        name
        for required in contracts.values()
        for member in required.values()
        for name in inventory.descriptor_classes(member.descriptor)
        if name in before
    }
    for name in sorted(descriptor_types):
        contracts.setdefault(name, {})
    original_hierarchy = inventory.Resolver(before)
    actual_hierarchy = inventory.Resolver(after)
    for name, required in contracts.items():
        original = before[name]
        class_count += 1
        member_count += len(required)
        actual = after.get(name)
        if actual is None:
            failures.append(f"missing class {name}")
            continue
        if original.flags & PUBLIC and not actual.flags & PUBLIC:
            failures.append(f"class no longer public: {name}")
        if bool(original.flags & INTERFACE) != bool(actual.flags & INTERFACE):
            failures.append(f"changed class/interface contract: {name}")
        if not original.flags & ABSTRACT and actual.flags & ABSTRACT:
            failures.append(f"class is now abstract: {name}")
        if not original.flags & FINAL and actual.flags & FINAL:
            failures.append(f"class is now final: {name}")
        parents, _ = original_hierarchy.ancestry(name)
        actual_parents, _ = actual_hierarchy.ancestry(name)
        # Internal helper supertypes may be renamed or merged. Public contracts
        # and library types must still be supertypes of the published class.
        for parent in sorted(parents - actual_parents):
            if parent != name and (parent in contracts or parent not in before):
                failures.append(f"lost superclass/interface {parent} from {name}")
        for key, expected in sorted(required.items()):
            got = actual.members.get(key)
            label = f"{name}.{expected.name}:{expected.descriptor}"
            if got is None:
                failures.append(f"missing {expected.kind} {label}")
                continue
            for issue in inventory.flag_issues(expected, got):
                failures.append(f"{issue}: {label}")
            if not expected.flags & (PUBLIC | PROTECTED | PRIVATE) and got.flags & PRIVATE:
                failures.append(f"package member is now private: {label}")
            if expected.flags & NATIVE and not got.flags & NATIVE:
                failures.append(f"JNI method no longer native: {label}")
    if not class_count:
        failures.append("no external contracts found in the compile-time input")
    return failures, class_count, member_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--classes", required=True, type=Path, help="unoptimized gesher-app-classes.jar")
    parser.add_argument("--dex", required=True, type=Path, help="actual runtime gesher-app.jar")
    parser.add_argument("--fixture-contracts", type=Path, default=ROOT / "app/proguard-fixture-contracts.pro",
                        help="reviewed ABI required by separate DEX/APK test consumers")
    args = parser.parse_args()
    before, _ = inventory.archive_members(args.classes.resolve(strict=True), "sdk", "gesher-app-classes")
    after, _ = inventory.archive_members(args.dex.resolve(strict=True), "boot", "gesher-app-runtime")
    failures, classes, members = check_contracts(before, after, fixture_contracts(args.fixture_contracts))
    for failure in failures:
        print(f"R8 contract: {failure}", file=sys.stderr)
    print(f"R8 contract check: {classes} classes, {members} members, {len(failures)} failures")
    return int(bool(failures))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (inventory.InvalidInput, OSError, ValueError) as error:
        print(f"R8 contract check: {error}", file=sys.stderr)
        raise SystemExit(1)
