#!/usr/bin/env bash
# Add every project's Soong files (*.bp) and textprotos to its sparse
# checkout, so tools/framework's planner sees each project's module graph
# before it checks out the files the plan needs.
#
#   tools/sparse-bp.sh [aosp dir]     (default: $AOSP_DIR or third_party/aosp)
#   tools/sparse-bp.sh --hash [aosp dir]
#
# --hash prints instead a short hash of every project's fetch stamp and
# sparse set: what a cache of the tree is keyed on.
#
# Patterns are only added, never replaced (other builds' files stay); full
# clones are left alone. Run after tools/fetch-aosp.sh.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
hash=
if [[ ${1:-} == --hash ]]; then
  hash=1
  shift
fi
aosp=${1:-${AOSP_DIR:-$root/third_party/aosp}}

projects() {
  find "$aosp" -mindepth 2 -maxdepth 5 -name .git -type d -prune -printf '%h
' | sort
}

if [[ -n $hash ]]; then
  projects | while read -r dir; do
    echo "${dir#"$aosp"/} $(cat "$dir/.gesher-tag" 2>/dev/null || true)"
    if [[ $(git -C "$dir" config --bool core.sparseCheckout || true) == true ]]; then
      git -C "$dir" sparse-checkout list
    fi
  done | sha256sum | cut -c1-16
  exit 0
fi

widen() {
  local dir=$1
  [[ $(git -C "$dir" config --bool core.sparseCheckout || true) == true ]] || return 0
  local have
  have=$(git -C "$dir" sparse-checkout list)
  local add=()
  for p in '*.bp' '*.textproto'; do
    grep -qxF "$p" <<<"$have" || add+=("$p")
  done
  ((${#add[@]})) || return 0
  { printf '%s\n' "$have"; printf '%s\n' "${add[@]}"; } | grep -v '^$' \
    | git -C "$dir" sparse-checkout set --no-cone --stdin
  git -C "$dir" -c core.autocrlf=false checkout -q HEAD
  echo "widened ${dir#"$aosp"/}"
}
export -f widen
export aosp

projects | xargs -P "${FETCH_JOBS:-8}" -I{} bash -c 'widen {}'
echo "*.bp files: $(find "$aosp" -name '*.bp' -not -path '*/.git/*' | wc -l)"
