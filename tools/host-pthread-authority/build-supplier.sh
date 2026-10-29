#!/usr/bin/env bash
# CI only. The caller supplies a private extracted compiler, never a shared cache.
set -euo pipefail
test "${GITHUB_ACTIONS:-}" = true
llvm=$(realpath "${1:?private compiler directory}")
role=${2:-production}
case "$role" in
  production) suffix= ;;
  fault) suffix=-fault ;;
  probe) suffix=-probe ;;
  *) exit 2 ;;
esac
root=$(git rev-parse --show-toplevel)
test "$llvm" != "$HOME/toolchain/${LLVM_MINGW_NAME:-}"
supplier="$PWD/host-authority-supplier$suffix"
test ! -e "$supplier"
mkdir "$supplier"
git -C "$supplier" init -q
git -C "$supplier" remote add origin https://github.com/mingw-w64/mingw-w64.git
git -C "$supplier" fetch --depth=1 origin a3d93999ef0521681d45e445c39964a7af0f593f
git -C "$supplier" checkout --detach FETCH_HEAD
python3 "$root/tools/host-pthread-authority/receipt.py" inputs "$supplier"
git -C "$supplier" apply --check "$root/tools/host-pthread-authority/supplier.patch"
git -C "$supplier" apply "$root/tools/host-pthread-authority/supplier.patch"
if test "$role" = fault; then
  git -C "$supplier" apply --check "$root/tools/host-pthread-authority/fault-supplier.patch"
  git -C "$supplier" apply "$root/tools/host-pthread-authority/fault-supplier.patch"
fi
export PATH="$llvm/bin:$PATH"
supplier_cflags='-g -O2 -mguard=cf -Werror -Xclang -fdump-record-layouts'
if test "$role" = probe; then supplier_cflags="$supplier_cflags -DGESHER_PTHREAD_ROUTING_PROBE=1"; fi
mkdir "$supplier/build"
cd "$supplier/build"
../mingw-w64-libraries/winpthreads/configure \
  --host=x86_64-w64-mingw32 --prefix="$llvm/x86_64-w64-mingw32" \
  --libdir="$llvm/x86_64-w64-mingw32/lib" --enable-shared --enable-static \
  CFLAGS="$supplier_cflags" CXXFLAGS='-g -O2 -mguard=cf -Werror' \
  CC="$llvm/bin/x86_64-w64-mingw32-clang" \
  AR="$llvm/bin/llvm-ar" RANLIB="$llvm/bin/llvm-ranlib" \
  RC="$llvm/bin/x86_64-w64-mingw32-windres" 2>&1 | tee configure-command.log
# Upstream's GCC-only -Wc,-no-pthread becomes an unused Clang argument at
# link time. Override that one make variable; keep every warning fatal.
# Clang adds winpthread only with -pthread, which this build never supplies.
supplier_ldflags='-no-undefined -version-info 1:0:0 -L$(builddir)/fakelib'
make -j2 V=1 "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee build-commands.log
make install "libwinpthread_la_LDFLAGS=$supplier_ldflags" 2>&1 | tee install-commands.log
mkdir -p "$llvm/x86_64-w64-mingw32/share/mingw32"
cp ../mingw-w64-libraries/winpthreads/COPYING "$llvm/x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt"
cd "$root"
python3 tools/host-pthread-authority/receipt.py supplier "$supplier" "$llvm" "$role"
