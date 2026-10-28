#!/usr/bin/env bash
# Build the framework's resources, framework-res.apk, and the R and Manifest
# classes the framework compiles against.
#
#   tools/build-framework-res.sh <out-dir>
#
# Mirrors frameworks/base/core/res/Android.bp: aapt2 compiles core/res/res
# and links it with the framework's manifest and assets, with framework-res's
# aaptflags and the release's values for the aconfig flags its resources and
# manifest reference (flags_packages; tools/gen/aconfig_values.py). The
# remote-views colors are their own small resource table, stored as a raw
# resource, as the remote-color-resources-* genrules do. Writes
# <out-dir>/framework-res.apk and the generated Java under
# <out-dir>/framework-res-java. aapt2 comes from Google's build-tools.
# Needs the AOSP sources from tools/fetch-aosp.sh.
set -euo pipefail

BUILD_TOOLS=36.1
RELEASE=bp4a  # android-16.0.0_r4 is BP4A.251205.006 (build/make core/build_id.mk)
root=$(cd "$(dirname "$0")/.." && pwd)
aosp=${AOSP_DIR:-$root/third_party/aosp}
out=$(mkdir -p "$1" && cd "$1" && pwd)
work=${WORK_DIR:-$(mktemp -d)}
mkdir -p "$work"
res=$aosp/frameworks/base/core/res
py=${PYTHON:-python3}

# Native tools on Windows take Windows paths.
if [[ $(uname -s) == MINGW* || $(uname -s) == MSYS* ]]; then
  native() { cygpath -w "$1"; }
  host=windows exe=.exe
else
  native() { echo "$1"; }
  host=linux exe=
fi

tools=$work/build-tools-$BUILD_TOOLS
if [[ ! -d $tools ]]; then
  curl -fsSL -o "$work/build-tools.zip" \
    "https://dl.google.com/android/repository/build-tools_r${BUILD_TOOLS}_$host.zip"
  rm -rf "$work/build-tools.tmp" && mkdir -p "$work/build-tools.tmp"
  "$py" -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "$(native "$work/build-tools.zip")" "$(native "$work/build-tools.tmp")"
  chmod +x "$work/build-tools.tmp"/*/aapt2* "$work/build-tools.tmp"/*/aidl* 2>/dev/null || true
  mv "$work/build-tools.tmp"/* "$tools" && rmdir "$work/build-tools.tmp"
fi
aapt2=$tools/aapt2$exe

flags=$("$py" "$root/tools/gen/aconfig_values.py" "$aosp/build/release" "$RELEASE" \
  --aapt2 "$res/res" "$res/AndroidManifest.xml")

# aapt2 wants each file under a res/<type>/ directory, by a relative path on
# Windows.
compile() {
  (cd "$1" && "$aapt2" compile "${@:3}" -o "$(native "$2")")
}

# --- Remote-views colors ----------------------------------------------------
colors=$work/remote-colors
rm -rf "$colors" && mkdir -p "$colors/flat" "$colors/raw/res/raw" "$colors/raw/res/values"
for f in public colors; do
  compile "$res/remote_color_resources_res" "$colors/flat" "$(native values/$f.xml)"
done
"$aapt2" link -o "$(native "$colors/remote-colors.apk")" \
  --manifest "$(native "$res/RemoteThemeColorsAndroidManifest.xml")" \
  "$(native "$colors/flat/values_public.arsc.flat")" \
  "$(native "$colors/flat/values_colors.arsc.flat")"
"$py" -c 'import sys, zipfile; open(sys.argv[2], "wb").write(zipfile.ZipFile(sys.argv[1]).read("resources.arsc"))' \
  "$(native "$colors/remote-colors.apk")" "$(native "$colors/raw/res/raw/remote_views_color_resources.arsc")"

# --- framework-res ----------------------------------------------------------
flat=$work/framework-res-flat
rm -rf "$flat" && mkdir -p "$flat"
compile "$res" "$flat/res.zip" --dir res --feature-flags "$flags"
# With its java-symbol declaration, as remote-color-resources-arsc-zip has it.
cp "$res/remote_color_resources_res/symbols.xml" "$colors/raw/res/values/"
compile "$colors/raw" "$flat" "$(native res/raw/remote_views_color_resources.arsc)"
compile "$colors/raw" "$flat" "$(native res/values/symbols.xml)"

# Optional CI resource overlay. The normal resource table remains the default;
# a caller using this must independently compare every generated R/Manifest byte.
overlay=()
if [[ -n ${FRAMEWORK_RES_OVERLAY_DIR:-} ]]; then
  [[ -d $FRAMEWORK_RES_OVERLAY_DIR ]] || { echo 'Missing framework resource overlay directory' >&2; exit 2; }
  compile "$FRAMEWORK_RES_OVERLAY_DIR" "$flat/fixture-overlay.zip" --dir . --feature-flags "$flags"
  overlay=(-R "$(native "$flat/fixture-overlay.zip")")
fi

java=$out/framework-res-java
rm -rf "$java" && mkdir -p "$java"
"$aapt2" link -o "$(native "$out/framework-res.apk")" \
  --manifest "$(native "$res/AndroidManifest.xml")" -A "$(native "$res/assets")" \
  --private-symbols com.android.internal --no-auto-version --auto-add-overlay \
  --enable-sparse-encoding --min-sdk-version 36 --target-sdk-version 36 \
  --feature-flags "$flags" --java "$(native "$java")" \
  "$(native "$flat/res.zip")" "$(native "$flat/raw_remote_views_color_resources.arsc.flat")" \
  "$(native "$flat/values_symbols.arsc.flat")" "${overlay[@]}"
ls -la "$out"
find "$java" -name '*.java'
