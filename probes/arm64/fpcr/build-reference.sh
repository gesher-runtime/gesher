#!/usr/bin/env bash
# Compile the original pinned body in CI as actual guest AArch64 code. Only
# symbol names change, so it coexists with the production shim under test.
set -euo pipefail
cc=$1
output=$2
reference_dir=$(cd "$(dirname "$0")" && pwd)
defines=(-D__fe_dfl_env=arm64_reference_default)
for name in fegetenv fesetenv feclearexcept fegetexceptflag fesetexceptflag \
            feraiseexcept fetestexcept fegetround fesetround feholdexcept \
            feupdateenv feenableexcept fedisableexcept fegetexcept; do
  defines+=("-D$name=arm64_reference_$name")
done
"$cc" -O2 -fPIC -Wall -Wextra -Werror "${defines[@]}" \
  -c "$reference_dir/upstream/fenv-arm64.c" -o "$output"
