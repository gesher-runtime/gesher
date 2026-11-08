# Gesher's interface language

Gesher's own interface supports Hebrew and English. It follows Windows' display
language preferences automatically, in order, matching the language portion of
BCP 47 tags (`he-IL`, `he`, and the legacy `iw` alias all select Hebrew).
If none of the preferred languages is supported, English is used. Regional number
and date settings do not override the display language.

The launcher splash, native permission/account consent dialogs and account chooser, command-line
launcher messages, microG addon setup, WebView file filters, Start menu descriptions,
the apps' Start menu folder, and Gesher Camera use these translations. Hebrew
consent dialogs and splash text
use right-to-left reading; identifiers in consent dialogs are isolated from the
surrounding text. Camera declares Android RTL support. Android's own framework
resources (including permission labels and standard dialogs) already include
Hebrew and use the existing `SystemLocale` configuration.

The Windows installer and uninstaller use Inno Setup's bundled English/Hebrew
translations, with Hebrew RTL layout and translated Gesher-specific tasks and
Explorer commands. Building requires Inno Setup 6.5 or later (Hebrew is bundled).
Setup selects the Windows UI language on each run, without a language prompt.
Installed shell entries are refreshed by running Setup after changing Windows'
display language. The runtime reads Windows preferences on launch; its existing
system locale refresh also handles `WM_SETTINGCHANGE`.

An individual Android app's per-app language setting does not change Gesher's
own interface language. Third-party app and web content follow their own available
translations. Developer diagnostics, stack traces, package identifiers and command
syntax keep their technical spelling.

`GESHER_LOCALES=he-IL,en-US` can override the runtime language preference list
for tests or an individual launch, consistently with the existing Android locale
override. Setup follows Windows itself (or Inno's explicit `/LANG=he` switch).

## Adding or changing translations

Edit `tools/i18n/messages.json`, the canonical catalog. Every key must have a
nonempty translation in every supported language with matching `{0}`, `{1}`, etc.
placeholders. Run:

```sh
python tools/i18n/generate.py
python tools/i18n/check.py --check
```

The generator produces the Java and native catalogs, packaged PowerShell catalog,
Inno custom messages and camera resources. Commit generated files with the source
catalog. To add a language, add its `languages` entry (and `rtl` flag), translate
all keys, regenerate, and add its bundled Inno language to `gesher.iss`. The
Java/PowerShell/native selectors all use the same preference and fallback rules.

On Windows, run `tests/windows/shell/UiLocalizationChecks.ps1` using Windows
PowerShell 5.1. `tests/i18n/ui_locale_test.c` tests the native selector, including
environment overrides, without modifying Windows' settings.
