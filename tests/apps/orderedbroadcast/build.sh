#!/usr/bin/env bash
# Two ordinary synthetic APKs exercising real cross-process ordered broadcasts.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
shift
for app in source target; do
  APP_DIR="$here/$app" bash "$here/../lifecycle/build.sh" "$out" "$@"
  mv "$out/$app.apk" "$out/orderedbroadcast-$app.apk"
done
