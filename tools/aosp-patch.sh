#!/usr/bin/env bash
# Create a new patch for an AOSP project.
#
#   tools/aosp-patch.sh begin <project>          # e.g. art, system/libbase
#   ... edit files in $AOSP_DIR/<project> ...
#   tools/aosp-patch.sh end <project> <slug>     # writes patches/<project>/NNNN-<slug>.patch
#
# `begin` records the current state (pinned tag + existing Gesher patches) as a
# temporary commit; `end` saves the diff against it as the next numbered patch
# and drops the temporary commit, leaving the working tree as edited.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
aosp=${AOSP_DIR:-$root/third_party/aosp}
cmd=${1:?begin|end}
project=${2:?project}
dir=$aosp/$project
marker="gesher: patch base (temporary)"

case $cmd in
  begin)
    if [[ $(git -C "$dir" log -1 --format=%s) == "$marker" ]]; then
      echo "already begun" >&2
      exit 1
    fi
    git -C "$dir" add -A -- . ':!.gesher-tag'
    git -C "$dir" -c user.name=gesher -c user.email=gesher@invalid \
      commit -q --allow-empty -m "$marker"
    echo "edit $dir, then: $0 end $project <slug>"
    ;;
  end)
    slug=${3:?slug}
    [[ $(git -C "$dir" log -1 --format=%s) == "$marker" ]] || { echo "not begun" >&2; exit 1; }
    mkdir -p "$root/patches/$project"
    last=$(ls "$root/patches/$project" | grep -oE '^[0-9]{4}' | sort | tail -1 || true)
    next=$(printf '%04d' $((10#${last:-0} + 1)))
    out=$root/patches/$project/$next-$slug.patch
    git -C "$dir" add -A -- . ':!.gesher-tag'
    git -C "$dir" diff --cached HEAD > "$out"
    git -C "$dir" reset -q --soft HEAD~1
    git -C "$dir" reset -q
    echo "wrote ${out#"$root"/} ($(grep -c '^+++ ' "$out") files)"
    ;;
  *)
    echo "usage: $0 begin|end <project> [slug]" >&2
    exit 2
    ;;
esac
