#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: $0 SERIAL SIGNED_APK" >&2
    exit 2
fi
serial=$1
apk=$2
pkg=com.gesher.restorefixture
adb_on_device() { "${ADB:-adb}" -s "$serial" "$@"; }
installed=$(adb_on_device shell pm list packages "$pkg" | tr -d '\r')
if [[ $installed == *"package:$pkg"* ]]; then
    echo "The fixture is already installed on this device" >&2
    exit 2
fi
adb_on_device shell getprop ro.build.fingerprint
shasum -a 256 "$apk"
adb_on_device install "$apk"
trap 'adb_on_device uninstall "$pkg" >/dev/null' EXIT
started=$(adb_on_device shell date '+%m-%d\ %H:%M:%S.000' | tr -d '\r')
events() { adb_on_device logcat -d -T "$started" -s F03Restore:I; }
adb_on_device shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n "$pkg/.ParentActivity" -f 0x10000000
for attempt in {1..35}; do
    output=$(events)
    if [[ $output == *PARENT_SAVED* && $output == *CHILD_READY* ]]; then break; fi
    sleep 0.5
done
printf '%s\n' "$output"
[[ $output == *PARENT_SAVED* && $output == *PARENT_STOPPED* && $output == *CHILD_READY* ]]
parent=$(sed -n 's/.*PARENT_FIRST pid=\([0-9]*\).*/\1/p' <<< "$output" | tail -1)
child=$(sed -n 's/.*CHILD_READY pid=\([0-9]*\).*/\1/p' <<< "$output" | tail -1)
[[ $parent =~ ^[0-9]+$ && $child =~ ^[0-9]+$ && $parent != "$child" ]]
[[ $(adb_on_device shell pidof "$pkg" | tr -d '\r') == "$parent" ]]
[[ $(adb_on_device shell pidof "$pkg:child" | tr -d '\r') == "$child" ]]
adb_on_device shell run-as "$pkg" kill -9 "$parent"
for attempt in {1..20}; do
    if ! adb_on_device shell pidof "$pkg" >/dev/null; then break; fi
    sleep 0.2
done
! adb_on_device shell pidof "$pkg" >/dev/null
[[ $(adb_on_device shell pidof "$pkg:child" | tr -d '\r') == "$child" ]]
printf 'PARENT_PROCESS_EXITED old=%s childStillLive=%s\n' "$parent" "$child"
adb_on_device shell run-as "$pkg" touch files/release-result
for attempt in {1..35}; do
    output=$(events)
    if [[ $output == *'PASS restored task and view received the child result exactly once'* ]]; then break; fi
    sleep 0.5
done
sleep 1
output=$(events)
printf '%s\n' "$output"
[[ $output == *'PARENT_RESTORED '*valid=true*'PARENT_VIEW '*valid=true* || $output == *'PARENT_VIEW '*valid=true*'PARENT_RESTORED '*valid=true* ]]
[[ $(grep -c 'PARENT_RESULT request=702 valid=true' <<< "$output") -eq 1 ]]
[[ $(grep -c 'PASS restored task and view received the child result exactly once' <<< "$output") -eq 1 ]]
