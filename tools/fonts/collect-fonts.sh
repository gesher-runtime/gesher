#!/usr/bin/env bash
# Collect Gesher's system fonts (docs/fonts.md) from the AOSP checkout:
#
#   tools/fonts/collect-fonts.sh <aosp-dir> <out-dir>
#
# Writes <out-dir>/fonts/: the font files and fonts.xml (config/fonts/fonts.xml),
# which the runtime expects as $ANDROID_ROOT/fonts/*.ttf and
# $ANDROID_ROOT/etc/fonts.xml; windows-fonts.xml (config/fonts), the fallback
# families of the fonts Windows has, used in place from %WINDIR%\Fonts
# ($ANDROID_ROOT/etc/windows-fonts.xml); and <out-dir>/FONTS-NOTICES.txt, the
# fonts' licenses. The versions are android-16.0.0_r4's, as Soong selects them
# by the BP4A release flags (RELEASE_PACKAGE_ROBOTO_FONT_VERSION 3.011,
# RELEASE_PACKAGE_NOTO_SANS_DEVANAGARI_VERSION 2.006; the defaults of
# RELEASE_PACKAGE_HEBREW_FONT_VERSION, 1.040, and
# RELEASE_PACKAGE_NOTO_NASKH_ARABIC_VERSION, 1.070), installed under the names
# fonts.xml gives them. Fails if a font fonts.xml names is missing.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
aosp=$1
out=$2
mkdir -p "$out/fonts"

roboto=$aosp/external/roboto-fonts
hebrew=$aosp/external/noto-fonts/notosanshebrew
serif=$aosp/external/noto-fonts/notoserif
data=$aosp/frameworks/base/data/fonts
noto=$aosp/external/noto-fonts

cp "$roboto/font/3.011/Roboto-Regular.ttf" "$out/fonts/"
cp "$hebrew/font/1.040/NotoSansHebrew-Regular.ttf" "$hebrew/font/1.040/NotoSansHebrew-Bold.ttf" "$out/fonts/"
cp "$serif"/NotoSerif-{Regular,Bold,Italic,BoldItalic}.ttf "$out/fonts/"
cp "$data/DroidSansMono.ttf" "$out/fonts/"
# Arabic, Thai, Devanagari (docs/fonts.md): the sans-serif fallbacks, elegant and
# compact (UI) variants.
cp "$noto/notonaskharabic/font/1.070/NotoNaskhArabic.ttf" "$out/fonts/NotoNaskhArabic-Regular.ttf"
cp "$noto/notonaskharabic/font/1.070/NotoNaskhArabic-Bold.ttf" "$out/fonts/"
cp "$noto"/notonaskharabicui/NotoNaskhArabicUI-{Regular,Bold}.ttf "$out/fonts/"
cp "$noto"/notosansthai/NotoSansThai-{Regular,Bold}.ttf "$out/fonts/"
cp "$noto"/notosansthaiui/NotoSansThaiUI-{Regular,Bold}.ttf "$out/fonts/"
cp "$noto/notosansdevanagari/font/2.006/NotoSansDevanagari-Regular.ttf" "$out/fonts/NotoSansDevanagari-VF.ttf"
cp "$noto/notosansdevanagariui/NotoSansDevanagariUI-VF.ttf" "$out/fonts/"
cp "$root/config/fonts/fonts.xml" "$out/fonts/fonts.xml"
cp "$root/config/fonts/windows-fonts.xml" "$out/fonts/windows-fonts.xml"

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
      "NotoNaskhArabic-Regular/Bold.ttf (1.070), NotoNaskhArabicUI-Regular/Bold.ttf (external/noto-fonts): SIL Open Font License 1.1|$noto/notonaskharabic/NOTICE" \
      "NotoSansThai-Regular/Bold.ttf, NotoSansThaiUI-Regular/Bold.ttf (external/noto-fonts): SIL Open Font License 1.1|$noto/notosansthai/NOTICE" \
      "NotoSansDevanagari-VF.ttf (2.006), NotoSansDevanagariUI-VF.ttf (external/noto-fonts): SIL Open Font License 1.1|$noto/notosansdevanagari/NOTICE" \
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
