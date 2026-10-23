# 0022. Fonts: where the system fonts live and how the framework gets them

Status: accepted (fonts track, Phase 3). The coordinator may renumber.

## Context

Android's text stack (Typeface, minikin, hwui) takes its fonts from the
system font map: `/system/etc/font_fallback.xml` (Android 16; `fonts.xml` is
the legacy file, still read by `Typeface.init()`) names the families, the
files are in `/system/fonts/`. `SystemFonts` holds both directories as
constants (`DEVICE_FONTS_XML_DIR`, `DEVICE_FONT_DIR`; only Ravenwood replaces
them), and `FontListParser.parse(InputStream)` hard-codes `/system/fonts/`.

On a device the system server builds the map once (FontManagerService:
`SystemFonts.getSystemFontConfig`, `buildSystemFallback`,
`buildSystemTypefaces`, `Typeface.serializeFontMap` into a SharedMemory)
and every app process installs it in `bindApplication`
(`Typeface.setSystemFontMap(SharedMemory)`), falling back to
`loadPreinstalledSystemFontMap()` (the same build in the app) when no buffer
came. `Typeface.DEFAULT` and the other static typefaces are placeholders
until then (`ENABLE_LAZY_TYPEFACE_INITIALIZATION`).

In Gesher `/system/...` is a path on the current drive
([0010](0010-paths.md)): there are no system fonts, `Typeface.DEFAULT`
throws "not fully initialized", and text with the default typeface fails.
`$ANDROID_ROOT` (`%LOCALAPPDATA%\Gesher\system` for the installed build) is
where Gesher keeps Android's system directory: `build.prop` and
`framework/framework-res.apk` are already there.

## Decision

- **Fonts live in `$ANDROID_ROOT`, laid out as on a device**:
  `$ANDROID_ROOT/fonts/*.ttf` and `$ANDROID_ROOT/etc/fonts.xml`. The build
  collects them from the AOSP checkout (`tools/fonts/collect-fonts.sh` →
  `fonts/` in the build output); the installer puts them in `{app}\fonts`
  and gesher-app's launcher copies them into `$ANDROID_ROOT` the way it
  copies `build.prop` (only when a file's size changed).
- **Gesher's own `fonts.xml`** (`config/fonts/fonts.xml`) is AOSP's, trimmed
  to the families Gesher ships, in the legacy syntax both parsers read:
  sans-serif and sans-serif-condensed (Roboto 3.011, one variable file),
  serif (Noto Serif), monospace (DroidSansMono), and the Hebrew fallback
  (Noto Sans Hebrew 1.040), with their aliases. About 3.7 MB.
- **The paths are not mapped; Gesher calls the framework's font pipeline
  with `$ANDROID_ROOT`'s paths** (`com.gesher.app.fonts.SystemFontMap`, run
  by gesher-app after its bootstrap): `FontListParser.parse(fontsXml,
  fontDir, …)`, `SystemFonts.buildSystemFallback`,
  `SystemFonts.buildSystemTypefaces`, `Typeface.serializeFontMap`, then
  `Typeface.setSystemFontMap(SharedMemory)` — the system server's and the
  app's halves of the device's startup in one process. Once the map comes
  from a buffer, `setSystemFontMap(null)` (a later `bindApplication` without
  one) is a no-op, so the real `handleBindApplication` will not replace it.
  If SharedMemory fails, the map is installed directly
  (`setSystemFontMap(Map)` + `loadNativeSystemFonts()`), as
  `loadPreinstalledSystemFontMap` would.
- **android.os.SharedMemory gets natives** (`compat/framework/shared_memory.cc`):
  a region is a gesher-base memfd (a delete-on-close temporary file that mmap
  maps with a file mapping), as compat/bionic's ASharedMemory. ashmem's
  protection mask is not modelled.

### Rejected

- **Mapping `/system` to `$ANDROID_ROOT` in gesher-base's paths.** It would
  make every hard-coded `/system/...` work, but 0010 deliberately keeps `/`
  as the drive's root, and the mapping would be partial: Skia opens font
  files by name with its own `_wfopen` (the serialized font map is read back
  by path, `SkData::MakeFromFileName`), and other CRT functions (`fopen`,
  `access`) are not mapped. Paths from gesher-base would also come back as
  `$ANDROID_ROOT/...` (realpath), not as the `/system/...` the caller used.
- **A frameworks/base patch to `SystemFonts.getFontsXmlDir()` /
  `getSystemFontDir()`** (read `$ANDROID_ROOT`, as Ravenwood does). The
  cleanest change for the framework, but a Java patch needs the file in
  frameworks/base's sparse set at fetch time; widening that aosp.lock line
  changes framework.yml's host-tools key (a full host-tools rebuild) and the
  AOSP caches. Worth doing if more framework Java needs patching anyway; then
  `SystemFontMap` reduces to `Typeface.loadPreinstalledSystemFontMap()`.
- **Windows' fonts** (`C:\Windows\Fonts`): not Android's metrics (apps lay
  out for Roboto), licensing does not allow redistribution, and the set
  differs between Windows versions.

## Consequences

- gesher-app's processes have the system font map: `Typeface.DEFAULT`,
  `Typeface.create("sans-serif", …)`, the aliases, the fallback chain
  (Hebrew from a Latin default), `SystemFonts.getAvailableFonts()`.
- Code that reads `/system/etc/fonts.xml` or `/system/fonts/` itself still
  finds nothing: `Typeface`'s static `init()` logs that it cannot open
  `/system/etc/fonts.xml` and skips its preloading (a performance hint
  only), and so would an NDK app that parses the file (Skia's
  SkFontMgr_android, Flutter). A native-libs path mapping for Android
  `.so` code can make those see `$ANDROID_ROOT`.
- A process that is not started by gesher-app (a plain dex program) gets
  the fonts by calling `SystemFontMap.load()` (the tests do).
