#!/usr/bin/env bash
# Build ordinary original versioned ELF DSOs with a host or API29+ compiler.
set -euo pipefail
compiler=$1
output=$2
here=$(cd "$(dirname "$0")" && pwd)
for directory in lookup-gnu lookup-sysv null-global required/good required/bad weak/good weak/bad unversioned global virtual tls tls-weak/good tls-weak/bad; do
  mkdir -p "$output/$directory"
done
common=(-shared -fPIC -O2 -fno-stack-protector -nostdlib -Wall -Wextra -Werror)
mkdir -p "$output/link-only"
for entry in 1:libgv_test_host.so 2:libdl.so 3:libc.so 4:libm.so; do
  role=${entry%%:*}; name=${entry#*:}
  "$compiler" "${common[@]}" -DGV_LINK_ROLE="$role" "-Wl,-soname,$name" \
    "$here/elf_versions_link_only.c" -o "$output/link-only/$name"
done
# Every callback-bearing DSO explicitly declares its isolated host provider.
common+=("-L$output/link-only" -Wl,--no-as-needed -l:libgv_test_host.so)
provider() {
  local directory=$1 role=$2 hash=$3 script=$4
  local flags=("${@:5}")
  if [[ -n "$script" ]]; then flags+=("-Wl,--version-script=$here/elf_versions/$script.map"); fi
  "$compiler" "${common[@]}" -DGV_ROLE="$role" "-Wl,--hash-style=$hash" \
    -Wl,-soname,libgv_provider.so "${flags[@]}" \
    "$here/elf_versions_fixture.c" -o "$output/$directory/libgv_provider.so"
}
provider lookup-gnu 1 gnu full
provider lookup-sysv 1 sysv full
# Bionic's relocation global group requires DF_1_GLOBAL. RTLD_GLOBAL alone
# provides DEFAULT visibility but does not make an unrelated relocation group.
provider null-global 1 gnu full -Wl,-z,global
provider required/bad 2 gnu old
provider unversioned 5 gnu ''
provider global 6 gnu global
cp "$output/lookup-gnu/libgv_provider.so" "$output/required/good/libgv_provider.so"
cp "$output/lookup-gnu/libgv_provider.so" "$output/weak/good/libgv_provider.so"
cp "$output/required/bad/libgv_provider.so" "$output/weak/bad/libgv_provider.so"
for role in 3 4; do
  if [[ "$role" == 3 ]]; then directory=required/good; else directory=weak/good; fi
  "$compiler" "${common[@]}" -DGV_ROLE="$role" -Wl,--hash-style=gnu \
    -Wl,-soname,libgv_client.so '-Wl,-rpath,$ORIGIN' \
    "$here/elf_versions_fixture.c" -L"$output/$directory" -Wl,--no-as-needed \
    -l:libgv_provider.so -o "$output/$directory/libgv_client.so"
done
for directory in required/bad unversioned global virtual; do
  cp "$output/required/good/libgv_client.so" "$output/$directory/libgv_client.so"
done
cp "$output/weak/good/libgv_client.so" "$output/weak/bad/libgv_client.so"
"$compiler" "${common[@]}" -DGV_ROLE=7 -ftls-model=global-dynamic \
  -Wl,--hash-style=both -Wl,-soname,libgv_tls_provider.so \
  "-Wl,--version-script=$here/elf_versions/tls.map" \
  "$here/elf_versions_fixture.c" -o "$output/tls/libgv_tls_provider.so"
"$compiler" "${common[@]}" -DGV_ROLE=8 -ftls-model=global-dynamic \
  -Wl,--hash-style=both -Wl,-soname,libgv_tls_client.so '-Wl,-rpath,$ORIGIN' \
  "$here/elf_versions_fixture.c" -L"$output/tls" -Wl,--no-as-needed \
  -l:libgv_tls_provider.so -o "$output/tls/libgv_tls_client.so"
cp "$output/tls/libgv_tls_provider.so" "$output/tls-weak/good/libgv_tls_provider.so"
"$compiler" "${common[@]}" -DGV_ROLE=9 -ftls-model=global-dynamic \
  -Wl,--hash-style=both -Wl,-soname,libgv_tls_provider.so \
  "-Wl,--version-script=$here/elf_versions/tls-old.map" \
  "$here/elf_versions_fixture.c" -o "$output/tls-weak/bad/libgv_tls_provider.so"
for role in 10 11; do
  if [[ "$role" == 10 ]]; then name=libgv_tls_weak_client.so; else name=libgv_tls_ie_client.so; fi
  "$compiler" "${common[@]}" -DGV_ROLE="$role" -ftls-model=global-dynamic \
    -Wl,--hash-style=both "-Wl,-soname,$name" '-Wl,-rpath,$ORIGIN' \
    "$here/elf_versions_fixture.c" -L"$output/tls-weak/good" -Wl,--no-as-needed \
    -l:libgv_tls_provider.so -o "$output/tls-weak/good/$name"
  cp "$output/tls-weak/good/$name" "$output/tls-weak/bad/$name"
done
# Reusing an output must not silently add a real provider to the virtual case.
test ! -e "$output/virtual/libgv_provider.so"
"$compiler" "${common[@]}" -Wl,--hash-style=both -Wl,-soname,libgv_guest_dlvsym.so \
  "-Wl,--version-script=$here/elf_guest_dlvsym.map" \
  "$here/elf_guest_dlvsym_fixture.c" -l:libdl.so -l:libc.so -l:libm.so \
  -o "$output/libgv_guest_dlvsym.so"

# Defined TLS references must select one provider for both module and offset.
mkdir -p "$output/tls-scope"
scope() {
  local name=$1
  shift
  "$compiler" "${common[@]}" -O0 -ftls-model=global-dynamic -Wl,--hash-style=both \
    "-Wl,-soname,$name" "$@" "$here/elf_versions_tls_scope.c" -o "$output/tls-scope/$name"
}
scope libgv_scope_global.so -DGV_SCOPE_PROVIDER -Wl,-z,global
scope libgv_scope_local.so -DGV_SCOPE_PROVIDER
scope libgv_scope_requester.so
scope libgv_scope_protected.so -DGV_SCOPE_PROTECTED
scope libgv_scope_hidden.so -DGV_SCOPE_HIDDEN
scope libgv_scope_static.so -DGV_SCOPE_STATIC
scope libgv_scope_symbolic.so -Wl,-Bsymbolic
scope libgv_scope_global_versioned.so -DGV_SCOPE_PROVIDER -Wl,-z,global \
  "-Wl,--version-script=$here/elf_versions/tls-scope.map"
scope libgv_scope_requester_versioned.so "-Wl,--version-script=$here/elf_versions/tls-scope.map"
scope libgv_scope_requester_ie.so -DGV_SCOPE_IE_ONLY
scope libgv_scope_protected_ie.so -DGV_SCOPE_IE_ONLY -DGV_SCOPE_PROTECTED
scope libgv_scope_hidden_ie.so -DGV_SCOPE_IE_ONLY -DGV_SCOPE_HIDDEN
scope libgv_scope_static_ie.so -DGV_SCOPE_IE_ONLY -DGV_SCOPE_STATIC
scope libgv_scope_symbolic_ie.so -DGV_SCOPE_IE_ONLY -Wl,-Bsymbolic
scope libgv_scope_requester_versioned_ie.so -DGV_SCOPE_IE_ONLY \
  "-Wl,--version-script=$here/elf_versions/tls-scope.map"

"$compiler" "${common[@]}" -Wl,--hash-style=both -Wl,-soname,libgv_null_import.so \
  "$here/elf_null_reloc_fixture.c" -o "$output/libgv_null_import.so"
