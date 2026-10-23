#!/usr/bin/env bash
# Assemble the boot class path's compiled classes as one java.base module
# directory (the "coremod") that javac --system=none --module-path takes,
# for tools/framework/build.py --core-classes.
#
#   tools/make-coremod.sh <boot work dir> <out-dir>
#
# <boot work dir> is tools/build-boot-jars.sh's WORK_DIR: its split/ holds
# core-oj, core-libart and core-icu4j, and classes/ the compiled module-info.
set -euo pipefail

work=$1
out=$2

rm -rf "$out"
mkdir -p "$out"
for jar in core-oj core-libart core-icu4j; do
  cp -r "$work/split/$jar/." "$out/"
done
cp "$work/classes/module-info.class" "$out/"
echo "coremod ready in $out ($(find "$out" -name '*.class' | wc -l) classes)"
