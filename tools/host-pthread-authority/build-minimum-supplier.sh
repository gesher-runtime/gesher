#!/usr/bin/env bash
# CI only; two reviewed minimum patches on pristine source, never private SDK code.
set -euo pipefail
test "${GITHUB_ACTIONS:-}" = true
test "${GITHUB_JOB:-}" = minimum_compile
llvm=$(realpath "${1:?fresh original extracted compiler}")
root=$(git rev-parse --show-toplevel)
supplier="$PWD/host-minimum-candidate-source"
test ! -e "$supplier"
mkdir "$supplier"
git -C "$supplier" init -q
git -C "$supplier" remote add origin https://github.com/mingw-w64/mingw-w64.git
git -C "$supplier" fetch --depth=1 origin a3d93999ef0521681d45e445c39964a7af0f593f
git -C "$supplier" checkout --detach FETCH_HEAD
python3 tools/host-pthread-authority/minimum_receipt.py inputs "$supplier"
python3 tools/host-pthread-authority/minimum_receipt.py original "$llvm" "$supplier" "$supplier/original.json"
for patch in minimum-detached-retirement.patch minimum-noattach-join.patch; do
  git -C "$supplier" apply --check "$root/tools/host-pthread-authority/$patch"
  git -C "$supplier" apply "$root/tools/host-pthread-authority/$patch"
done
python3 tools/host-pthread-authority/minimum_receipt.py patched "$supplier"
export PATH="$llvm/bin:$PATH"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir "$supplier/build"
cd "$supplier/build"
# No private role flag or WINVER override. Capture actual supplied SDK profile.
printf '#define __LARGE_MBSTATE_T\n#ifdef HAVE_CONFIG_H\n#include "config.h"\n#endif\n#include <malloc.h>\n#include <signal.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <wchar.h>\n#define WIN32_LEAN_AND_MEAN\n#include <windows.h>\nDWORD (WINAPI *minimum_getthreadid_declaration)(HANDLE)=GetThreadId;\n' > sdk-profile.c
"$llvm/bin/x86_64-w64-mingw32-clang" -dM -E sdk-profile.c > sdk-profile-macros.txt
cat sdk-profile-macros.txt
"$llvm/bin/x86_64-w64-mingw32-clang" -Wall -Wextra -Werror -c sdk-profile.c -o sdk-profile.obj
"$llvm/bin/llvm-readobj" --file-headers sdk-profile.obj > sdk-profile-machine.txt
cat sdk-profile-machine.txt
supplier_cflags='-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts'
../mingw-w64-libraries/winpthreads/configure \
  --host=x86_64-w64-mingw32 --prefix="$llvm/x86_64-w64-mingw32" \
  --libdir="$llvm/x86_64-w64-mingw32/lib" --enable-shared --enable-static \
  CFLAGS="$supplier_cflags" CXXFLAGS='-g -O2 -mguard=cf -Werror' \
  CC="$llvm/bin/x86_64-w64-mingw32-clang" AR="$llvm/bin/llvm-ar" \
  RANLIB="$llvm/bin/llvm-ranlib" RC="$llvm/bin/x86_64-w64-mingw32-windres" \
  2>&1 | tee configure-command.log
# Repeat against the actual configured supplier include/macro profile.
"$llvm/bin/x86_64-w64-mingw32-clang" -DHAVE_CONFIG_H -I. -I../mingw-w64-libraries/winpthreads \
  -I../mingw-w64-libraries/winpthreads/include -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
  -Wall -Wextra -Werror -dM -E sdk-profile.c > supplier-profile-macros.txt
cat supplier-profile-macros.txt
"$llvm/bin/x86_64-w64-mingw32-clang" -DHAVE_CONFIG_H -I. -I../mingw-w64-libraries/winpthreads \
  -I../mingw-w64-libraries/winpthreads/include -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
  -Wall $supplier_cflags -c sdk-profile.c -o supplier-profile.obj
"$llvm/bin/llvm-readobj" --file-headers supplier-profile.obj > supplier-profile-machine.txt
pwd > actual-build-directory.txt
(cd "$root" && python3 tools/host-pthread-authority/minimum_receipt.py profile "$supplier/build")
# Same documented Clang/libtool no-pthread override as earlier builds; no -pthread.
supplier_ldflags="-no-undefined -version-info 1:0:0 -L\$(builddir)/fakelib -Wl,-Map,$supplier/build/supplier-dll.map -Wl,--verbose"
if ! make -j2 V=1 "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee build-commands.log; then
  "$llvm/bin/x86_64-w64-mingw32-clang" -DHAVE_CONFIG_H -I. -I../mingw-w64-libraries/winpthreads \
    -I../mingw-w64-libraries/winpthreads/include -DIN_WINPTHREAD -D__USE_MINGW_ANSI_STDIO=0 \
    -Wall $supplier_cflags -c ../mingw-w64-libraries/winpthreads/src/thread.c -o minimum-static-diagnostic.obj 2>&1 || :
  exit 1
fi
make install "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee install-commands.log
mkdir -p "$llvm/x86_64-w64-mingw32/share/mingw32"
cp ../mingw-w64-libraries/winpthreads/COPYING "$llvm/x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt"
cd "$root"
python3 tools/host-pthread-authority/minimum_receipt.py supplier "$supplier" "$llvm" "$supplier/original.json"
