#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: $0 SERIAL SIGNED_APK" >&2
    exit 2
fi
serial=$1
apk=$2
pkg=com.gesher.launchreset
device() { "${ADB:-adb}" -s "$serial" "$@"; }
if device shell pm path "$pkg" 2>/dev/null | grep '^package:' >/dev/null; then
    echo "The fixture is already installed on this device" >&2
    exit 2
fi
work=$(mktemp -d)
remote_ui=/data/local/tmp/gesher-launch-reset-$(basename "$work").xml
installed=false
cleanup() {
    device shell rm -f "$remote_ui" >/dev/null || true
    if [[ $installed == true ]]; then device uninstall "$pkg" >/dev/null || true; fi
    rm -rf -- "$work"
}
trap cleanup EXIT

# The stock portrait Launcher3 home screen opens its app drawer with this swipe.
size=$(device shell wm size | tr -d '\r' | tail -1)
if [[ ! $size =~ ([0-9]+)x([0-9]+)$ ]]; then
    echo "Unknown Android screen size: $size" >&2
    exit 1
fi
width=${BASH_REMATCH[1]}
height=${BASH_REMATCH[2]}
tap_app_icon() {
    device shell input keyevent KEYCODE_HOME
    sleep 0.4
    device shell input swipe "$((width / 2))" "$((height * 9 / 10))" \
        "$((width / 2))" "$((height / 5))" 300
    device shell uiautomator dump "$remote_ui" >/dev/null
    device exec-out cat "$remote_ui" > "$work/drawer.xml"
    local coords x y
    coords=$(python3 - "$work/drawer.xml" <<'ANDROID_PY'
import re
import sys
from xml.etree.ElementTree import parse

for node in parse(sys.argv[1]).iter("node"):
    if node.get("text") == "Gesher Reset Probe" and node.get("clickable") == "true":
        x1, y1, x2, y2 = map(int, re.findall(r"\d+", node.attrib["bounds"]))
        print((x1 + x2) // 2, (y1 + y2) // 2)
        break
else:
    raise SystemExit("Fixture app icon is absent from the Android app drawer")
ANDROID_PY
)
    read -r x y <<< "$coords"
    device shell input tap "$x" "$y"
}

events() { device logcat -d -T "$started" -s F01LauncherReset:I; }
require_count() {
    local pattern=$1 expected=$2 output count
    for attempt in {1..60}; do
        output=$(events)
        count=$(grep -cE "$pattern" <<< "$output" || true)
        if [[ $count == "$expected" ]]; then return; fi
        sleep 0.25
    done
    printf '%s\n' "$output" >&2
    printf 'Expected %s occurrences of %s, saw %s\n' "$expected" "$pattern" "$count" >&2
    return 1
}
device shell getprop ro.build.fingerprint
shasum -a 256 "$apk"
device install "$apk"
installed=true
started=$(device shell date '+%m-%d\ %H:%M:%S.000' | tr -d '\r')

tap_app_icon
require_count 'CHILD_RESUME task=' 1
device shell input keyevent KEYCODE_HOME
sleep 0.4
device shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER \
    -n "$pkg/.LaunchRoot" -f 0x10000000
require_count 'CHILD_RESUME task=' 2
events > "$work/before.log"
tap_app_icon
require_count 'CHILD_DESTROY task=' 1
require_count 'ROOT_RESUME count=2 task=' 1
events > "$work/after.log"
cat "$work/after.log"
python3 - "$work" <<'ANDROID_PY'
import re
import sys
from pathlib import Path

work = Path(sys.argv[1])
before = (work / "before.log").read_text()
after = (work / "after.log").read_text()
create = re.search(r"ROOT_CREATE count=1 task=(\d+) flags=10200000", before)
if not create:
    raise SystemExit("The app icon did not create the root with task reset flags")
task = create.group(1)
if (len(re.findall(rf"CHILD_RESUME task={task}\b", before)) != 2
        or "ROOT_RESUME count=2" in before or "CHILD_DESTROY" in before):
    raise SystemExit("The explicit launch without reset did not retain the child")
if (after.count("ROOT_CREATE ") != 1
        or not re.search(rf"ROOT_RESUME count=2 task={task}\b", after)
        or not re.search(rf"CHILD_DESTROY task={task} finishing=true\b", after)):
    raise SystemExit("The second app icon launch did not reset the same task")
print("Android launcher reset verified")
ANDROID_PY
