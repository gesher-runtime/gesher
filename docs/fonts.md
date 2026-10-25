# System fonts (roadmap phase 3)

Goal: Android's system fonts on Windows — `Typeface.DEFAULT`, the font
families and fallback chain of `fonts.xml`, Hebrew through Noto — so text
drawn through minikin and hwui looks and measures as on a device; and text
in views right for every script a Windows user may read, right to left
included, in the user's Windows language. Work happened on branches
`wip/fonts` and `wip/rtl` (the rtl track: further scripts, right to left in
views, the locale; see "Right to left, further scripts, the locale" below).
The decision records are [ADR 0022](decisions/0022-fonts-system-font-map.md)
and [ADR 0044](decisions/0044-rtl-scripts-and-locale.md).

## What Gesher ships

From AOSP android-16.0.0_r4, the versions the BP4A release selects:

| File | Family | Source | License |
|---|---|---|---|
| `Roboto-Regular.ttf` (2.4 MB) | `sans-serif`, `sans-serif-condensed` (variable: `wght` 100–900, `wdth`, `ital`) | external/roboto-fonts `font/3.011` (`RELEASE_PACKAGE_ROBOTO_FONT_VERSION`) | Apache 2.0 |
| `NotoSerif-{Regular,Bold,Italic,BoldItalic}.ttf` (1 MB) | `serif` | external/noto-fonts `notoserif` | OFL 1.1 |
| `DroidSansMono.ttf` (108 KB) | `monospace` | frameworks/base `data/fonts` (`RELEASE_USE_ROBOTO_MONO_FONT` is false) | Apache 2.0 |
| `NotoSansHebrew-{Regular,Bold}.ttf` (35 KB) | fallback `und-Hebr` | external/noto-fonts `notosanshebrew/font/1.040` (`RELEASE_PACKAGE_HEBREW_FONT_VERSION`'s default) | OFL 1.1 |

Added by the rtl track (ADR 0044), at the versions BP4A's release flags select:

| File | Family | Source | License |
|---|---|---|---|
| `NotoNaskhArabic-{Regular,Bold}.ttf`, `NotoNaskhArabicUI-{Regular,Bold}.ttf` (472 KB) | fallback `und-Arab`, elegant and compact | external/noto-fonts `notonaskharabic/font/1.070`, `notonaskharabicui` | OFL 1.1 |
| `NotoSansThai-{Regular,Bold}.ttf`, `NotoSansThaiUI-{Regular,Bold}.ttf` (81 KB) | fallback `und-Thai` | `notosansthai`, `notosansthaiui` | OFL 1.1 |
| `NotoSansDevanagari-VF.ttf`, `NotoSansDevanagariUI-VF.ttf` (524 KB) | fallback `und-Deva` (variable `wght`) | `notosansdevanagari/font/2.006`, `notosansdevanagariui` | OFL 1.1 |

About 4.5 MB in all (18 files); before the rtl track 3.4 MB (8 files). `config/fonts/fonts.xml` is AOSP's `fonts.xml`
(Apache 2.0) cut down to these families, unchanged otherwise: sans-serif
with its 18 variable instances and aliases (`sans-serif-thin`, `-light`,
`-medium`, `-black`, `arial`, `helvetica`, `tahoma`, `verdana`),
sans-serif-condensed, serif and its aliases (`serif-bold`, `times`,
`georgia`, …), monospace (`sans-serif-monospace`, `monaco`), then the
Hebrew fallback family (and, since the rtl track, Arabic, Thai and
Devanagari). Left out: every other script's fallback, emoji,
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
- No updatable fonts (`/data/fonts`), no OEM customization
  (`/product/etc/fonts_customization.xml`). (Further scripts, CJK and emoji:
  see below.)
- ashmem's protection mask (`SharedMemory.setProtect`) is not enforced.

## Right to left, further scripts, the locale (rtl track)

ADR [0044](decisions/0044-rtl-scripts-and-locale.md). Branch `wip/rtl`.

### Further scripts: the fallback chain

- **Bundled** (above): Arabic, Thai and Devanagari Noto fonts in
  `fonts.xml`, elegant and compact (UI) variants; Roboto covers Latin, Greek
  and Cyrillic; Noto Sans Hebrew stays.
- **Windows' fonts in place**: `config/fonts/windows-fonts.xml` (in the build
  output's `fonts/`, the installer's `{app}onts`, copied to
  `$ANDROID_ROOT/etc/windows-fonts.xml` by the launcher and the tests) lists
  fallback families whose files are in the Windows fonts folder: CJK
  (Microsoft YaHei zh-Hans, JhengHei zh-Hant, Yu Gothic / Meiryo / MS Gothic
  ja, Malgun Gothic ko, SimSun-ExtB/ExtG), Segoe UI Emoji (`und-Zsye`, color
  COLR), Segoe UI Symbol, Nirmala UI (Indic), Leelawadee UI, Ebrima, Gadugi,
  Myanmar Text, Himalaya, Mongolian Baiti, Yi Baiti, Javanese Text, Tai Le,
  New Tai Lue, Phags-pa, MV Boli, Sylfaen, Segoe UI and Segoe UI Historic.
  `SystemFontMap.load()` parses it with the Windows folder as font directory
  and appends its families to `fonts.xml`'s fallback list; files a Windows
  lacks are dropped. Its log line says so: `… (4 named families, 33 fallback
  families; 25 fallback families (38 fonts) of Windows' from …; font map
  through SharedMemory, 153773 bytes)` (windows-2022).
- **The folder**: `com.gesher.app.fonts.WindowsFonts.directory()`:
  `$GESHER_WINDOWS_FONTS` (`none` turns it off), else `%WINDIR%\Fonts` from
  the native (`GetSystemWindowsDirectoryW`; `compat/hwui/text/text_natives.cc`)
  in Gesher's path form. It must be on the current drive (drive-less path,
  ADR 0010), as an installed Gesher is; the tests, whose root is on `D:`,
  make a junction `$root/windows-fonts` → `C:\Windows\Fonts` and name it with
  `$GESHER_WINDOWS_FONTS` (`32-fonts.ps1`).

### The locale from Windows

`com.gesher.app.locale.SystemLocale` (natives in `text_natives.cc`):
Windows' preferred UI languages (`GetUserPreferredUILanguages`) are the
locale list; the first takes the regional format's region when it is the
same language; first day of the week, hour cycle and digits that differ
from that locale's become `-u-fw-`, `-u-hc-`, `-u-nu-` extensions (Android
14+ regional preferences). `$GESHER_LOCALES` (e.g. `he-IL,en-US`) replaces
the list. Used by `DeviceProfile.configuration()` (locales and layout
direction of every Configuration, the system server's included, so app
processes on the device get it in `bindApplication`), `Main.bootstrap`
(`LocaleList.setDefault`) and the settings stand-in's `TIME_12_24`.
`ApkPackage` now sets `FLAG_SUPPORTS_RTL` from `android:supportsRtl` (the
parse never derived ApplicationInfo's flags), without which views ignore an
RTL layout direction.

### Tests

- `tests/dex-graphics/gesher/ScriptsTest.java` (32-fonts.ps1): Arabic from
  Noto Naskh Arabic UI, shaped (four joined seens are initial, medial and
  final glyphs; with ZWNJ the isolated one), drawn as one connected shape
  (four apart), right to left; Devanagari (ka-virama-ssa one conjunct glyph),
  Thai, Cyrillic, Greek with the bundled fonts' widths; Chinese, Japanese,
  Korean with glyphs, not the `.notdef` of U+0378; zh-CN text identical to
  msyh.ttc alone; 直 differs between ja (Yu Gothic) and zh-CN (YaHei); the
  emoji drawn in color, as wide as seguiemj.ttf alone; a symbol.
- `tests/dex-graphics/gesher/LocaleTest.java` (39-rtl.ps1): the locale list,
  default Locale/LocaleList, first day of week (java.util and ICU), 24-hour
  time and its pattern, number format against Windows' own settings, read by
  the script in a new process: the runner's (en-US), with the regional format
  set to en-GB (→ en-GB) and to de-DE (→ en-US-u-fw-mon-hc-h23), restored
  after; and `$GESHER_LOCALES`.
- `tests/apps/rtlapp` (rtlapp.apk, `android:supportsRtl`, `values-iw`,
  `values-ar`) and `tests/windows/tracks/39-rtl.ps1`: its window read back
  with PrintWindow; the view rectangles come from the app's log. Colored runs
  (Hebrew, Arabic: red then blue logically; mixed: Hebrew, "ABC", "123",
  Hebrew) must be ordered right to left with the LTR run inside; RTL
  paragraphs with gravity start at the right edge; Arabic joined (fewer
  connected shapes, narrower than with ZWNJ); textDirection rtl right,
  textAlignment viewStart with the layout, gravity end of an RTL paragraph
  left; CJK ink against the tofu line; emoji colored pixels; a row of
  red|blue mirrored with the layout, a row with `layoutDirection="ltr"` not.
  Three runs: en-US single process; `he-IL,en-US` single process, with
  arrow keys (posted WM_KEYDOWN/UP) moving the cursor of the RTL EditText
  visually (Left from offset 0 → 1 → 2, Right → 1, Shift+Left selects 1-2;
  End → 0 and Home → 4: Android moves them to the right and left edges,
  `Selection.moveToRightEdge`/`LeftEdge`, which in RTL text are the logical
  start and end); `ar-EG` on the Gesher device (app process started by the
  system server).

### Status and next steps

(Kept current with each push; a successor can continue from here.)

- Done, on wip/rtl: everything above. CI: run 40114899592 (build failed:
  a comment in fonts.xml named `…-VF.ttf`, which collect-fonts.sh took for a
  file); run 40128190431 (build, package green; tests: ScriptsTest's CJK
  check used `hasGlyph` on a multi-character string; rtlapp ran with
  `supportsRtl false`, so nothing mirrored; the EditText's cursor had moved
  to the end on focus; everything else passed, the locale checks included);
  run 40129151328: everything passed on both runners (scripts, locale,
  mirrored layouts, bidi, single process and device) except the last cursor
  step: End went to offset 0, which is Android's behavior (the right edge),
  not the test's expected 4; the test now expects End → 0, Home → 4.
- **CI-verified: run 40133992911 (commit 1c9b6b2), green on windows-2022 and
  windows-2025, package and e2e.** PASS lines: `dalvikvm: further scripts:
  …`, `dalvikvm: locale from Windows: …` (en-US, en-GB, de-DE →
  en-US-u-fw-mon-hc-h23), `dalvikvm: locale from $GESHER_LOCALES (he-IL,en-US)`,
  `gesher-app: rtlapp.apk under a left-to-right locale: …`, `gesher-app:
  rtlapp.apk under he-IL: …` (6 of 6 cursor moves), `gesher-app: the Gesher
  device under ar-EG: …`; the earlier fonts and windows checks still pass.
  The installed gesher-app (package job) uses Windows' fonts through the
  native's drive-less `/Windows/Fonts/` and the SharedMemory map (`… 25
  fallback families (38 fonts) of Windows' from /Windows/Fonts/; font map
  through SharedMemory, 154421 bytes`). Installer artifact 84.6 MB (was
  84.3 MB). The roadmap's Phase 3 fonts box is ticked.
- Not done / gaps: per-app locales, Windows settings changed while an app
  runs, and the regional formats are the locale track's (docs/locale.md, ADR
  0049; custom decimal/grouping symbols and date patterns stay not carried
  over, by decision); fonts on another drive than the current one need
  `$GESHER_WINDOWS_FONTS`; the emoji and CJK look is Windows' (Segoe UI
  Emoji, YaHei...), not Noto's.
