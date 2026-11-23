#!/usr/bin/env python3
"""Export explicit fixture ADB steps or parse an owner's raw controller result.

All mutations address five isolated fixture packages. Only initial cleanup may
fail. Results retain raw controller output and every notification polling sample.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re

PACKAGES = {name: "gesher.notificationdelegation." + name for name in
            ("owner", "delegate", "other", "sharedowner", "shareddelegate")}
PROPERTIES = ("ro.build.fingerprint", "ro.build.version.sdk", "ro.build.version.sdk_full",
              "ro.build.version.release", "ro.product.cpu.abilist")


def build_request(apks: Path) -> dict:
    apks = apks.resolve()
    steps: list[dict] = []

    def adb(label: str, *words: str, cleanup: bool = False, **metadata) -> None:
        step = {"label": label, "adb": list(words), "timeout": 90, **metadata}
        if cleanup: step["allow_failure"] = True
        steps.append(step)

    def call(name: str, command: str, label: str, target: str | None = None,
             id: int | None = None, await_id: int | None = None, present: bool = True) -> None:
        words = ["shell", "am", "broadcast", "--include-stopped-packages", "-n",
                 PACKAGES[name] + "/gesher.notificationdelegation.ControlReceiver",
                 "--es", "command", command]
        if target is not None: words += ["--es", "target", PACKAGES.get(target, target)]
        if id is not None: words += ["--ei", "id", str(id)]
        if await_id is not None:
            words += ["--ei", "awaitId", str(await_id), "--ez", "awaitPresent", str(present).lower()]
        adb(label, *words, fixture_package=PACKAGES[name], fixture_command=command)

    for key in PROPERTIES:
        adb("device-" + key, "shell", "getprop", key, device_property=key)
    hashes = []
    for name, package in PACKAGES.items():
        apk = apks / (name + ".apk")
        hashes.append({"name": name, "path": str(apk), "sha256": hashlib.sha256(apk.read_bytes()).hexdigest()})
        adb("initial-cleanup-" + name, "uninstall", package, cleanup=True)
        adb("install-" + name, "install", "-r", str(apk))
        adb("grant-" + name, "shell", "pm", "grant", package, "android.permission.POST_NOTIFICATIONS")
        call(name, "get", name + "-identity")
        call(name, "channel", name + "-channel")
    call("owner", "notify", "owner-self-post", id=1)
    call("owner", "active", "owner-self-post-visible", await_id=1)
    call("delegate", "notify", "unapproved-post", "owner", 2)
    call("owner", "set", "set-delegate", "delegate")
    call("delegate", "can", "delegate-allowed", "owner")
    call("other", "can", "other-denied", "owner")
    call("delegate", "channels", "delegate-channel-query", "owner")
    call("other", "channels", "other-channel-query", "owner")
    call("delegate", "notify", "delegate-post", "owner", 2)
    call("owner", "active", "owner-sees-delegated", await_id=2)
    call("delegate", "active", "delegate-sees-own-post", await_id=2)
    call("other", "active", "other-sees-none", await_id=2, present=False)
    call("delegate", "cancel", "delegate-cannot-cancel-owner", "owner", 1)
    call("delegate", "cancel", "delegate-cancels-own", "owner", 2)
    call("owner", "active", "owner-post-remains", await_id=2, present=False)
    call("delegate", "notify", "delegate-post-before-revoke", "owner", 3)
    call("owner", "active", "pre-revoke-post-visible", await_id=3)
    call("owner", "set", "revoke-delegate")
    call("delegate", "can", "revoked-cannot-notify", "owner")
    call("delegate", "active", "revoked-post-still-queryable", await_id=3)
    call("delegate", "cancel", "revoked-cancel-denied", "owner", 3)
    call("owner", "cancel", "owner-cancels-delegated", id=3)
    call("owner", "active", "owner-cancel-observed", await_id=3, present=False)
    call("owner", "set", "restore-delegate", "delegate")
    call("owner", "set", "unknown-delegate-keeps-current", "gesher.notificationdelegation.absent")
    call("owner", "get", "unknown-delegate-get")
    adb("revoke-delegate-post-permission", "shell", "pm", "revoke", PACKAGES["delegate"], "android.permission.POST_NOTIFICATIONS")
    call("delegate", "notify", "delegate-own-permission-not-required", "owner", 4)
    call("owner", "active", "owner-sees-post-from-denied-delegate", await_id=4)
    adb("revoke-owner-post-permission", "shell", "pm", "revoke", PACKAGES["owner"], "android.permission.POST_NOTIFICATIONS")
    call("delegate", "notify", "owner-permission-required", "owner", 5)
    call("owner", "active", "owner-permission-revoked-active", await_id=5, present=False)
    adb("restore-owner-post-permission", "shell", "pm", "grant", PACKAGES["owner"], "android.permission.POST_NOTIFICATIONS")
    adb("stop-delegate-process", "shell", "am", "force-stop", PACKAGES["delegate"])
    call("delegate", "can", "grant-survives-process-stop", "owner")
    adb("uninstall-delegate", "uninstall", PACKAGES["delegate"])
    call("owner", "get", "delegate-uninstalled-name")
    adb("reinstall-delegate", "install", str(apks / "delegate.apk"))
    call("delegate", "get", "delegate-reinstall-identity")
    call("delegate", "can", "delegate-reinstall-authorization", "owner")
    call("owner", "set", "delegate-reinstall-regrant", "delegate")
    call("delegate", "can", "delegate-reinstall-allowed-after-regrant", "owner")
    call("shareddelegate", "can", "same-uid-can-without-grant", "sharedowner")
    call("shareddelegate", "notify", "same-uid-post-without-grant", "sharedowner", 30)
    call("sharedowner", "active", "same-uid-owner-active", await_id=30)
    call("shareddelegate", "channels", "same-uid-channel-query", "sharedowner")
    adb("uninstall-owner", "uninstall", PACKAGES["owner"])
    adb("reinstall-owner", "install", str(apks / "owner.apk"))
    call("owner", "get", "owner-reinstall-clears-delegate")
    call("delegate", "can", "owner-reinstall-authorization", "owner")
    return {"schema": "gesher-notification-delegation-adb-v1", "apks": hashes,
            "absence_caveat": "Bounded absence windows retain timing uncertainty; presence uses predicate polling.", "steps": steps}


def parse_result(request: dict, raw: dict) -> dict:
    observations, errors, inconclusive, device = [], [], [], {}
    steps, results = request["steps"], raw.get("results", [])
    if raw.get("requested_steps") != len(steps):
        errors.append("Controller requested_steps differs from exported request")
    if len(results) != len(steps): errors.append(f"Controller executed {len(results)} of {len(steps)} steps")
    for index, (step, event) in enumerate(zip(steps, results)):
        label = step["label"]
        if event.get("argv") != step["adb"]:
            errors.append(f"Step {index} ({label}): command differs from exported request")
            continue
        if event.get("exit_code") != 0 and not step.get("allow_failure", False):
            errors.append(f"Step {index} ({label}): ADB exit {event.get('exit_code')}")
            continue
        output = event.get("output", "")
        if "device_property" in step: device[step["device_property"]] = output.strip()
        if "fixture_command" not in step: continue
        match = re.search(r'data="F32_DELEGATION:(.*)"\s*$', output, re.MULTILINE)
        if match is None:
            errors.append(f"Step {index} ({label}): missing fixture JSON result")
            continue
        try: value = json.loads(match.group(1))
        except json.JSONDecodeError as failure:
            errors.append(f"Step {index} ({label}): malformed fixture JSON: {failure}")
            continue
        if value.get("package") != step["fixture_package"] or value.get("command") != step["fixture_command"]:
            errors.append(f"Step {index} ({label}): fixture identity or command mismatch")
        if value.get("predicateObserved") is False:
            inconclusive.append({"label": label, "reason": "Notification polling deadline reached"})
        observations.append({"label": label, "step": index, "value": value})
    return {"schema": "gesher-notification-delegation-observations-v1", "completed": not errors,
            "device": device, "apks": request["apks"], "observations": observations, "errors": errors,
            "inconclusive": inconclusive, "absence_caveat": request["absence_caveat"],
            "request": request, "raw_controller_result": raw}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("export")
    export.add_argument("--apks", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)
    parse = sub.add_parser("parse")
    parse.add_argument("--request", type=Path, required=True)
    parse.add_argument("--result", type=Path, required=True)
    parse.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = (build_request(args.apks) if args.command == "export" else
              parse_result(json.loads(args.request.read_text()), json.loads(args.result.read_text())))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(args.output)
    if args.command == "parse" and not result["completed"]: raise SystemExit(1)


if __name__ == "__main__": main()
