#!/usr/bin/env bash
# Build net-tests.dex: tests/dex-net (docs/network.md), plain Java (java.net,
# javax.net.ssl), run by tests/windows/tracks/56-net.ps1.
#
#   tests/dex-net/build.sh <out-dir> <r8.jar>
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
r8=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

javac -encoding UTF-8 --release 11 -d "$work/classes" "$here"/*.java
mkdir -p "$work/dex"
java -cp "$r8" com.android.tools.r8.D8 --release --min-api 26 \
  --output "$work/dex" $(find "$work/classes" -name '*.class')
cp "$work/dex/classes.dex" "$out/net-tests.dex"
echo "net-tests.dex written to $out"
