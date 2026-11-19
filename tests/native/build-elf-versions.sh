#!/usr/bin/env bash
# Build ordinary original versioned ELF DSOs with a host or API29+ compiler.
set -euo pipefail
compiler=$1
output=$2
here=$(cd "$(dirname "$0")" && pwd)
for directory in lookup-gnu lookup-sysv required/good required/bad weak/good weak/bad unversioned global virtual tls tls-weak/good tls-weak/bad; do
  mkdir -p "$output/$directory"
done
common=(-shared -fPIC -O2 -fno-stack-protector -nostdlib -Wall -Wextra -Werror)
provider() {
  local directory=$1 role=$2 hash=$3 script=$4
  local flags=()
  if [[ -n "$script" ]]; then flags+=("-Wl,--version-script=$here/elf_versions/$script.map"); fi
  "$compiler" "${common[@]}" -DGV_ROLE="$role" "-Wl,--hash-style=$hash" \
    -Wl,-soname,libgv_provider.so "${flags[@]}" \
    "$here/elf_versions_fixture.c" -o "$output/$directory/libgv_provider.so"
}
provider lookup-gnu 1 gnu full
provider lookup-sysv 1 sysv full
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
  "$here/elf_guest_dlvsym_fixture.c" -o "$output/libgv_guest_dlvsym.so"
"$compiler" "${common[@]}" -Wl,--hash-style=both -Wl,-soname,libgv_null_import.so \
  "$here/elf_null_reloc_fixture.c" -o "$output/libgv_null_import.so"
