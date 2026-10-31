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
python3 -I -B tools/host-pthread-authority/minimum_join_runtime_authority_wip.py diagnostic-producer
archive=$(realpath "${1:?official compiler archive}")
work="${RUNNER_TEMP:?}/join-runtime-${GITHUB_RUN_ID:?}-${GITHUB_RUN_ATTEMPT:?}"
test ! -e "$work"; mkdir -p "$work/toolchain"
printf '%s  %s\n' 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4 "$archive" | sha256sum --check --strict
tar -xJf "$archive" -C "$work/toolchain"
llvm="$work/toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64"
test -x "$llvm/bin/x86_64-w64-mingw32-clang"
printf '%s  %s\n' fa2a3887fbae6c61ee3c20027101dbe71492c1c2d63ab178746b16777cff052b "$llvm/x86_64-w64-mingw32/include/processthreadsapi.h" | sha256sum --check --strict
# Snapshot actual pristine publisher DLL surface before the diagnostic rebuild.
"$llvm/bin/llvm-readobj" --coff-exports "$llvm/x86_64-w64-mingw32/bin/libwinpthread-1.dll" > "$work/publisher-dll-exports.txt"
"$llvm/bin/llvm-readobj" --coff-imports "$llvm/x86_64-w64-mingw32/bin/libwinpthread-1.dll" > "$work/publisher-dll-imports.txt"
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
python3 - "$source" "$llvm" "$work/publisher-headers.json" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
src,llvm,out=map(Path,sys.argv[1:]);source=src/'mingw-w64-libraries/winpthreads/include';dest=llvm/'x86_64-w64-mingw32/include'
headers={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in source.glob('*.h')}
assert len(headers)==7 and all(hashlib.sha256((dest/name).read_bytes()).hexdigest()==value for name,value in headers.items())
out.write_text(json.dumps(headers,sort_keys=True,indent=2)+'\n')
PYCODE
for patch in minimum-detached-retirement.patch minimum-noattach-join.patch minimum-join-wait-diagnostic.patch; do
 git -C "$source" apply --check "$root/tools/host-pthread-authority/$patch"
 git -C "$source" apply "$root/tools/host-pthread-authority/$patch"
 sha256sum "$root/tools/host-pthread-authority/$patch"
done
printf '%s  %s\n' f90a3275d5713751b3691dc4866831c746390e1784357a42285645d653ea71bf "$source/mingw-w64-libraries/winpthreads/src/thread.c" | sha256sum --check --strict
export PATH="$llvm/bin:$PATH"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir "$work/build";cd "$work/build"
supplier_cflags='-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC'
"$source/mingw-w64-libraries/winpthreads/configure" --host=x86_64-w64-mingw32 \
 --prefix="$llvm/x86_64-w64-mingw32" --libdir="$llvm/x86_64-w64-mingw32/lib" \
 --enable-shared --enable-static CFLAGS="$supplier_cflags" CXXFLAGS='-g -O2 -mguard=cf -Werror' \
 CC="$llvm/bin/x86_64-w64-mingw32-clang" AR="$llvm/bin/llvm-ar" \
 RANLIB="$llvm/bin/llvm-ranlib" RC="$llvm/bin/x86_64-w64-mingw32-windres" 2>&1 | tee "$work/configure.log"
cat > "$work/sdk-profile.c" <<'PROFILE'
#define __LARGE_MBSTATE_T
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
DWORD (WINAPI *minimum_getthreadid_declaration)(HANDLE)=GetThreadId;
PROFILE
"$llvm/bin/x86_64-w64-mingw32-clang" -dM -E "$work/sdk-profile.c" > "$work/sdk-macros.txt"
"$llvm/bin/x86_64-w64-mingw32-clang" -DHAVE_CONFIG_H -I. -I"$source/mingw-w64-libraries/winpthreads" \
 -I"$source/mingw-w64-libraries/winpthreads/include" -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
 -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC -dM -E "$work/sdk-profile.c" > "$work/supplier-macros.txt"
"$llvm/bin/x86_64-w64-mingw32-clang" -Wall -Wextra -Werror -c "$work/sdk-profile.c" -o "$work/sdk-profile.obj"
"$llvm/bin/x86_64-w64-mingw32-clang" -DHAVE_CONFIG_H -I. -I"$source/mingw-w64-libraries/winpthreads" \
 -I"$source/mingw-w64-libraries/winpthreads/include" -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
 -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC -Wall -Wextra -Werror -c "$work/sdk-profile.c" -o "$work/supplier-profile.obj"
"$llvm/bin/llvm-readobj" --file-headers "$work/sdk-profile.obj" > "$work/sdk-machine.txt"
"$llvm/bin/llvm-readobj" --file-headers "$work/supplier-profile.obj" > "$work/supplier-machine.txt"
supplier_ldflags="-no-undefined -version-info 1:0:0 -L\$(builddir)/fakelib -Wl,-Map,$work/supplier-dll.map -Wl,--verbose"
make -j2 V=1 "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee "$work/build.log"
make install "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee "$work/install.log"
cd "$root"
out="$root/minimum-join-runtime-bundle";test ! -e "$out";mkdir "$out"
cc="$llvm/bin/x86_64-w64-mingw32-clang";lib="$llvm/x86_64-w64-mingw32/lib"
"$cc" --version
"$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
 -DGESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC -c tests/native/host_pthread_minimum_join_cancel_candidate.c -o "$out/candidate.obj"
"$llvm/bin/llvm-nm" --undefined-only "$out/candidate.obj" > "$out/candidate-undefined.txt"
cp "$lib/libwinpthread.a" "$out/libwinpthread.a"
cp "$lib/libwinpthread.dll.a" "$out/libwinpthread.dll.a"
"$llvm/bin/llvm-nm" --defined-only "$out/libwinpthread.dll.a" > "$out/import-archive-symbols.txt"
python3 - "$lib" "$out" <<'PYCODE'
import hashlib,json,os,sys
from pathlib import Path
lib,out=map(Path,sys.argv[1:])
archives={}
for name in ('libwinpthread.a','libwinpthread.dll.a'):
 path=(lib/name).resolve(strict=True);raw=path.read_bytes()
 assert hashlib.sha256(raw).digest()==hashlib.sha256((out/name).read_bytes()).digest()
 archives[name]={'path':str(path),'sha256':hashlib.sha256(raw).hexdigest()}
value={'source':os.environ['GITHUB_SHA'],'run':os.environ['GITHUB_RUN_ID'],
       'attempt':os.environ['GITHUB_RUN_ATTEMPT'],'role':'join-wait-diagnostic-only-v1',
       'archives':archives,'sealed':False}
(out/'producer-archive-observations.json').write_text(json.dumps(value,sort_keys=True,indent=2)+'\n')
PYCODE
for mode in static dynamic; do
 if [ "$mode" = static ]; then library="$lib/libwinpthread.a";flags=(-pthread); else library="$lib/libwinpthread.dll.a";flags=();fi
 "$cc" -static "${flags[@]}" "$out/candidate.obj" "$library" -Wl,--verbose -Wl,-Map,"$out/$mode.map" -o "$out/$mode.exe" 2>&1 | tee "$out/$mode-link.txt"
 "$llvm/bin/llvm-readobj" --file-headers --coff-imports "$out/$mode.exe" > "$out/$mode-imports.txt"
 "$llvm/bin/llvm-nm" --defined-only "$out/$mode.exe" > "$out/$mode-symbols.txt"
 "$llvm/bin/llvm-objdump" --disassemble --reloc "$out/$mode.exe" > "$out/$mode-disassembly.txt"
done
cp "$llvm/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$out/libwinpthread-1.dll"
"$llvm/bin/llvm-readobj" --coff-exports "$out/libwinpthread-1.dll" > "$out/dll-exports.txt"
"$llvm/bin/llvm-readobj" --file-headers "$out/libwinpthread-1.dll" > "$out/dll-machine.txt"
"$llvm/bin/llvm-readobj" --coff-imports "$out/libwinpthread-1.dll" > "$out/dll-imports.txt"
cp "$work/publisher-dll-exports.txt" "$out/publisher-dll-exports.txt"
cp "$work/publisher-dll-imports.txt" "$out/publisher-dll-imports.txt"
cp "$work/publisher-headers.json" "$out/publisher-headers.json"
python3 - "$llvm" "$out" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
llvm,out=map(Path,sys.argv[1:]);expected=json.loads((out/'publisher-headers.json').read_bytes());dest=llvm/'x86_64-w64-mingw32/include'
actual={name:hashlib.sha256((dest/name).read_bytes()).hexdigest() for name in expected}
assert actual==expected
(out/'postbuild-headers.json').write_text(json.dumps(actual,sort_keys=True,indent=2)+'\n')
PYCODE
cp "$work/supplier-dll.map" "$out/supplier-dll.map"
cp "$work/build.log" "$out/supplier-build.log"
cp "$work/install.log" "$out/supplier-install.log"
cp "$work/configure.log" "$out/supplier-configure.log"
cp "$work/sdk-macros.txt" "$out/sdk-macros.txt"
cp "$work/supplier-macros.txt" "$out/supplier-macros.txt"
cp "$work/sdk-machine.txt" "$out/sdk-machine.txt"
cp "$work/supplier-machine.txt" "$out/supplier-machine.txt"
python3 -I -B tools/host-pthread-authority/collect-minimum-join-dll-origin-wip.py "$source" "$work/build" "$work" "$out"
"$llvm/bin/llvm-objdump" --disassemble --reloc "$out/libwinpthread-1.dll" > "$out/dll-disassembly.txt"
python3 -I -B tools/host-pthread-authority/verify-minimum-join-linkage-wip.py "$out"
sha256sum "$cc" "$lib/libwinpthread.a" "$lib/libwinpthread.dll.a" "$out"/*
python3 -I -B tools/host-pthread-authority/minimum_join_runtime_bundle_wip.py seal "$out"
printf '%s\n' 'JOIN_RUNTIME_DIAGNOSTIC_BUILD_ONLY current_manifest_sealed=true target_wait_proved=false product_qualification=false'
