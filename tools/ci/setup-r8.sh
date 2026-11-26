#!/usr/bin/env bash
# Share the verified D8 compiler among the builds in this Actions job.
set -euo pipefail

r8_jar="$RUNNER_TEMP/gesher-r8.jar"
curl -fsSL --retry 5 --retry-all-errors --retry-delay 10 -o "$r8_jar.download" \
  https://dl.google.com/android/maven2/com/android/tools/r8/9.5.26/r8-9.5.26.jar
printf '%s  %s\n' 3502f087388ce95bbf5254555927f09385dc2636873964d0bd53f3eaf4fb49ad "$r8_jar.download" \
  | sha256sum --check --strict
mv "$r8_jar.download" "$r8_jar"
printf 'R8_JAR=%s\n' "$r8_jar" >> "$GITHUB_ENV"
