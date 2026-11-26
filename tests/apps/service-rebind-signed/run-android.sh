#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: $0 SERIAL SIGNED_APK" >&2
    exit 2
fi
serial=$1
apk=$2
pkg=gesher.contract.svcdeath
adb_on_device() { "${ADB:-adb}" -s "$serial" "$@"; }
installed=$(adb_on_device shell pm list packages "$pkg" | tr -d '\r')
if grep -Fxq "package:$pkg" <<< "$installed"; then
    echo "The fixture is already installed on this device" >&2
    exit 2
fi
adb_on_device shell getprop ro.build.fingerprint
shasum -a 256 "$apk"
adb_on_device install "$apk"
trap 'adb_on_device uninstall "$pkg" >/dev/null' EXIT
started=$(adb_on_device shell date '+%m-%d\ %H:%M:%S.000' | tr -d '\r')
adb_on_device shell am start -W -a android.intent.action.MAIN \
    -c android.intent.category.LAUNCHER -n "$pkg/.Probe" -f 0x10000000
for attempt in {1..40}; do
    output=$(adb_on_device logcat -d -T "$started" -s ServiceDeathProbe:I)
    if [[ $output == *'ServiceDeathProbe: PASS '* || $output == *'ServiceDeathProbe: FAIL '* ]]; then break; fi
    sleep 0.5
done
printf '%s\n' "$output"
[[ $output == *'ServiceDeathProbe: PASS connected=2 disconnected=1 unexpected=0'* ]]
[[ $output != *'ServiceDeathProbe: UNEXPECTED_'* && $output != *'ServiceDeathProbe: FAIL '* ]]
