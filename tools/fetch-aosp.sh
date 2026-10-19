#!/usr/bin/env bash
# Fetch the AOSP projects listed in aosp.lock, shallow, at the pinned tag.
#
#   tools/fetch-aosp.sh [dest]     (default: $AOSP_DIR or third_party/aosp)
#
# Patches in patches/<project>/*.patch are applied in order after checkout.
# Re-running is cheap: projects already at the pinned tag with the same
# patches are skipped; a patch change resets the checkout and re-applies.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-${AOSP_DIR:-$root/third_party/aosp}}
lock=$root/aosp.lock

tag=$(awk '$1 == "tag" { print $2 }' "$lock")
mkdir -p "$dest"

clone() {  # clone <project> <dir> [sparse dirs...]
  local url=https://android.googlesource.com/platform/$1 dir=$2
  shift 2
  # Keep LF endings everywhere: AOSP generators parse sources with regexes.
  local git=(git -c advice.detachedHead=false -c core.autocrlf=false)
  if (($#)); then
    "${git[@]}" clone -q --depth 1 --filter=blob:none --sparse --branch "$tag" "$url" "$dir"
    git -C "$dir" sparse-checkout set "$@"
  else
    "${git[@]}" clone -q --depth 1 --branch "$tag" "$url" "$dir"
  fi
}

grep -vE '^\s*(#|$|tag )' "$lock" | while read -r project sparse; do
  dir=$dest/$project
  stamp=$dir/.gesher-tag
  patches=("$root/patches/$project"/*.patch)
  [[ -e ${patches[0]} ]] || patches=()
  want="$tag $(cat "${patches[@]}" /dev/null | git hash-object --stdin)"
  have=$(cat "$stamp" 2>/dev/null || true)

  if [[ $have == "$want" ]]; then
    continue
  elif [[ ${have%% *} == "$tag" ]]; then
    echo "re-patching $project"
    git -C "$dir" reset -q --hard
    git -C "$dir" clean -qfdx -e .gesher-tag
  else
    echo "fetching $project @ $tag"
    rm -rf "$dir"
    # shellcheck disable=SC2086
    clone "$project" "$dir" $sparse
  fi

  for patch in "${patches[@]}"; do
    echo "  applying ${patch#"$root"/}"
    git -C "$dir" apply --whitespace=nowarn "$patch"
  done
  echo "$want" > "$stamp"
done

echo "AOSP sources ready in $dest"
