#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

if [[ $# -ne 2 || -z ${R8_JAR:-} ]]; then
    echo "usage: R8_JAR=/path/to/r8.jar $0 FRAMEWORK_CLASSES.jar OUTPUT_DIR" >&2
    exit 2
fi
framework=$1
out=$(mkdir -p "$2" && cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/classes" "$work/dex"
javac -source 17 -target 17 -Xlint:-options -XDstringConcat=inline \
    -cp "$framework" -d "$work/classes" "$here/RegistryProbe.java"
java -cp "$R8_JAR" com.android.tools.r8.D8 --release --min-api 36 \
    --output "$work/dex" "$work/classes/RegistryProbe.class"
cp "$work/dex/classes.dex" "$out/a03-registry.dex"
