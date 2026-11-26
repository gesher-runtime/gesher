#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 SERIAL APK_DIR PORT baseline|api36-opt-in|api37" >&2
    exit 2
fi
serial=$1
out=$2
port=$3
mode=$4
run=$(python3 -c 'import time; print(time.time_ns())')
adb_on_device() { "${ADB:-adb}" -s "$serial" "$@"; }

probe() {
    local target=$1 phase=$2 expected=$3
    local label="$run-$phase" pkg="org.gesher.contracts.lan.t$target" output
    adb_on_device shell am force-stop "$pkg"
    adb_on_device shell am start -W -n "$pkg/org.gesher.contracts.lan.LanProbe" \
        --es host 10.0.2.2 --ei port "$port" --es label "$label"
    for attempt in {1..15}; do
        output=$(adb_on_device logcat -d -s GESHER_LAN_PROBE:I | grep -F "$label " || true)
        if [[ "$output" == *"$label complete"* ]]; then
            printf '%s\n' "$output"
            if [[ $expected == allowed ]]; then
                [[ "$output" == *"$label tcp echo-ok"* && "$output" == *"$label udp echo-ok"* ]]
            else
                [[ "$output" == *"$label tcp SocketTimeoutException"* && \
                    "$output" == *"$label udp IOException"*EPERM* ]]
            fi
            return
        fi
        sleep 1
    done
    printf '%s\n' "$output"
    return 1
}

adb_on_device shell getprop ro.build.fingerprint
case $mode in
    baseline)
        adb_on_device install -r "$out/target-36.apk"
        probe 36 baseline allowed
        ;;
    api36-opt-in)
        pkg=org.gesher.contracts.lan.t36
        permission=android.permission.NEARBY_WIFI_DEVICES
        # Enable the documented compat change and reboot the disposable emulator beforehand.
        trap 'adb_on_device shell pm revoke "$pkg" "$permission"; adb_on_device shell am compat reset RESTRICT_LOCAL_NETWORK "$pkg"' EXIT
        adb_on_device shell pm revoke "$pkg" "$permission"
        probe 36 opt-in-denied blocked
        adb_on_device shell pm grant "$pkg" "$permission"
        probe 36 opt-in-granted allowed
        adb_on_device shell pm revoke "$pkg" "$permission"
        probe 36 opt-in-revoked blocked
        ;;
    api37)
        adb_on_device install -r "$out/target-36.apk"
        adb_on_device install -r "$out/target-37.apk"
        pkg=org.gesher.contracts.lan.t37
        permission=android.permission.ACCESS_LOCAL_NETWORK
        trap 'adb_on_device shell pm revoke "$pkg" "$permission"' EXIT
        adb_on_device shell pm revoke "$pkg" "$permission"
        probe 36 legacy-default allowed
        probe 37 target37-default blocked
        adb_on_device shell pm grant "$pkg" "$permission"
        probe 37 target37-granted allowed
        adb_on_device shell pm revoke "$pkg" "$permission"
        probe 37 target37-revoked blocked
        ;;
    *) echo "unknown mode: $mode" >&2; exit 2 ;;
esac
