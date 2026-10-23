#!/usr/bin/env bash
# Fetch the AOSP projects listed in aosp.lock, shallow, at the pinned tag.
#
#   tools/fetch-aosp.sh [dest]     (default: $AOSP_DIR or third_party/aosp)
#
# Patches in patches/<project>/*.patch are applied in order after checkout.
# Re-running is cheap: projects already at the pinned tag with the same
# patches are skipped; a patch change resets the checkout and re-applies.
# Projects are fetched $FETCH_JOBS at a time (default 8).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-${AOSP_DIR:-$root/third_party/aosp}}
lock=$root/aosp.lock
jobs=${FETCH_JOBS:-8}

tag=$(awk '$1 == "tag" { print $2 }' "$lock")
mkdir -p "$dest"

clone() {  # clone <repository> <dir> [sparse dirs...]
  local url=https://android.googlesource.com/$1 dir=$2
  shift 2
  # Keep LF endings everywhere: AOSP generators parse sources with regexes.
  local git=(git -c advice.detachedHead=false -c core.autocrlf=false)
  if (($#)); then
    # Non-cone patterns, so single files can be listed too. protectNTFS is off
    # because some AOSP projects (libcore) have paths Windows cannot hold;
    # those paths are outside the sparse set and never written to disk.
    "${git[@]}" -c core.protectNTFS=false clone -q --depth 1 --filter=blob:none --no-checkout       --branch "$tag" "$url" "$dir"
    git -C "$dir" config core.protectNTFS false
    git -C "$dir" sparse-checkout set --no-cone "$@"
    git -C "$dir" read-tree -mu HEAD
  else
    "${git[@]}" clone -q --depth 1 --branch "$tag" "$url" "$dir"
  fi
}

sync_project() {  # sync_project <entry> [sparse dirs...]
  set -euo pipefail
  # <path>[=<repository>]; the repository defaults to platform/<path>.
  local entry=$1
  shift
  local sparse="$*"
  local project=${entry%%=*}
  local repo=platform/$project
  [[ $entry == *=* ]] && repo=${entry#*=}
  local dir=$dest/$project
  local stamp=$dir/.gesher-tag
  local patches=("$root/patches/$project"/*.patch)
  [[ -e ${patches[0]} ]] || patches=()
  local want have
  want="$tag $({ echo "$sparse"; cat "${patches[@]}" /dev/null; } | git hash-object --stdin)"
  have=$(cat "$stamp" 2>/dev/null || true)

  if [[ $have == "$want" ]]; then
    return 0
  elif [[ ${have%% *} == "$tag" && -z $sparse ]]; then
    echo "re-patching $project"
    git -C "$dir" reset -q --hard
    git -C "$dir" clean -qfdx -e .gesher-tag
  else
    echo "fetching $project @ $tag"
    rm -rf "$dir"
    # shellcheck disable=SC2086
    clone "$repo" "$dir" $sparse
  fi

  local patch
  for patch in "${patches[@]}"; do
    echo "  applying ${patch#"$root"/}"
    git -C "$dir" apply --whitespace=nowarn "$patch"
  done
  echo "$want" > "$stamp"
}

export -f clone sync_project
export root dest tag

grep -vE '^\s*(#|$|tag )' "$lock" | xargs -P "$jobs" -d '\n' -I{} bash -c 'sync_project {}'

echo "AOSP sources ready in $dest"
