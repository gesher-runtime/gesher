#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 SDK_ROOT OUT_DIR" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(cd "$1" && pwd)
out=$(mkdir -p "$2" && cd "$2" && pwd)
platform=$sdk/platforms/android-36/android.jar
tools=$sdk/build-tools/36.0.0
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

python3 - "$platform" "$out/sdk36-classes.txt" <<'PY'
from pathlib import Path
import sys
from zipfile import ZipFile

with ZipFile(sys.argv[1]) as jar:
    names = [name[:-6].replace("/", ".") for name in jar.namelist()
             if name.endswith(".class") and not name.startswith("META-INF/")]
if not names or len(names) != len(set(names)) or any("\n" in name for name in names):
    raise SystemExit("Invalid SDK class list")
Path(sys.argv[2]).write_text("\n".join(sorted(names)) + "\n", encoding="utf-8")
PY
javac -source 8 -target 8 -Xlint:-options -bootclasspath "$platform" \
    -d "$build" "$here/BootClassCensus.java"
"$tools/d8" --min-api 36 --output "$out" "$build/BootClassCensus.class"
mv "$out/classes.dex" "$out/boot-census.dex"
