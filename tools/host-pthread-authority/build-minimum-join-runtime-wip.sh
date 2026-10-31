#!/usr/bin/env bash
# WIP CI-only diagnostic supplier/fixture build. No product qualification.
# A separate immutable source gate and receipt verifier are mandatory first.
set -euo pipefail
test "${GITHUB_ACTIONS:-}" = true
test "${GITHUB_JOB:-}" = diagnostic-producer
test "${GESHER_MINIMUM_JOIN_RUNTIME_AUTHORIZED_SOURCE:-}" = "${GITHUB_SHA:?}"
root=$(git rev-parse --show-toplevel)
test "$(git rev-parse HEAD)" = "$GITHUB_SHA"
test -z "$(git status --porcelain --untracked-files=all)"
archive=$(realpath "${1:?official compiler archive}")
work="${RUNNER_TEMP:?}/join-runtime-${GITHUB_RUN_ID:?}-${GITHUB_RUN_ATTEMPT:?}"
test ! -e "$work"; mkdir -p "$work/toolchain"
printf '%s  %s\n' 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4 "$archive" | sha256sum --check --strict
tar -xJf "$archive" -C "$work/toolchain"
llvm="$work/toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
source="$work/source"
git init -q "$source"
git -C "$source" remote add origin https://github.com/mingw-w64/mingw-w64.git
git -C "$source" fetch --depth=1 origin a3d93999ef0521681d45e445c39964a7af0f593f
git -C "$source" checkout --detach FETCH_HEAD
python3 - "$root" "$source" <<'PY'
import hashlib,json,subprocess,sys
from pathlib import Path
root,src=map(Path,sys.argv[1:]); p=json.loads((root/'tools/host-pthread-authority/minimum-inputs.json').read_text())
assert subprocess.check_output(['git','-C',str(src),'rev-parse','HEAD']).decode().strip()==p['upstream_commit']
for name,expected in p['source_sha256'].items():
 actual=hashlib.sha256((src/name).read_bytes()).hexdigest();assert actual==expected;print('JOIN_RUNTIME_UPSTREAM',name,actual,flush=True)
for name,expected in p['ordered_patch_sha256'].items():assert hashlib.sha256((root/'tools/host-pthread-authority'/name).read_bytes()).hexdigest()==expected
PY
for patch in minimum-detached-retirement.patch minimum-noattach-join.patch minimum-join-wait-diagnostic.patch; do
 git -C "$source" apply --check "$root/tools/host-pthread-authority/$patch"
 git -C "$source" apply "$root/tools/host-pthread-authority/$patch"
 sha256sum "$root/tools/host-pthread-authority/$patch"
done
printf '%s  %s\n' f90a3275d5713751b3691dc4866831c746390e1784357a42285645d653ea71bf "$source/mingw-w64-libraries/winpthreads/src/thread.c" | sha256sum --check --strict
export PATH="$llvm/bin:$PATH"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir "$work/build";cd "$work/build"
supplier_cflags='-g -O2 -mguard=cf -Werror -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC'
"$source/mingw-w64-libraries/winpthreads/configure" --host=x86_64-w64-mingw32 \
 --prefix="$llvm/x86_64-w64-mingw32" --libdir="$llvm/x86_64-w64-mingw32/lib" \
 --enable-shared --enable-static CFLAGS="$supplier_cflags" CXXFLAGS='-g -O2 -mguard=cf -Werror' \
 CC="$llvm/bin/x86_64-w64-mingw32-clang" AR="$llvm/bin/llvm-ar" \
 RANLIB="$llvm/bin/llvm-ranlib" RC="$llvm/bin/x86_64-w64-mingw32-windres"
supplier_ldflags="-no-undefined -version-info 1:0:0 -L\$(builddir)/fakelib -Wl,-Map,$work/supplier-dll.map -Wl,--verbose"
make -j2 V=1 "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee "$work/build.log"
make install "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee "$work/install.log"
cd "$root"
out="$root/minimum-join-runtime-bundle";test ! -e "$out";mkdir "$out"
cc="$llvm/bin/x86_64-w64-mingw32-clang";lib="$llvm/x86_64-w64-mingw32/lib"
"$cc" --version
"$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
 -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC -c tests/native/host_pthread_minimum_join_cancel_candidate.c -o "$out/candidate.obj"
for mode in static dynamic; do
 if [ "$mode" = static ]; then library="$lib/libwinpthread.a";flags=(-pthread); else library="$lib/libwinpthread.dll.a";flags=();fi
 "$cc" -static "${flags[@]}" "$out/candidate.obj" "$library" -Wl,--verbose -Wl,-Map,"$out/$mode.map" -o "$out/$mode.exe" 2>&1 | tee "$out/$mode-link.txt"
 "$llvm/bin/llvm-readobj" --file-headers --coff-imports "$out/$mode.exe" > "$out/$mode-imports.txt"
 "$llvm/bin/llvm-nm" --defined-only "$out/$mode.exe" > "$out/$mode-symbols.txt"
 "$llvm/bin/llvm-objdump" --disassemble --reloc "$out/$mode.exe" > "$out/$mode-disassembly.txt"
done
cp "$llvm/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$out/libwinpthread-1.dll"
"$llvm/bin/llvm-readobj" --file-headers --coff-exports "$out/libwinpthread-1.dll" > "$out/dll-exports.txt"
"$llvm/bin/llvm-objdump" --disassemble --reloc "$out/libwinpthread-1.dll" > "$out/dll-disassembly.txt"
sha256sum "$cc" "$lib/libwinpthread.a" "$lib/libwinpthread.dll.a" "$out"/*
printf '%s\n' 'JOIN_RUNTIME_DIAGNOSTIC_BUILD_ONLY receipt_not_sealed=true target_wait_proved=false product_qualification=false'
