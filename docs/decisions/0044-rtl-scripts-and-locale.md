# 0044. Right to left, further scripts and the locale: bundled Noto where small, Windows' fonts in place, Windows' languages

Status: accepted (rtl track, Phase 3). The coordinator may renumber.

## Context

[0022](0022-fonts-system-font-map.md) gave Gesher a system font map with
Roboto, Noto Serif, DroidSansMono and Noto Sans Hebrew. Everything else a
Windows user may read was tofu: Arabic, Devanagari, Thai, CJK, emoji and
symbols. And the device was always en-US: `DeviceProfile` put `en-US` in
every Configuration with a left-to-right layout direction, so a Hebrew or
Arabic Windows user got neither a mirrored layout nor the app's translated
resources, and dates, times and numbers in American formats.

What a device ships for these scripts (android-16.0.0_r4, external/noto-fonts):

| Script | Files | Size |
|---|---|---|
| Arabic | NotoNaskhArabic{,-Bold} 1.070, NotoNaskhArabicUI-{Regular,Bold} | 472 KB |
| Devanagari | NotoSansDevanagari-VF 2.006, NotoSansDevanagariUI-VF | 524 KB |
| Thai | NotoSansThai{,UI}-{Regular,Bold} | 81 KB |
| CJK | NotoSansCJK-Regular.ttc (static) / NotoSansCJK-VF.otf.ttc | 19.5 MB / 32.7 MB |
| Emoji | NotoColorEmoji (CBDT bitmaps; emoji-compat's copy) | about 10 MB |

The installer (`gesher-setup`) is about 80 MB; CJK and emoji alone would add
30 to 43 MB to it, and to every user's `%LOCALAPPDATA%\Gesher\system\fonts`
(the launcher copies the fonts there).

Every Windows 10/11 (and Windows Server with the desktop experience) has, in
`%WINDIR%\Fonts`: Microsoft YaHei, JhengHei, Yu Gothic and Malgun Gothic
(CJK), Segoe UI Emoji (color, COLR/CPAL), Segoe UI Symbol, Nirmala UI
(Indic), Leelawadee UI (Thai, Lao, Khmer), Ebrima, Gadugi, Myanmar Text,
Microsoft Himalaya and more. Windows' fonts are licensed with Windows for use
on that machine; any Windows program renders with them by reading the files.
They may not be redistributed.

## Decision

### Fonts: a hybrid fallback chain

- **Bundle the small Noto fonts of the scripts a device covers with its own
  look**: Arabic (Noto Naskh Arabic, elegant, and Noto Naskh Arabic UI,
  compact), Thai (Noto Sans Thai and UI), Devanagari (Noto Sans Devanagari and
  UI, variable), at the versions Soong selects for BP4A, in AOSP's families of
  `fonts.xml` (minus their `fallbackFor="serif"` fonts). +1.08 MB. Latin,
  Greek and Cyrillic are Roboto's; Hebrew stays Noto Sans Hebrew.
- **Use Windows' fonts in place for the rest**: `config/fonts/windows-fonts.xml`
  (installed as `$ANDROID_ROOT/etc/windows-fonts.xml`) lists fallback
  families in `fonts.xml`'s format whose files are in the Windows fonts
  folder: CJK in AOSP's order (zh-Hans YaHei, zh-Hant JhengHei, ja Yu Gothic
  / Meiryo / MS Gothic, ko Malgun Gothic, SimSun-ExtB/ExtG for the
  supplementary planes), `und-Zsye` Segoe UI Emoji, `und-Zsym` Segoe UI
  Symbol, then Nirmala UI, Leelawadee UI, Ebrima, Gadugi, Myanmar Text,
  Himalaya, Mongolian Baiti, Yi Baiti, Javanese Text, New Tai Lue, Tai Le,
  Phags-pa, MV Boli, Sylfaen, and Segoe UI and Segoe UI Historic last.
  `SystemFontMap` parses it with `FontListParser` with the Windows fonts
  folder as the font directory (files that are missing are dropped, and a
  family left without fonts) and appends its families to `fonts.xml`'s
  fallback list before building the map. Nothing is copied: the font map
  (and its SharedMemory form) names the files where Windows keeps them.
  Alternatives of one family (Nirmala.ttc on newer Windows, Nirmala.ttf on
  older) are separate families; a family holds one font per style.
- **The folder** is `%WINDIR%\Fonts` (`GetSystemWindowsDirectoryW`, natives in
  `compat/hwui/text/text_natives.cc`), or `$GESHER_WINDOWS_FONTS` (`none`
  turns Windows' fonts off). It must be reachable as a drive-less path
  ([0010](0010-paths.md)): `SystemFonts` maps each font by
  `File.getAbsolutePath()`, the font map's SharedMemory form stores that path
  and the reader maps the file again by it, and Java treats `C:/...` as
  relative. An installed Gesher runs on Windows' drive (its launcher already
  assumes one drive); a test root on another drive (CI's `D:`) names a
  junction to the folder with `$GESHER_WINDOWS_FONTS`. When neither works,
  the map is built without Windows' fonts and the log says why.
- **Color emoji** are Segoe UI Emoji's COLR/CPAL layers, drawn by Skia's
  FreeType backend (Fontations is not built, ADR 0029); minikin prefers the
  `und-Zsye` family for emoji presentation.

Rejected: bundling Noto CJK and Noto Color Emoji (+30–43 MB, for glyphs every
Windows already has); reading every script from Windows (Arabic, Thai and
Devanagari would lose Android's look and metrics for 1 MB saved); copying
Windows' fonts into `$ANDROID_ROOT` (redistribution, and 60+ MB per user);
mapping `C:/...` paths in gesher-base or libcore (a path form ADR 0010 left
out, owned by other tracks).

### Locale: Windows' languages and regional format

`com.gesher.app.locale.SystemLocale` gives the device's `LocaleList`, which
`DeviceProfile.configuration()` (every process's Configuration, the system
server's included) and `Main.bootstrap` (`LocaleList.setDefault`) use:

- the list is Windows' preferred UI (display) languages, in order
  (`GetUserPreferredUILanguages`);
- the first locale takes the region of the user's regional format
  (`GetUserDefaultLocaleName`) when that is the same language (an en-US
  display with en-GB formats gives en-GB: its dates, first day, numbers);
- the regional settings that differ from that locale's own become Unicode
  extensions on it, as Android 14+ keeps its regional preferences: first day
  of the week (`-u-fw-`, `LOCALE_IFIRSTDAYOFWEEK`), hour cycle (`-u-hc-h23`/
  `h12`, the time format), digits (`-u-nu-`, `LOCALE_IDIGITSUBSTITUTION`);
- `$GESHER_LOCALES` (language tags, `,` between) replaces the list as it is,
  for a device in another language than Windows' (and for tests);
- the layout direction is the first locale's (`Configuration.setLayoutDirection`),
  so Hebrew, Arabic, Persian or Urdu Windows mirror the layouts of apps
  that declare `android:supportsRtl`;
- `Settings.System.TIME_12_24` (the settings stand-in) is Windows' time
  format's 12 or 24 hours.

Not taken over from Windows: custom decimal and grouping symbols, date
patterns or currency (Android has no setting for them; the locale's own
apply), and the regional format's language when it is not the display
language's (Android has one locale list). Settings changed while an app runs
are seen by the next process (no configuration change is sent).

## Consequences

- Arabic, Thai and Devanagari look and measure as on a device; CJK, emoji and
  symbols look like Windows' (YaHei, not Noto Sans CJK; Microsoft's emoji
  designs), and their metrics differ from a device's. What is covered depends
  on the Windows edition and its optional font features; a Windows without
  them (Server Core, Nano Server) shows tofu for those scripts only.
- The installer grows by about 1 MB, not 30–43 MB.
- Text and layouts follow the user's Windows language without app changes;
  an RTL Windows mirrors RTL-aware apps.
- Tests (`tests/windows/tracks/32-fonts.ps1`, `39-rtl.ps1`) run Windows'
  fonts through a junction on the test root's drive.
