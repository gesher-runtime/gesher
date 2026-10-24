# 0049. Locales: per-app locales in Gesher's server, Windows' setting changes as configuration changes, formats as regional preferences

Status: accepted (locale track, Phase 3/4). The coordinator may renumber.

## Context

[0044](0044-rtl-scripts-and-locale.md) gave every process the locale list of
the user's Windows languages and regional format, computed once per process.
Three things Android apps rely on were missing:

- **Per-app locales** (Android 13+): `LocaleManager.setApplicationLocales`
  (and `AppCompatDelegate.setApplicationLocales`, which calls it on API 33+)
  ask the `locale` service to give one app other locales than the system's;
  the system keeps them per package, puts them in that app's Configuration and
  recreates its activities. Without the service `getSystemService(LocaleManager)`
  fails, and apps with an in-app language picker do nothing.
- **Changes while apps run**: on a device, the user changing the language or
  the regional preferences reaches every running app as a configuration
  change (activities relaunched or `onConfigurationChanged`) and
  `ACTION_LOCALE_CHANGED`. Windows tells programs with `WM_SETTINGCHANGE`
  ("intl"); Gesher ignored it.
- **Formats**: Windows' regional format has more than the first day, hour
  cycle and digits 0044 took: the measurement system, the calendar, custom
  decimal and grouping symbols, custom date and time patterns.

## Decision

### Per-app locales: a LocaleManager in Gesher's server

`com.gesher.server.LocaleManagerService` serves `android.app.ILocaleManager`
as `locale` (registered with the other services, a generated stub for binder):

- `setApplicationLocales` keeps the package's language tags in
  `$ANDROID_DATA/system/app_locales` (what ATMS's PackageConfigPersister keeps
  in /data/system); an empty list clears them. Unknown packages are refused.
- The configuration of a package (`LocaleManagerService.configurationFor`) is
  the device's (`DeviceProfile.configuration()`) with the app's locales in
  front of the system's (LocaleOverlayHelper.combineLocales: the app's, then
  the system's not among them) and the layout direction of the first. It is
  what `bindApplication`, `LaunchActivityItem`, `getTasks` and the headless
  window session's relayout give that package, so an app restarted (or a
  device restarted) starts in its own locales.
- A change is a configuration change of that package only (below) and an
  `ACTION_LOCALE_CHANGED` broadcast to it, as Android 13+ sends one to the app
  whose effective locale changed. `ACTION_APPLICATION_LOCALE_CHANGED` (to an
  installer) is not sent: the device has no installer app.
- `getSystemLocales` answers the device's list; `get/setOverrideLocaleConfig`
  are kept per package in the server (not persisted). `android:localeConfig`
  needs nothing of the server: PackageParser2 puts the resource id in
  ApplicationInfo and `LocaleConfig(Context)` reads it from the app's
  resources.

### Configuration changes, as ATMS sends them

`ActivityTaskManagerService.updateConfiguration(package | all)`:

1. each running process of the package gets a `ConfigurationChangeItem` when
   its configuration differs from the one it was given (bindApplication or
   the last change): its resources, default locales and the non-UI
   components' (Application, services, providers) `onConfigurationChanged`;
2. then each activity, as `ActivityRecord.ensureActivityConfiguration`: when
   the public diff from the configuration it was launched with is all in its
   `android:configChanges` (`ActivityInfo.getRealConfigChanged`), an
   `ActivityConfigurationChangeItem` (`onConfigurationChanged`); else an
   `ActivityRelaunchItem` (saved state kept, `isChangingConfigurations`) and the
   lifecycle item that brings it back to where it was. Only the resumed
   activity is relaunched at once; one that is not (stopped under another,
   paused, launching) is relaunched or told when it is next resumed, as ATMS
   relaunches invisible activities when they become visible.

Every configuration carries a growing `Configuration.seq`, so an app process
applies them in order (ConfigurationController drops older ones) and a stale
relayout answer cannot undo a change. The relaunch preserves the window
(`preserveWindow`), as ATMS does for a configuration change.

### Windows' settings changing: WM_SETTINGCHANGE on the devsvc events window

The system server already has a hidden top-level window for Windows'
broadcasts (devsvc, [0038](0038-devsvc-windows-backed-services.md));
`WM_SETTINGCHANGE` is `WindowsHost.EVENT_SETTING_CHANGED`. The locale service
listens to it rather than adding a second window: `SystemLocale.refresh()`
reads the languages and formats again, and when the list or the 12/24-hour
format changed:

- every running app gets the new configuration (above);
- `ACTION_LOCALE_CHANGED` goes to every package's registered and manifest
  receivers (an implicit broadcast apps may still declare), and
  `ACTION_CONFIGURATION_CHANGED` to registered receivers;
- a changed time format sets `Settings.System.TIME_12_24` and sends
  `ACTION_TIME_CHANGED` with `EXTRA_TIME_PREF_24_HOUR_FORMAT`, as the Settings
  app does (`DateFormat.is24HourFormat` reads the setting).

The message's lParam ("intl", "ImmersiveColorSet", a policy name...) is not
looked at: reading the settings again is cheap, and only a real change is
delivered.

In gesher-app's single-process mode there is no activity manager driving the
activity: the same listener (the process's own events window) applies a change
with `ActivityThread.handleConfigurationChanged` (resources, default locales,
Application) and calls the activity's `onConfigurationChanged` when it handles
the change; an activity that does not is not recreated there (logged).

### Formats: regional preferences where Android has them, a log where it has not

Android 14+ keeps regional preferences as Unicode extensions of the first
locale. Beyond 0044's `-u-fw-` (first day), `-u-hc-` (hour cycle) and `-u-nu-`
(digits), `SystemLocale` now maps:

| Windows (GetLocaleInfoEx, user's) | Android |
|---|---|
| `LOCALE_IMEASURE` (metric / U.S.) | `-u-mu-celsius` / `-u-mu-fahrenhe` (temperature unit), when it is not the locale's (Fahrenheit where CLDR's measurement system is U.S.) |
| `LOCALE_ICALENDARTYPE` (Gregorian, Japanese era, Taiwan, Hijri, Thai Buddhist, Hebrew, Umm al-Qura) | `-u-ca-` (gregorian, japanese, roc, islamic, buddhist, hebrew, islamic-umalqura), when it is not the locale's |
| short time / time format with `H` | `TIME_12_24` and `-u-hc-` (0044) |

Each extension is added only when Windows' value differs from the locale's own
CLDR default, so an untouched regional format gives a plain tag.

**Not carried over, by decision**: custom decimal and grouping symbols
(`LOCALE_SDECIMAL`, `LOCALE_STHOUSAND` when not the locale's), custom short and
long date patterns and time formats (the user's value against the regional
format's own, `LOCALE_NOUSEROVERRIDE`). Android has no setting for them: every
`NumberFormat`, `DateFormat` and `DateTimeFormatter` uses the locale's CLDR
data (the old `Settings.System.DATE_FORMAT` is not read by the framework's
`DateFormat` any more). Faking them (patching ICU's data per process, or a made-up locale)
would make Gesher's apps format unlike any Android device and differently from
their own servers' expectations. The system server logs which custom formats
were not carried over (`locale: ... Windows' custom formats not carried over`).

## Consequences

- In-app language pickers work, per package, across app and device restarts;
  an app's activities relaunch into the new language as on a phone.
- The user changing Windows' language list or regional format (Settings,
  intl.cpl, `Set-Culture`) reaches running apps without restarting them; apps
  that declare `configChanges="locale"` get `onConfigurationChanged`.
- Windows' display-language list changes need the user to sign in again for
  most programs; Gesher reads `GetUserPreferredUILanguages` again on each
  `WM_SETTINGCHANGE` and passes on whatever Windows answers then.
- Tests: `tests/windows/tracks/49-locale.ps1` with `tests/apps/localeapp`
  (single process and the Gesher device; `Set-Culture` and `WM_SETTINGCHANGE`
  sent to the events window by handle, the format restored after).
