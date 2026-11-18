#!/usr/bin/env bash
# Build original valid ELF DSOs with a host or Android API29+ C compiler.
# Link-only helper DSOs must never be added to a runtime namespace search path.
set -euo pipefail
if [[ $# != 2 ]]; then
  printf 'usage: %s COMPILER OUTPUT\n' "$0" >&2
  exit 2
fi
compiler=$1
output=$2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$output"
output=$(cd "$output" && pwd)
common=(-shared -fPIC -O2 -fno-stack-protector -fno-optimize-sibling-calls
  -nostdlib -Wall -Wextra -Werror -Wl,--hash-style=both,--enable-new-dtags,-z,defs)
printf 'path\tsoname\trole\tvalue\n' > "$output/fixtures.tsv"

library() {
  local path=$1 soname=$2 role=$3 value=$4
  shift 4
  local soname_flag=()
  if [[ $soname != - ]]; then soname_flag=("-Wl,-soname,$soname"); fi
  mkdir -p "$output/$(dirname "$path")"
  "$compiler" "${common[@]}" "-DNS_ROLE=$role" "-DNS_VALUE=$value" \
    "${soname_flag[@]}" "$here/elf_namespace_fixture.c" "$@" -o "$output/$path"
  printf '%s\t%s\t%s\t%s\n' "$path" "$soname" "$role" "$value" \
    >> "$output/fixtures.tsv"
}

library link-only/libns_test_host.so libns_test_host.so 0 0
library link-only/libdl.so libdl.so 6 0
event_flags=("-L$output/link-only" -Wl,--no-as-needed -l:libns_test_host.so)
library identity/a/libns_identity.so libns_identity.so 1 101 \
  -DNS_EVENT_ID=11 "${event_flags[@]}"
library identity/b/libns_identity.so libns_identity.so 1 202 \
  -DNS_EVENT_ID=12 "${event_flags[@]}"
library identity/other/libns_other_filename.so libns_identity.so 1 303 \
  -DNS_EVENT_ID=13 "${event_flags[@]}"
library no-soname/a/libns_no_soname.so - 1 404 \
  -DNS_EVENT_ID=14 "${event_flags[@]}"
library no-soname/b/libns_no_soname.so - 1 505 \
  -DNS_EVENT_ID=15 "${event_flags[@]}"

library search/ld/libns_leaf.so libns_leaf.so 1 61 -DNS_EXPORT=ns_leaf_value
library search/root/run/libns_leaf.so libns_leaf.so 1 62 -DNS_EXPORT=ns_leaf_value
library search/default/libns_leaf.so libns_leaf.so 1 63 -DNS_EXPORT=ns_leaf_value
library search/lib-root/lib64/libns_leaf.so libns_leaf.so 1 64 -DNS_EXPORT=ns_leaf_value
library search/chain/children/libns_leaf.so libns_leaf.so 1 65 -DNS_EXPORT=ns_leaf_value
library search/root/libns_search.so libns_search.so 2 0 -DNS_EXPORT=ns_read \
  '-Wl,-rpath,$ORIGIN/run' "-L$output/search/root/run" -Wl,--no-as-needed -l:libns_leaf.so
library search/lib-root/libns_lib.so libns_lib.so 2 0 -DNS_EXPORT=ns_read \
  '-Wl,-rpath,${ORIGIN}/${LIB}' "-L$output/search/lib-root/lib64" \
  -Wl,--no-as-needed -l:libns_leaf.so
library search/chain/children/libns_middle.so libns_middle.so 2 0 -DNS_EXPORT=ns_middle \
  "-L$output/search/chain/children" -Wl,--no-as-needed -l:libns_leaf.so
library search/chain/children/libns_middle_origin.so libns_middle_origin.so 2 0 \
  -DNS_EXPORT=ns_middle '-Wl,-rpath,$ORIGIN' "-L$output/search/chain/children" \
  -Wl,--no-as-needed -l:libns_leaf.so
library search/chain/libns_chain.so libns_chain.so 2 0 \
  -DNS_EXPORT=ns_read -DNS_IMPORT=ns_middle '-Wl,-rpath,$ORIGIN/children' \
  "-L$output/search/chain/children" -Wl,--no-as-needed -l:libns_middle.so
library search/chain/libns_chain_origin.so libns_chain_origin.so 2 0 \
  -DNS_EXPORT=ns_read -DNS_IMPORT=ns_middle '-Wl,-rpath,$ORIGIN/children' \
  "-L$output/search/chain/children" -Wl,--no-as-needed -l:libns_middle_origin.so

library scope/libns_global.so libns_global.so 3 10
library scope/libns_dfglobal.so libns_dfglobal.so 3 70 -Wl,-z,global
library scope/libns_local.so libns_local.so 3 20
library scope/libns_plain.so libns_plain.so 3 30
library scope/libns_symbolic.so libns_symbolic.so 3 40 -Wl,-Bsymbolic
library scope/libns_protected.so libns_protected.so 3 50 -DNS_VISIBILITY=1
library scope/libns_hidden.so libns_hidden.so 3 60 -DNS_VISIBILITY=2
library scope/libns_import.so libns_import.so 2 0 \
  -DNS_EXPORT=ns_read -DNS_IMPORT=ns_collision '-Wl,-rpath,$ORIGIN' \
  "-L$output/scope" -Wl,--no-as-needed -l:libns_local.so
library scope/libns_deep.so libns_deep.so 3 82
library scope/libns_left.so libns_left.so 4 80 '-Wl,-rpath,$ORIGIN' \
  "-L$output/scope" -Wl,--no-as-needed -l:libns_deep.so
library scope/libns_right.so libns_right.so 3 81
library scope/libns_bfs.so libns_bfs.so 2 0 \
  -DNS_EXPORT=ns_read -DNS_IMPORT=ns_collision '-Wl,-rpath,$ORIGIN' \
  "-L$output/scope" -Wl,--no-as-needed -l:libns_left.so -l:libns_right.so

library caller/a/libns_next_a.so libns_next_a.so 3 401
library caller/b/libns_next_b.so libns_next_b.so 3 402
library caller/a/libns_caller_a.so libns_caller_a.so 5 301 '-Wl,-rpath,$ORIGIN' \
  "-L$output/caller/a" "-L$output/link-only" -Wl,--no-as-needed \
  -l:libns_next_a.so -l:libdl.so
library caller/b/libns_caller_b.so libns_caller_b.so 5 302 '-Wl,-rpath,$ORIGIN' \
  "-L$output/caller/b" "-L$output/link-only" -Wl,--no-as-needed \
  -l:libns_next_b.so -l:libdl.so

printf 'Built %s original namespace ELF fixtures in %s\n' \
  "$(($(wc -l < "$output/fixtures.tsv") - 1))" "$output"
