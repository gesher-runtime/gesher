#!/usr/bin/env bash
# Collect Gesher's system fonts (docs/fonts.md) from the AOSP checkout:
#
#   tools/fonts/collect-fonts.sh <aosp-dir> <out-dir>
#
# Writes <out-dir>/fonts/: the font files and fonts.xml (config/fonts/fonts.xml),
# which the runtime expects as $ANDROID_ROOT/fonts/*.ttf and
# $ANDROID_ROOT/etc/fonts.xml; and <out-dir>/FONTS-NOTICES.txt, the fonts'
# licenses. The versions are android-16.0.0_r4's (BP4A release flags:
# RELEASE_PACKAGE_ROBOTO_FONT_VERSION 3.011, RELEASE_PACKAGE_HEBREW_FONT_VERSION
# the default, 1.040). Fails if a font fonts.xml names is missing.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
aosp=$1
out=$2
mkdir -p "$out/fonts"

roboto=$aosp/external/roboto-fonts
hebrew=$aosp/external/noto-fonts/notosanshebrew
serif=$aosp/external/noto-fonts/notoserif
data=$aosp/frameworks/base/data/fonts

cp "$roboto/font/3.011/Roboto-Regular.ttf" "$out/fonts/"
cp "$hebrew/font/1.040/NotoSansHebrew-Regular.ttf" "$hebrew/font/1.040/NotoSansHebrew-Bold.ttf" "$out/fonts/"
cp "$serif"/NotoSerif-{Regular,Bold,Italic,BoldItalic}.ttf "$out/fonts/"
cp "$data/DroidSansMono.ttf" "$out/fonts/"
cp "$root/config/fonts/fonts.xml" "$out/fonts/fonts.xml"

# Every file fonts.xml names is there, and nothing else is.
missing=0
for f in $(grep -oE '[A-Za-z0-9_-]+\.(ttf|otf|ttc)' "$out/fonts/fonts.xml" | sort -u); do
  if [[ ! -f $out/fonts/$f ]]; then
    echo "fonts.xml names $f, which is not collected" >&2
    missing=1
  fi
done
for f in "$out"/fonts/*.ttf; do
  grep -q "$(basename "$f")" "$out/fonts/fonts.xml" || { echo "$(basename "$f") is not in fonts.xml" >&2; missing=1; }
done
((missing == 0)) || exit 1

{
  echo "Gesher's system fonts and their licenses (docs/fonts.md)."
  echo "Fonts from AOSP android-16.0.0_r4; fonts.xml is derived from"
  echo "frameworks/base/data/fonts/fonts.xml (Apache License 2.0)."
  for part in \
      "Roboto-Regular.ttf (Roboto 3.011, external/roboto-fonts): Apache License 2.0|$roboto/NOTICE" \
      "NotoSansHebrew-Regular.ttf, NotoSansHebrew-Bold.ttf (1.040, external/noto-fonts): SIL Open Font License 1.1|$hebrew/NOTICE" \
      "NotoSerif-Regular/Bold/Italic/BoldItalic.ttf (external/noto-fonts): SIL Open Font License 1.1|$serif/NOTICE" \
      "DroidSansMono.ttf and fonts.xml (frameworks/base/data/fonts): Apache License 2.0|$data/NOTICE"; do
    echo
    echo "=============================================================================="
    echo "${part%%|*}"
    echo "=============================================================================="
    echo
    cat "${part#*|}"
  done
} > "$out/FONTS-NOTICES.txt"

ls -la "$out/fonts"
du -sh "$out/fonts"
