#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -ne 2 || -z $1 ]]; then
    echo "usage: $0 SERIAL SIGNED_APK" >&2
    exit 2
fi
serial=$1
apk=$2
package=org.gesher.contracts.storagemigration
device() { "${ADB:-adb}" -s "$serial" "$@"; }

installed=$(device shell pm list packages "$package" | tr -d '\r')
if [[ "$installed" == *"package:$package"* ]]; then
    echo "The fixture is already installed on this device" >&2
    exit 2
fi
device shell getprop ro.build.fingerprint
shasum -a 256 "$apk"
device install "$apk"
trap 'device uninstall "$package" >/dev/null' EXIT
started=$(device shell date '+%m-%d\ %H:%M:%S.000' | tr -d '\r')
pids=" "
token=
for step in ce-to-de de-to-ce verified; do
    device shell am start -W -n "$package/.MigrationActivity"
    for attempt in {1..40}; do
        output=$(device logcat -d -T "$started" -s StorageMigration:I)
        if [[ $output == *"StorageMigration: PASS step=$step "* ||
                $output == *'StorageMigration: FAIL '* ]]; then break; fi
        sleep 0.5
    done
    printf '%s\n' "$output"
    [[ $output != *'StorageMigration: FAIL '* ]] || exit 1
    result=$(printf '%s\n' "$output" |
        sed -n "s/.*StorageMigration: PASS step=$step pid=\([0-9]*\) fixture-id=\([a-f0-9-]*\).*/\1 \2/p")
    if [[ ! $result =~ ^([0-9]+)\ ([a-f0-9-]+)$ ]]; then
        echo "Missing or duplicate result for $step" >&2
        exit 1
    fi
    pid=${BASH_REMATCH[1]}
    current_token=${BASH_REMATCH[2]}
    [[ $pids != *" $pid "* && ( -z $token || $token == "$current_token" ) ]] || exit 1
    pids+="$pid "
    token=$current_token
    device shell am force-stop "$package"
done
echo "Android signed storage migration verified across three processes"
