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

- Code complete for the brief's four items (per-app locales, setting changes
  delivered, formats mapped or recorded as limits, tests); first CI run
  pending at the time of writing.
- Next: CI on both runners; then tick the roadmap boxes with the run id.
