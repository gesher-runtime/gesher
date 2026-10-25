# Locales (locale track)

Design: [ADR 0049](decisions/0049-locale-per-app-and-formats.md), on top of
[ADR 0044](decisions/0044-rtl-scripts-and-locale.md) (the locale list from
Windows, docs/fonts.md "The locale from Windows"). Branch `wip/locale`.
Tests: `tests/windows/tracks/49-locale.ps1`, `tests/apps/localeapp`.

## Pieces

| Piece | Where |
|---|---|
| The device's locales from Windows, regional preferences (`-u-ca-`, `-u-fw-`, `-u-hc-`, `-u-mu-`, `-u-nu-`), `refresh()`, the custom formats not carried over | `app/java/com/gesher/app/locale/SystemLocale.java` (natives: `compat/hwui/text/text_natives.cc`, unchanged) |
| `locale` service: per-app locales (persisted in `$ANDROID_DATA/system/app_locales`), `configurationFor(package)`, WM_SETTINGCHANGE handling, the broadcasts, the single-process path | `app/java/com/gesher/server/LocaleManagerService.java`; registered in `Services.java`; stub `android.app.ILocaleManager` in `app/build.sh` |
| Configuration changes to processes and activities (ConfigurationChangeItem, ActivityConfigurationChangeItem, ActivityRelaunchItem; deferred until resumed) | `ActivityTaskManagerService.dispatchConfiguration`, `ensureConfiguration`, `configurationItem`, `resume`; `ActivityClientController.activityRelaunched` |
| The package's configuration where the server hands one out | `ActivityManagerService.bindApplication`, `ActivityTaskManagerService.realStartActivity`/`getTasks`, `StandInWindowSession` (the caller's package), `GesherAppDriver.bindApplication` (single process) |
| `Settings.System.TIME_12_24` following Windows' time format | `SettingsProvider.putSystem` |
| Windows' events | the devsvc track's hidden window (`WindowsHost`, `compat/devsvc/devsvc_win32.cc`: `WM_SETTINGCHANGE` → `EVENT_SETTING_CHANGED`) |

## Behavior

- **What an app sees**: `LocaleList.getDefault()` and its resources'
  configuration: the device's list (Windows' display languages, the regional
  format's region, regional preferences as `-u-` extensions), or its own
  locales in front of it. `LocaleManager.getSystemLocales()` is the device's,
  `getApplicationLocales()` the app's (empty: none). `LocaleConfig(context)`
  reads `android:localeConfig`. `DateFormat.is24HourFormat` is Windows' time
  format (TIME_12_24).
- **setApplicationLocales** (LocaleManager, AppCompatDelegate on API 33+):
  persisted per package; the app's process gets the new configuration, each
  activity is relaunched (saved state kept) or, with
  `android:configChanges="locale|layoutDirection"`, gets
  `onConfigurationChanged`; `ACTION_LOCALE_CHANGED` to the app. The next process
  of the app, and the next device, start with them. An empty list clears them.
- **Windows' settings changing** (WM_SETTINGCHANGE to the system server's
  events window): the device's list and time format are read again; when they
  changed, every running app gets the configuration change as above,
  `ACTION_LOCALE_CHANGED` (registered and manifest receivers of every package),
  `ACTION_CONFIGURATION_CHANGED` (registered), and when the 12/24-hour format
  changed `TIME_12_24` and `ACTION_TIME_CHANGED` with
  `EXTRA_TIME_PREF_24_HOUR_FORMAT`. A stopped activity is relaunched as it
  comes back to the front.
- **Single process** (`com.gesher.app.Main`, gesher-app without the device: no activity manager): the
  change is applied to the process (resources, default locales,
  `Application.onConfigurationChanged`) and to the activity when it handles it;
  an activity that does not is not recreated (logged:
  `locale: X is not recreated in a single process`).
- **Not carried over**: custom decimal and grouping symbols, custom date and
  time patterns (Android has no setting for them; logged by the system server
  as `Windows' custom formats not carried over`); `ACTION_APPLICATION_LOCALE_CHANGED`
  (no installer app); LocaleConfig overrides are not persisted.

Logs to look for: `system_server: locale: the device's locales ...`,
`locale: <package>'s app locales set to ...`, `locale: Windows' settings
changed: locales A → B, 24-hour false → true`, `scheduleTransaction
ConfigurationChangeItem to process ...`, `ActivityRelaunchItem (changes 0x...,
handles 0x..., <locales>)`, `ActivityConfigurationChangeItem (...)`.

## Status and next steps

- Done and verified: ci run 40191538466 (09464511, phase4/integration merged),
  every job green, both runners (windows-2022, windows-2025): the 13 `locale:`
  checks of 49-locale.ps1 (single process: what the app sees, setApplicationLocales
  applied in the process, kept across an app restart, cleared, de-DE and back
  delivered to a running activity that handles locale; the device: what the app
  sees, setApplicationLocales relaunching the activity with he-IL and sending
  LOCALE_CHANGED, kept across an app restart and a device restart, cleared,
  de-DE and back while the app runs: ConfigurationChangeItem,
  Application.onConfigurationChanged, LOCALE_CHANGED, CONFIGURATION_CHANGED,
  TIME_SET with the 24-hour extra, onConfigurationChanged in front, the stopped
  activity relaunched as it comes back and again on the restore), and 39-rtl's
  de-DE check now expecting `-u-mu-celsius`.
- Found on the way: an ActivityConfigurationChangeItem whose override config has
  seq 0 is dropped by the client (it compares it with itself as the pending one);
  the server gives it the change's seq (run 40142410272 failed on it).
- Windows' NLS data seen by a long-running process follows `Set-Culture` at once
  on the CI runners (no registry reading needed).
- Remaining gaps (by design or small): no recreation of activities in single-process
  mode; LocaleConfig overrides not persisted; `ACTION_APPLICATION_LOCALE_CHANGED`
  not sent (no installer app); app locales stay in app_locales when a package
  is uninstalled (a reinstall gets them back); a display-language list
  change is only as live as Windows' `GetUserPreferredUILanguages` answer (not
  tested on CI: changing it needs a sign-in); custom separators and date
  patterns not carried over (ADR 0049).
