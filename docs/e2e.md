# End-to-end: real apps on the installed Gesher

The `e2e` job of `.github/workflows/ci.yml` (windows-2022, after the `package`
job) installs the Gesher installer that job built on a clean runner and runs
real, unmodified open-source Android apps from F-Droid on it
(`tests/e2e/run-e2e.ps1`, [0034](decisions/0034-e2e-real-apps.md)). The apps
are pinned in `tests/e2e/apps.json` by version code and SHA-256 and are
downloaded from `https://f-droid.org/repo/<package>_<versionCode>.apk` in CI
only.

For each app the script:

1. downloads it and checks its SHA-256;
2. `gesher-app install` (the package parsed by the system server, its Start
   menu shortcut made);
3. launches it on the Gesher device with an off-desktop window
   (`gesher-app <apk> --hidden-window`) and waits for the first frame
   (`first frame drawn ... hwnd 0x...` in the app's output);
4. reads the window back with PrintWindow and checks it is not blank: at least
   0.5% of its pixels away from the background (the most common color) and 150
   "text" pixels (far from the background in luminance); saves it as
   `<package>.launch.png`;
5. posts the app's actions from the manifest (a click at client coordinates,
   keys typed with WM_KEYDOWN/WM_KEYUP) and checks that each changes at least
   30 pixels (`<package>.launch.after-N.png`);
6. closes the window (WM_CLOSE, then any other window of the app) and checks
   that the launcher exits with 0 and the app's process is gone.

The first app that passes is also started through the `.apk` association's
registered command (`HKCU\Software\Classes\Gesher.Apk\shell\open\command`,
`%1` replaced, `GESHER_APP_WINDOW=hidden-window`) and through its Start menu
shortcut's command line (`gesher-app.cmd launch <package>`), each to a drawn,
not blank first frame and a clean close.

Each check prints a `PASS` or `FAIL` line; a `FAIL` carries the first
exception or crash of the app's log. An app with `knownFailure` in the
manifest prints `KNOWN-FAIL` with the reason and does not fail the job; it is
still run every time. The job also lists the system server's stand-in methods
the apps called that answer only a default. Everything (PNGs, the launcher's
and the app's logs, `system_server.log`, `summary.txt`) is the run's
`e2e-results` artifact.

## The apps

| App | Version | License | Why | Status |
|---|---|---|---|---|
| [RPN calculator](https://github.com/lpar/RPN) `com.ath0.rpn` | 2.0.4 (18) | GPL-3.0 | One Activity of framework widgets, pure Java, no AndroidX; Material buttons (StateListAnimator), an EULA dialog, clipboard menus | **PASS**: clicks on 1 and 2 show 12 |
| [Editor](https://github.com/billthefarmer/editor) `org.billthefarmer.editor` | 1.98 (198) | GPL-3.0 | Text editor: a full-window monospace EditText in scroll views, action bar menus, SharedPreferences, files, all-files access | **PASS**: typed text appears |
| [Currency](https://github.com/billthefarmer/currency) `org.billthefarmer.currency` | 1.58 (158) | GPL-3.0 | Converter: ListView and adapter, EditText, SharedPreferences, a rates fetch that checks connectivity first, app widgets | **PASS**: "No connection", a click selects USD as the base |
| [NoNonsense Notes](https://github.com/spacecowboy/NotePad) `com.nononsenseapps.notepad` | 7.2.7 (72700) | GPL-3.0 | Notes and lists: SQLite through its own ContentProvider, loaders, AndroidX AppCompat, Material, ViewPager, fragments, a sync scheduler (alarms, PendingIntents), Linkify | **PASS**: its welcome task from its SQLite database; a click ticks it |
| [Unit Converter Ultimate](https://github.com/physphil/UnitConverterUltimate) `com.physphil.android.unitconverterultimate` | 5.7.3 (50703) | Apache-2.0 | AndroidX AppCompat, Material components (navigation drawer, toolbar, FAB), fragments, RadioGroups, Java and Kotlin | **PASS**: a unit chosen, a digit typed, the result converted |
| [Simon Tatham's Puzzles](https://github.com/chrisboyle/sgtpuzzles) `name.boyle.chris.sgtpuzzles` | 2025-09-12 (149) | MIT | NDK: `lib/x86_64/libpuzzles.so` over JNI, drawing on a Canvas through Java callbacks; AndroidX | **KNOWN-FAIL**: see below |

All six install and draw their first frame on Gesher; five pass every check.

### Verified in CI

ci run **40077182991** (commit badcd8a; the `e2e` job on windows-2022 is
green, and so is the `package` job):

```
PASS  e2e: com.ath0.rpn 2.0.4 (RPN calculator) installs, draws its first frame (not blank), reacts to input, closes and its process ends
PASS  e2e: org.billthefarmer.editor 1.98 (Editor) installs, draws its first frame (not blank), reacts to input, closes and its process ends
PASS  e2e: org.billthefarmer.currency 1.58 (Currency) installs, draws its first frame (not blank), reacts to input, closes and its process ends
PASS  e2e: com.nononsenseapps.notepad 7.2.7 (NoNonsense Notes) installs, draws its first frame (not blank), reacts to input, closes and its process ends
PASS  e2e: com.physphil.android.unitconverterultimate 5.7.3 (Unit Converter Ultimate) installs, draws its first frame (not blank), reacts to input, closes and its process ends
KNOWN-FAIL  e2e: name.boyle.chris.sgtpuzzles 2025-09-12-1919-23762278-fdroid (Simon Tatham's Puzzles): click at 100,200 did not change the window (0 pixels). ... (known failure: ... libpuzzlesgen.so run as a separate executable ...)
PASS  e2e: com.ath0.rpn starts through the .apk association's registered command, draws, closes
PASS  e2e: com.ath0.rpn starts through its Start menu shortcut 'RPN' ("...\Gesher\gesher-app.cmd" launch com.ath0.rpn), draws, closes
INFO  e2e: 0 unexpected failures
```

and the test job (both runners) `PASS  e2e: SQLite (SQLiteOpenHelper onCreate
and onUpgrade, a transaction, a Cursor, the UNICODE collator, in-memory) and
SharedPreferences in dbapp.apk` and `PASS  e2e: number and date formats
(java.text and ICU NumberFormat, SimpleDateFormat, DateFormat) and Linkify in
dbapp.apk` (`tests/windows/tracks/70-e2e.ps1`).

Screenshots: the `e2e-results` artifact of that run, e.g.
`com.ath0.rpn.launch.after-2.png` (the display shows 12),
`org.billthefarmer.editor.launch.after-2.png` ("Gesher" typed in the
monospace editor), `org.billthefarmer.currency.launch.png` (the header below
the action bar, "No connection"), `com.nononsenseapps.notepad.launch.png`
(its "Welcome!" task, read from its SQLite database) and `.after-1.png` (the
task ticked), `com.physphil.android.unitconverterultimate.launch.after-2.png`
(1.03 km² = 1030000 m²), `name.boyle.chris.sgtpuzzles.launch.png` (GamePlay,
no game).

## Gaps the apps hit, fixed

In the order the apps hit them; each with the run that showed it fixed.

| Gap | Hit by | Fix | Where |
|---|---|---|---|
| No SQLite: `android.database.sqlite` and `CursorWindow` natives missing | NoNonsense Notes (and every Room/SQLiteOpenHelper app) | external/sqlite (sqlite-autoconf-3500400 with libsqlite3_android and ICU) built as Soong builds it for Windows hosts (SQLite's Win32 VFS); `android_database_*.cpp` in libandroid_runtime, their tables made System V by patch 0200; `tests/apps/dbapp` (SQLiteOpenHelper create/upgrade, a transaction, a Cursor ordered by the UNICODE collator, in-memory, SharedPreferences): `PASS  e2e: SQLite ... and SharedPreferences in dbapp.apk` (ci run 40065911293) | `cmake/aosp/framework_e2e.cmake`, `aosp.lock`, `patches/frameworks/base/0200-e2e-sysv-jni-tables.patch`, `tools/jni-sysv-patch.py` (`e2e` set), `compat/framework/android_runtime.cc`, `tests/windows/tracks/70-e2e.ps1` |
| `StatFs("/data")` failed (SQLiteGlobal's page size) | SQLite users | `statvfs` of a missing absolute path answers for the volume it would be on | `compat/posix/src/system.cc` |
| `PropertyValuesHolder` natives missing (ObjectAnimator; every Material button's StateListAnimator) | RPN | `android_animation_PropertyValuesHolder.cpp` in libandroid_runtime (ci run 40066631812) | as above |
| `getSystemService` null for services every phone has: `power` (+`thermalservice`), `alarm`, `clipboard`, `appops`, `appwidget`, `search` | Editor (appops), Currency (appwidget), NoNonsense (alarm, search) | stand-ins answering like a device without the hardware behind them (screen on, alarms accepted and not delivered, one device clipboard, every op allowed, no widgets, no searchables) | `app/java/com/gesher/server/DeviceServices.java`, `Services.java`, `app/build.sh` |
| `ConnectivityManager` absent (a mainline module) | Currency | placeholders with the module's public API: no active network, no networks, callbacks never called; registered by `ConnectivityFrameworkInitializer` (Currency shows "No connection", ci run 40066631812) | `app/java/android/net/*` |
| No shared storage volume: `Environment.getExternalStorageDirectory()` and `isExternalStorageManager()` index an empty list | Editor | the `mount` stand-in has the primary emulated volume, `$ANDROID_DATA/media/<user>` | `Services.StorageService` |
| Every app-compat change enabled for every app (empty `disabledCompatChanges`): apps targeting 35 lost `windowOptOutEdgeToEdgeEnforcement` and drew their content under the action bar | Currency, Editor | `app/gen/CompatChangesGen.java` reads the framework's `@ChangeId` fields (181, from the sources: the annotations have SOURCE retention) at build time; `CompatChanges.disabledFor(targetSdk)` as PlatformCompat (ci run 40069998825) | `app/gen/`, `CompatChanges.java`, `ActivityManagerService.bindApplication` |
| No system features (`hasSystemFeature` always false): AppWidgetManager is null without `app_widgets` | Currency | the device declares `app_widgets`, `faketouch`, both screen orientations, in the package manager and in the shared memory's `SystemFeaturesCache` | `PackageManagerService.FEATURES` |
| `startActivities` (TaskStackBuilder) unanswered | Puzzles' launcher activity | the last activity is started (the ones below are not recorded) | `ActivityTaskManagerService` |
| No PendingIntents (`getIntentSenderWithFeature` answered null) | NoNonsense's sync scheduler | PendingIntents, lite: one IIntentSender per key, the usual flags, `send` starts the activity, broadcast or service | `PendingIntents.java`, `ActivityManagerService` |
| `android.os.ext.SdkExtensions` missing (AndroidX BuildCompat) | NoNonsense | placeholder: every extension at 0 | `app/java/android/os/ext/SdkExtensions.java` |
| No `SecureRandom` (Conscrypt is not on the boot class path): `UUID.randomUUID()` in AndroidX Fragment's constructor | NoNonsense | `SecureRandom` over `/dev/urandom` in Gesher's JCA provider | `app/java/com/gesher/app/Digests.java` |
| `SystemSensorManager` natives missing | Puzzles | zero-answer device stand-ins: no sensors (ci run 40069998825) | `cmake/aosp/framework.cmake` |
| `sincos`/`sincosf` missing in the bionic shim | Puzzles' libpuzzles.so | forwarded to the CRT's sin and cos | `compat/bionic/shim.cc` |
| libphonenumber is not in framework.jar: `Linkify.addLinks(..., ALL)` died on a stackless NoClassDefFoundError on the main thread (found with a method trace, `"trace": true` in apps.json, decoded by hand: Linkify.gatherTelLinks) | NoNonsense (it linkifies every title) | `com.android.i18n.phonenumbers.PhoneNumberUtil`/`PhoneNumberMatch` placeholders that find no numbers; dbapp linkifies a text (ci run 40076131169) | `app/java/com/android/i18n/phonenumbers/` |
| `gesher-app --system-server` raced binderd and servicemanager | the package job (ci run 40067653118) | the launcher waits for binderd's pipe and the system server's registration | `tools/package/gesher-app.ps1` |

## Remaining gaps

- **Simon Tatham's Puzzles (known failure)**: GamePlay starts, libpuzzles.so
  loads through the ELF loader and its JNI works, but new games are
  generated by `lib/x86_64/libpuzzlesgen.so` run as a separate executable
  (`ProcessBuilder`); Gesher cannot run an Android ELF executable as a
  process, so the game view stays empty (native-libs: ELF executables).
- **Phone numbers** are never detected (libphonenumber placeholder); the
  framework build should compile libphonenumber in (framework track).
- **Alarms** are accepted and never delivered; **PendingIntents** have no
  cancel listeners and `startActivities` keeps only the top activity.
- **No network state**: ConnectivityManager always says "no network", though
  the app's own sockets work (Windows' network).
- **Clipboard** is the device's own, not Windows'.
- Stand-in calls the apps make that answer only a default (from the job's
  INFO line): `IActivityClientController.navigateUpTo`,
  `setRequestedOrientation`, `IAlarmManager.remove`,
  `IAppWidgetService.notifyProviderInheritance`,
  `IInputManager.getVelocityTrackerStrategy`, `registerInputDevicesChangedListener`,
  `setPointerIcon`, `IInputMethodManager.startInputOrWindowGainedFocus`,
  `IPackageManager.queryIntentContentProviders`, `IWindowManager.setInTouchMode`.
- Not covered by these apps: WebView (absent: an app that creates one fails),
  media and audio playback, camera, location, notifications' UI, multiple
  windows of one app, Play Services.

## Running it

```
pwsh tests/e2e/run-e2e.ps1 -Setup installer\gesher-setup-*.exe -Out e2e-results [-Only <package>,...]
```

on a Windows machine without Gesher installed (it installs it), with network
access to f-droid.org. To add or update an app, add its entry to
`tests/e2e/apps.json` with an empty `sha256`: the first run prints the hash
(`INFO  e2e: <package> <versionCode>: sha256 ... (not pinned yet)`) to pin.
