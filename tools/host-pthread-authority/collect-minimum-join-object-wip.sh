#!/usr/bin/env bash
# Source-only WIP for a future separately authorized CI disassembly collector.
# This does not execute a native fixture or claim a wait/cancellation result.
set -euo pipefail
test "${GITHUB_ACTIONS:-}" = true
test "${GITHUB_JOB:-}" = diagnostic-producer
test "${GESHER_MINIMUM_JOIN_COLLECT_AUTHORIZED_SOURCE:-}" = "${GITHUB_SHA:?}"
root=$(git rev-parse --show-toplevel)
archive=$(realpath "${1:?pristine official compiler archive}")
work="${RUNNER_TEMP:?}/minimum-join-collector-${GITHUB_RUN_ID:?}-${GITHUB_RUN_ATTEMPT:?}"
test ! -e "$work"
mkdir "$work"
printf '%s  %s\n' 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4 "$archive" | sha256sum --check --strict
mkdir "$work/toolchain"
tar -xJf "$archive" -C "$work/toolchain"
llvm="$work/toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
test -x "$llvm/bin/x86_64-w64-mingw32-clang"
source="$work/source"
git init -q "$source"
git -C "$source" remote add origin https://github.com/mingw-w64/mingw-w64.git
git -C "$source" fetch --depth=1 origin a3d93999ef0521681d45e445c39964a7af0f593f
git -C "$source" checkout --detach FETCH_HEAD
python3 - "$root" "$source" <<'PY'
import hashlib,json,subprocess,sys
from pathlib import Path
root,source=map(Path,sys.argv[1:]); data=json.loads((root/'tools/host-pthread-authority/minimum-inputs.json').read_text())
assert subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD']).decode().strip()==data['upstream_commit']
for name,expected in data['source_sha256'].items():
 raw=(source/name).read_bytes();actual=hashlib.sha256(raw).hexdigest();assert actual==expected,(name,actual,expected)
 print('JOIN_DIAG_UPSTREAM_SOURCE',name,actual,flush=True)
for name,expected in data['ordered_patch_sha256'].items():
 raw=(root/'tools/host-pthread-authority'/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==expected
PY
for patch in minimum-detached-retirement.patch minimum-noattach-join.patch minimum-join-wait-diagnostic.patch; do
  git -C "$source" apply --check "$root/tools/host-pthread-authority/$patch"
  git -C "$source" apply "$root/tools/host-pthread-authority/$patch"
  sha256sum "$root/tools/host-pthread-authority/$patch"
done
printf '%s  %s\n' dd3f2c90ebb6b7ea22588c1548e7675332b1606745281120dd95189b7fd37541 "$source/mingw-w64-libraries/winpthreads/src/thread.c" | sha256sum --check --strict
mkdir "$work/build"
cd "$work/build"
export PATH="$llvm/bin:$PATH"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
"$source/mingw-w64-libraries/winpthreads/configure" --host=x86_64-w64-mingw32 --prefix="$work/install" \
  CC="$llvm/bin/x86_64-w64-mingw32-clang" AR="$llvm/bin/llvm-ar" RANLIB="$llvm/bin/llvm-ranlib" \
  RC="$llvm/bin/x86_64-w64-mingw32-windres"
compiler="$llvm/bin/x86_64-w64-mingw32-clang"
sha256sum "$compiler" "$llvm/bin/llvm-objdump" "$llvm/bin/llvm-readobj"
"$compiler" --version
"$compiler" -DHAVE_CONFIG_H -I. -I"$source/mingw-w64-libraries/winpthreads" \
  -I"$source/mingw-w64-libraries/winpthreads/include" -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
  -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC -Wall -g -O2 -mguard=cf -Werror \
  -c "$source/mingw-w64-libraries/winpthreads/src/thread.c" -o diagnostic-thread.obj
sha256sum diagnostic-thread.obj
"$llvm/bin/llvm-readobj" --file-headers --symbols --relocations diagnostic-thread.obj
"$llvm/bin/llvm-objdump" --disassemble --reloc --demangle diagnostic-thread.obj
printf '%s\n' 'JOIN_DIAG_OBJECT_COLLECTION_ONLY runtime_executed=false target_wait_proved=false product_qualification=false'
