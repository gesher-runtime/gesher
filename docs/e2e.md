# End-to-end: real apps on the installed Gesher

The `e2e` job of `.github/workflows/ci.yml` (windows-2022 and windows-2025, after the `package`
job) installs the Gesher installer that job built on a clean runner and runs
real, unmodified open-source Android apps from F-Droid on it
(`tests/e2e/run-e2e.ps1`, [0034](decisions/0034-e2e-real-apps.md)). The apps
are pinned in `tests/e2e/apps.json` by version code and SHA-256 and are
downloaded from `https://f-droid.org/repo/<package>_<versionCode>.apk` in CI
only.

The transport uses a CI cache keyed by the manifest and verifies every SHA-256
again on cache hits. Downloads go to a temporary file and are renamed only
after the pin matches. If the primary repository fails, it tries the configured
mirror (ftp.fau.de, listed in [F-Droid's mirror documentation](https://f-droid.org/eu/docs/Running_a_Mirror/)).
Versions, pins and app assertions are unchanged. The transport regression uses
tiny own fixture bytes to exercise DNS fallback, cache reuse, corrupt cache
replacement and rejection of mismatched downloads; no APK is downloaded locally.
These transport changes are pending full CI after the DNS failure in
40334251458's windows-2022 e2e job.

The table and failure history below describe the initial e2e stage. Puzzles
and the later required TIC-80 checks now pass on both runners in ci 40320549410;
remaining service contracts are tracked in services3, not waived by smoke tests.

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

## Structured evidence and the coverage denominator

The runner also writes `observations.json` before app installation and updates it
after each scenario transition. The `e2e` job always runs `tests/e2e/coverage.py`
afterwards, including after a failed runner step. The resulting `coverage.json`
and `coverage.md` are uploaded with the logs; the Markdown is also in the job
summary. A missing, malformed, stale or interrupted observation ledger fails
the evidence gate instead of becoming an empty successful run.

Each invocation owns its output directory through an exclusive file handle,
acquired before clearing earlier reports and held through the final checkpoint.
A concurrent writer is rejected before touching the active run's evidence.
The handle is released on normal completion, exceptions or process death; the
lock file stays in place so ownership cannot split across different files.
Every run has a fresh invocation ID. CI supplies that ID to the runner and
requires the same ID when reporting, in addition to checking run/attempt/checkout
identity. A successful report from another invocation cannot satisfy this gate.

The denominator is **every package/version entry in the supplied manifest**.
`-Only` exclusions remain visible as `skipped`. A default launch and a requested
`surface-trace` are separate scenarios, so a traced pass cannot replace a failed
default result. The `.apk` association and shortcut checks have separate
integration results. Checks not reached are `not_run` or `blocked`, and an
interrupted active check remains `running`.

`knownFailure` preserves the runner's existing CI waiver, but the report keeps
the actual failed or blocked outcome. A diagnostic failure is not waived. If a
known failure prevents launch altogether, its dependent trace is explicitly
blocked. Unknown `-Only` packages are rejected rather than running nothing.

All current real-app checks are classified as **visual/lifecycle smoke**.
Drawing pixels, reacting to clicks or matching the existing log assertions
does not establish a complete functional workflow or an Android differential
oracle. Therefore this report explicitly shows zero demonstrated full
functional results and a `not_tested` functional status for every app, even
when all smoke checks pass. This is an evidence foundation for
[#56 A01/T14](https://github.com/gesher-runtime/gesher/issues/56), not completion
of those roadmap items or a percentage of all Android applications.

Identity fields distinguish measured bytes from declared or unknown values:

- The manifest and downloaded APK are hashed independently; a reported passing
  scenario must have an observed APK hash matching the manifest's pin.
- The installer is hashed when it is used. `-SkipInstall` records
  `existing_install` and leaves the installer hash unknown.
- Installed `dalvikvm.exe`, `gesher-app.jar`, `framework.jar` and `build.prop`
  are hashed when present. These are component identities, not a complete SBOM.
- SDK/version/ABI properties are recorded from installed `build.prop` and are
  labelled **declared properties**, not measured Java APIs or hardware support.
  `target_sdk` is taken from the actual successful package-parser log when
  available. Neither metadata source is inferred from an app's display name.
- The test checkout SHA is separate from the runtime source revision. The
  latter, AOSP tag, selected app ABI, modules, providers, hardware profile and
  Android reference stay null until a future collector can verify them.

### Measured APK identity and profile rows

`apk_identity.py` inspects a private copy of each exact pinned APK with
[AAPT2 `dump badging` and `dump xmltree`](https://developer.android.com/tools/aapt2#dump) and
[`apksigner verify --print-certs`](https://developer.android.com/tools/apksigner).
It does not execute APK code. The collector rejects an incorrect hash, package
or version code, a failed signature check, ambiguous metadata and a timeout.
The report records the SDK parser's minSdk/targetSdk, long version code, complete
app signer certificate SHA-256 set and packaged native ABI inventory. With
`multiArch`, both AAPT2's `native-code` and `alt-native-code` records contribute to
the inventory; neither is the runtime-selected ABI. An APK source-stamp
certificate is recorded separately as `source_stamp_certificate_sha256` and
never added to the application's signer set. The collector also hashes the
actual AAPT2 executable, apksigner JAR and Java launcher used for collection.
Certificate subject names and local tool paths are not included in the ledger.

For v3.1 rotation, apksigner prints certificates for API ranges rather than a
numbered current signer set. The source-launched `SdkApkSigners.java` adapter
uses the same SDK JAR's `ApkVerifier.Result.getSignerCertificates()` for the
current set. It records `signer_api_ranges` separately, including development
release targeting, without treating old range certificates as concurrent
signers or inferring the selected signer for a running device. This path needs
a JDK 17 or newer; CI supplies JDK 21. The adapter source hash is recorded with
the tool hashes. API-range observations are not the complete signing lineage.

`declared_min_sdk` and `declared_target_sdk` record attributes actually present
in the manifest's direct `uses-sdk` element. `min_sdk` and `target_sdk` record
the effective requirements: an omitted minimum is 1 for a numeric or absent
target, while a preview target codename propagates to an omitted minimum.
An omitted target inherits the minimum. Codenames remain symbolic; resolving
them requires a platform profile and does not establish release-device support.
These rules follow the [Android manifest contract](https://developer.android.com/guide/topics/manifest/uses-sdk-element)
and [AOSP's manifest parser](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/java/android/content/pm/parsing/ApkLiteParseUtils.java).
The collector inspects `dump xmltree` before applying these defaults; a missing
badging line alone cannot establish that an attribute was omitted. The reporter
checks the declared/effective relationship and compares the effective target
with Gesher's package-parser result, including for legacy APKs without `uses-sdk`.
An omitted `versionCode` similarly becomes zero only after xmltree confirms its
absence; `declared_version_code` distinguishes omission from explicit zero.

Each manifest version has a `compatibility_matrix` row, even when excluded or
not run. This connects the APK identity to the measured/declared runtime fields,
the smoke result and the still-untested functional result. Metadata rejected by
validation is not copied into the matrix as verified data. A disagreement
between SDK targetSdk and the runtime's package-parser targetSdk fails the
evidence gate. Packaged ABIs do **not** identify the ABI selected or executed by
Gesher, and verified signatures do not establish trust in their signer.

CI installs Android SDK build-tools 36.1.0 and requires measured identity for
every selected APK. The reporter independently requires that mode, so removing
the runner flag cannot weaken the gate. A known application failure cannot
waive missing or invalid identity evidence. Local smoke runs can leave the
collector optional: a missing SDK or failed collection remains explicitly
`unavailable`, with no verified metadata. Use `-RequireApkIdentity` to apply the
same gate locally and `-BuildTools <directory>` to select installed tools.
Without an explicit directory the collector searches `ANDROID_HOME` and
`ANDROID_SDK_ROOT` for the newest installed stable build-tools; it never
downloads tools itself. The legacy singular `signer_sha256` field remains null;
the measured set is `sdk_identity.signer_certificate_sha256`.

The `e2e-evidence` workflow also builds and signs project-owned,
non-executable fixture APKs on Linux and Windows. It verifies the actual SDK
collector against valid bytes, mismatched manifest identity and modifications
after signing, a four-ABI `multiArch` APK, source stamps with a distinct key,
omitted version/SDK attributes, and real v3.1 key rotation. These tests establish
metadata handling, not app compatibility.
Signer rotation history, split-APK sets and measured runtime/provider/hardware
selection remain follow-up work in issue #56 A01/T01/T13/T14.

To regenerate a report locally after a Windows run:

```text
python tests/e2e/coverage.py --manifest tests/e2e/apps.json --observations e2e-results/observations.json --out-dir e2e-results
```

Append `--require-apk-identity` when validating a run made with
`-RequireApkIdentity`, including all current CI runs.

The command exits nonzero on invalid evidence, incomplete selected scenarios,
an incomplete run, or an unwaived failure/block. It does not turn known failures
into passes, and it does not require full functional coverage from a smoke
suite. The `e2e-evidence` workflow runs the report's Python contracts and the
actual PowerShell writer's isolated fixture tests on Linux and Windows without
downloading or executing APKs. Those fixture tests validate the evidence
pipeline only; they are not runtime compatibility results.

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
| `startActivities` (TaskStackBuilder) unanswered | Puzzles' launcher activity | ordered task history is retained; only visible activities are created, and Back creates an uninstantiated parent when needed (docs/lifecycle.md) | `ActivityTaskManagerService` |
| No PendingIntents (`getIntentSenderWithFeature` answered null) | NoNonsense's sync scheduler | PendingIntents, lite: one IIntentSender per key, the usual flags, `send` starts the activity, broadcast or service | `PendingIntents.java`, `ActivityManagerService` |
| `android.os.ext.SdkExtensions` missing (AndroidX BuildCompat) | NoNonsense | placeholder: every extension at 0 | `app/java/android/os/ext/SdkExtensions.java` |
| No `SecureRandom` (Conscrypt is not on the boot class path): `UUID.randomUUID()` in AndroidX Fragment's constructor | NoNonsense | `SecureRandom` over `/dev/urandom` in Gesher's JCA provider | `app/java/com/gesher/app/Digests.java` |
| `SystemSensorManager` natives missing | Puzzles | zero-answer device stand-ins: no sensors (ci run 40069998825) | `cmake/aosp/framework.cmake` |
| `sincos`/`sincosf` missing in the bionic shim | Puzzles' libpuzzles.so | forwarded to the CRT's sin and cos | `compat/bionic/shim.cc` |
| libphonenumber is not in framework.jar: `Linkify.addLinks(..., ALL)` died on a stackless NoClassDefFoundError on the main thread (found with a method trace, `"trace": true` in apps.json, decoded by hand: Linkify.gatherTelLinks) | NoNonsense (it linkifies every title) | the framework build bundles libphonenumber (`libphonenumber-platform`, which Android's `ext` boot jar links in) into framework.jar, its metadata as resources; it first had placeholders that found no numbers, and Gmail's compose window died on the missing `PhoneNumberUtil$PhoneNumberFormat` when a recipient was typed; dbapp linkifies a text, FrameworkFixesTest formats and parses numbers (94-framework-fixes) | `tools/framework/plan.py` (`EXT_BUNDLED`), `build.py` |
| tagsoup (frameworks/base's `ext` boot jar) is not on the boot class path: `android.text.Html.fromHtml` died on a stackless NoClassDefFoundError at its `new Parser()`, whatever the markup; Compose caught it in composition ("Error was captured in composition"), so the screen stayed as it was (found with an ART method trace, `-Xmethod-trace-stream` in a single-process run, decoded by hand) | Wikipedia (its onboarding's forward arrow: the next page's text goes through Html.fromHtml) | the framework build compiles tagsoup's sources (compile only); `app/build.sh` puts their classes into gesher-app.jar, as framework-location's; FrameworkFixesTest parses HTML (94-framework-fixes) | `app/build.sh` |
| `gesher-app --system-server` raced binderd and servicemanager | the package job (ci run 40067653118) | the launcher waits for binderd's pipe and the system server's registration | `tools/package/gesher-app.ps1` |

## Remaining gaps

- **Simon Tatham's Puzzles (known failure)**: GamePlay starts, libpuzzles.so
  loads through the ELF loader and its JNI works, but new games are
  generated by `lib/x86_64/libpuzzlesgen.so` run as a separate executable
  (`ProcessBuilder`); Gesher cannot run an Android ELF executable as a
  process, so the game view stays empty (native-libs: ELF executables).
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
