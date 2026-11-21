#!/usr/bin/env bash
# Build original ELF fixtures using a host or API29+ NDK C compiler. Output
# preserves a complete graph and a graph missing only its transitive leaf.
set -euo pipefail
compiler=$1
output=$2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$output/complete" "$output/incomplete"
complete="$output/complete"
common=(-shared -fPIC -O2 -fno-stack-protector -nostdlib -Wall -Wextra -Werror
        -Wl,--hash-style=both '-Wl,-rpath,$ORIGIN')
build() {
  local name=$1 role=$2
  shift 2
  "$compiler" "${common[@]}" -DTX_ROLE="$role" \
    "-Wl,-soname,tx_${name}.so" -o "$complete/tx_${name}.so" \
    "$here/elf_load_transaction_graph.c" -L"$complete" -Wl,--no-as-needed "$@"
}
build newchild 1
build late 2
build middle 3 -l:tx_late.so
build root 4 -l:tx_newchild.so -l:tx_middle.so
build reloc_child 5
build reloc_root 6 -l:tx_reloc_child.so
build shared 7
build left 8 -l:tx_shared.so
build right 9 -l:tx_shared.so
build diamond 10 -l:tx_left.so -l:tx_right.so
# First create A as an import library, then close the actual A <-> B graph.
build cycle_a 11
build cycle_b 12 -l:tx_cycle_a.so
build cycle_a 11 -l:tx_cycle_b.so
build reentrant_child 13
build reentrant_root 14
build concurrent_child 15
build concurrent_root 16 -l:tx_concurrent_child.so
build protect_child 17
build protect_root 18 -l:tx_protect_child.so
build concurrent_fail_child 19
build concurrent_fail_root 20 -l:tx_concurrent_fail_child.so
build ifunc_file 21
build ifunc_memory 22
# Resolver TLS accesses must not depend on a still-unresolved external PLT.
build ifunc_tls_child 23 -fno-plt -ftls-model=global-dynamic
build ifunc_tls_root 24 -fno-plt -ftls-model=global-dynamic -l:tx_ifunc_tls_child.so
for name in newchild middle root; do
  cp "$complete/tx_${name}.so" "$output/incomplete/tx_${name}.so"
done
# A reused build directory must not accidentally repair the intended failure.
test ! -e "$output/incomplete/tx_late.so"
