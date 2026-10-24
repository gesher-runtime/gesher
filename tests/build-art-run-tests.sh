#!/usr/bin/env bash
# A subset of ART's own run-tests (art/test/NNN-*), the ones that exercise the
# code generators and need no native library of their own, compiled to DEX
# for tracks/70-codegen.ps1 (docs/compiled-code.md):
#
#   tests/build-art-run-tests.sh <out-dir> <r8.jar>
#
# Writes <out-dir>/art-<test>.dex and art-<test>.expected-stdout.txt.
# A test that does not compile is reported and left out.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
r8=$2
aosp=${AOSP_DIR:-$root/third_party/aosp}
tests=$aosp/art/test
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for t in 003-omnibus-opcodes 008-exceptions 021-string2 046-reflect 061-out-of-memory \
         080-oom-throw 083-compiler-regressions 114-ParallelGC 123-compiler-regressions-mt \
         201-built-in-except-detail-messages; do
  dir=$tests/$t
  if [[ ! -d $dir/src ]]; then
    echo "art run-tests: $t: no sources, skipped"
    continue
  fi
  mkdir -p "$work/$t/classes" "$work/$t/dex"
  srcs=$(find "$dir/src" ${dir}/src-multidex -name '*.java' 2>/dev/null || true)
  if ! javac -encoding UTF-8 --release 11 -nowarn -proc:none -d "$work/$t/classes" $srcs \
      > "$work/$t/javac.log" 2>&1; then
    echo "art run-tests: $t: javac failed, skipped"
    tail -5 "$work/$t/javac.log"
    continue
  fi
  # As the test's own javac_post.sh does.
  if [[ $t == 003-omnibus-opcodes ]]; then rm -f "$work/$t/classes/UnresClass.class"; fi
  # --debug keeps line numbers exact (some tests print stack traces).
  if ! java -cp "$r8" com.android.tools.r8.D8 --debug --min-api 26 --output "$work/$t/dex" \
      $(find "$work/$t/classes" -name '*.class') > "$work/$t/d8.log" 2>&1; then
    echo "art run-tests: $t: d8 failed, skipped"
    tail -5 "$work/$t/d8.log"
    continue
  fi
  cp "$work/$t/dex/classes.dex" "$out/art-$t.dex"
  cp "$dir/expected-stdout.txt" "$out/art-$t.expected-stdout.txt"
  echo "art run-tests: $t"
done
