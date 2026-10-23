# System fonts (roadmap phase 3)

Goal: Android's system fonts on Windows — `Typeface.DEFAULT`, the font
families and fallback chain of `fonts.xml`, Hebrew through Noto — so text
drawn through minikin and hwui looks and measures as on a device. Work
happens on branch `wip/fonts`. The decision record is
[ADR 0022](decisions/0022-fonts-system-font-map.md).

## What Gesher ships

From AOSP android-16.0.0_r4, the versions the BP4A release selects:

| File | Family | Source | License |
|---|---|---|---|
| `Roboto-Regular.ttf` (2.4 MB) | `sans-serif`, `sans-serif-condensed` (variable: `wght` 100–900, `wdth`, `ital`) | external/roboto-fonts `font/3.011` (`RELEASE_PACKAGE_ROBOTO_FONT_VERSION`) | Apache 2.0 |
| `NotoSerif-{Regular,Bold,Italic,BoldItalic}.ttf` (1 MB) | `serif` | external/noto-fonts `notoserif` | OFL 1.1 |
| `DroidSansMono.ttf` (108 KB) | `monospace` | frameworks/base `data/fonts` (`RELEASE_USE_ROBOTO_MONO_FONT` is false) | Apache 2.0 |
| `NotoSansHebrew-{Regular,Bold}.ttf` (35 KB) | fallback `und-Hebr` | external/noto-fonts `notosanshebrew/font/1.040` (`RELEASE_PACKAGE_HEBREW_FONT_VERSION`'s default) | OFL 1.1 |

About 3.4 MB in all (8 files). `config/fonts/fonts.xml` is AOSP's `fonts.xml`
(Apache 2.0) cut down to these families, unchanged otherwise: sans-serif
with its 18 variable instances and aliases (`sans-serif-thin`, `-light`,
`-medium`, `-black`, `arial`, `helvetica`, `tahoma`, `verdana`),
sans-serif-condensed, serif and its aliases (`serif-bold`, `times`,
`georgia`, …), monospace (`sans-serif-monospace`, `monaco`), then the
Hebrew fallback family. Left out: every other script's fallback, emoji,
serif-monospace (Cutive Mono), casual, cursive, smallcaps, Source Sans Pro,
Roboto Flex. Adding a script is a font in `collect-fonts.sh`, its family in
`fonts.xml` (copied from AOSP's) and, if the font is not fetched yet, its
directory in aosp.lock's `external/noto-fonts` line.

`FONTS-NOTICES.txt` (installed in `{app}\licenses`) carries the four
NOTICE files.

## Where they live

As on a device, under the Android root: `$ANDROID_ROOT/fonts/*.ttf` and
`$ANDROID_ROOT/etc/fonts.xml`.

- **Build**: `tools/fonts/collect-fonts.sh <aosp> <out>` (ci.yml's build job)
  writes `fonts/` (the fonts and `fonts.xml`) and `FONTS-NOTICES.txt` into
  the `windows-x64` artifact; it fails if `fonts.xml` names a file that is
  not collected, or a collected font is not in `fonts.xml`.
- **Installer** (`tools/package/gesher.iss`): `{app}\fonts\*`, and the
  notices in `{app}\licenses`. `gesher-app.ps1` copies them to
  `%LOCALAPPDATA%\Gesher\system\fonts` and `...\system\etc\fonts.xml` (its
  `$ANDROID_ROOT`) when a file's size changed, as it does `build.prop`.
- **Tests** (`tests/windows/tracks/32-fonts.ps1`): into the test root's
  `system/fonts` and `system/etc`.

## How the framework gets them

`SystemFonts` and `Typeface` name `/system/etc/` and `/system/fonts/` as
constants, which on Windows are paths on the current drive (ADR 0010).
`com.gesher.app.fonts.SystemFontMap.load()` (gesher-app, run by `Main`
after its bootstrap) does with `$ANDROID_ROOT`'s paths what the system
server and the app process do at startup on a device:

1. `FontListParser.parse($ANDROID_ROOT/etc/fonts.xml, $ANDROID_ROOT/fonts/, …)`
   (font files that do not exist are filtered out, as on a device);
2. `SystemFonts.buildSystemFallback` and `buildSystemTypefaces`: the fallback
   chain of every named family, typefaces for families and aliases;
3. `Typeface.serializeFontMap` into a `SharedMemory` (FontManagerService's
   step) and `Typeface.setSystemFontMap(SharedMemory)` (ActivityThread's
   `handleBindApplication`): the typefaces are read back from the buffer
   (minikin's serialized font collections; each font file is mapped again by
   path), `nativeAddFontCollections` registers them for
   `SystemFonts.getAvailableFonts`, the pending `Typeface.DEFAULT`,
   `DEFAULT_BOLD`, `SANS_SERIF`, `SERIF`, `MONOSPACE` are completed, the
   native default typeface is set and the CSS generic families registered.

`android.os.SharedMemory` has natives for this
(`compat/framework/shared_memory.cc`, a gesher-base memfd). If it ever
fails, `load()` installs the map directly (`setSystemFontMap(Map)` and
`loadNativeSystemFonts()`) and says so in its result.

gesher-app logs `gesher-app: system fonts: N typefaces from … (…; font map
through SharedMemory, … bytes)`, or `none (no …/fonts.xml)`.

## Status

### Done and CI-verified

`tests/dex-graphics/gesher/FontsTest.java`, run by
`tests/windows/tracks/32-fonts.ps1` on windows-2022 and windows-2025
(`PASS  dalvikvm: system fonts: Typeface.DEFAULT, fonts.xml families, Hebrew
through the fallback chain, bold/italic`, ci run 40011630189):

- the font map from `$ANDROID_ROOT/etc/fonts.xml`: 25 typefaces (4 named
  families, 2 fallback families), serialized into a SharedMemory (about
  26 KB) and installed from it by `Typeface.setSystemFontMap`;
- the named families and aliases (`sans-serif`, `sans-serif-medium`,
  `sans-serif-condensed`, `serif`, `monospace`, `arial`, `times`);
- `Typeface.DEFAULT`, `DEFAULT_BOLD`, `SANS_SERIF`, `SERIF`, `MONOSPACE`
  initialized; `Typeface.create` with styles, weights and family names;
- `SystemFonts.getAvailableFonts()`: 61 fonts (font + axes instances), all
  in `$ANDROID_ROOT/fonts`;
- Latin text with the default typeface (a Paint with no typeface) measures
  and draws exactly as Roboto-Regular.ttf alone;
- Hebrew from a Latin default comes from the fallback chain: `hasGlyph`,
  widths and pixels identical to Noto Sans Hebrew alone (Roboto alone gives
  other widths); drawn right to left; a Hebrew `StaticLayout` paragraph is
  right to left and aligned right; mixed Latin and Hebrew measured;
- bold differs from regular (640 vs 629 px wide at 200 px, 962 vs 648 dark pixels at 32 px), `create(DEFAULT,
  BOLD)` is `DEFAULT_BOLD`'s face, bold Hebrew is Noto Sans Hebrew Bold;
  italic differs from upright; monospace is monospaced; serif is Noto Serif.

The installer (package job, same run): `{app}\fonts\fonts.xml`, every font
it names and `{app}\licenses\FONTS-NOTICES.txt` are installed, and the
installed gesher-app loads them (`PASS  installer: gesher-app loads the
installed system fonts`; its log: `gesher-app: system fonts: 25 typefaces
from …/Gesher/system/etc/fonts.xml (4 named families, 2 fallback families;
font map through SharedMemory, 26717 bytes)`).

### CI runs

- ci 40002967687: build failed (`jniCreateFileDescriptor` is in
  JNIPlatformHelp.h).
- ci 40006279655: fonts installed and loaded everywhere (package job
  green), but SharedMemory's `mapReadWrite` failed with ENOMEM (gesher-base
  refused a writable view ending inside the file's last page; fixed in
  `compat/posix/src/mman.cc`), and bold/regular widths compared at 32 px
  were both 102 (whole-pixel advances).
- ci 40010423197: SharedMemory font map works; only the 32 px width
  comparison failed.
- ci 40011630189: green.

## Known gaps

- `Typeface`'s static initializer reads `/system/etc/fonts.xml` itself (to
  preload the files of the locale's script and register locale lists): on
  Windows it logs that the file cannot be opened and skips that — a
  start-up optimisation only.
- Code that parses `/system/etc/fonts.xml` or lists `/system/fonts/` by
  itself finds nothing (some NDK engines do: Skia's SkFontMgr_android,
  Flutter). For Android `.so` code a path mapping in the native-libs layer
  could point them at `$ANDROID_ROOT`.
- A dex program not started by gesher-app has no font map until it calls
  `SystemFontMap.load()`.
- No emoji, CJK or other scripts' fonts yet; no updatable fonts
  (`/data/fonts`), no OEM customization (`/product/etc/fonts_customization.xml`).
- ashmem's protection mask (`SharedMemory.setProtect`) is not enforced.
