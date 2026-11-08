# The Windows installer

`tools/package/gesher.iss` (Inno Setup 6) is built and tested by the
`package` job in `.github/workflows/ci.yml`. It installs per user, without
administrator rights, into `%LOCALAPPDATA%\Programs\Gesher`, puts `gesher`
and `gesher-app` on the user's `PATH`, adds Start menu entries (including the
Gesher Control Center), offers optional 1-click Google Play services (microG)
and Aurora Store installation, and associates `.apk`, `.apks` and `.xapk` files
with gesher-app. Apps installed with gesher-app get Start menu shortcuts of
their own ([below](#installed-apps-in-the-start-menu)).

Gesher's icon, `tools/package/gesher.ico` (a bridge, drawn by
`tools/icons/make-icons.py`), is Setup's (`SetupIconFile`), Add/Remove
Programs' (`UninstallDisplayIcon`), the Start menu entries' and that of an
`.apk` file Gesher has not installed.

## The `.apk` association

Everything is per user, under `HKCU\Software\Classes`:

| Key | Value |
|---|---|
| `.apk` (default) | `Gesher.Apk`, only when `.apk` is unclaimed (see below) |
| `.apk\OpenWithProgids` | `Gesher.Apk` (empty string) |
| `Gesher.Apk` (default) | `Android app (Gesher)` |
| `Gesher.Apk` `AppUserModelID` | `Gesher.Installer`: opened APKs are its recent items, not those of every app's jump list |
| `Gesher.Apk\DefaultIcon` (default) | `%1` (the icon handler's) |
| `Gesher.Apk\shellex\IconHandler` (default) | `{6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50}` |
| `CLSID\{6B0C3C9E-...}\InprocServer32` | `<app>\gesher_shellext.dll`, `ThreadingModel` `Apartment` |
| `Gesher.Apk\shell\open` (default) | `Run with Gesher` |
| `Gesher.Apk\shell\open\command` (default) | `"<app>\gesher-launch.exe" "%1"` |

`ChangesAssociations=yes` makes Setup and the uninstaller notify the shell
(`SHChangeNotify`), so Explorer picks up the change without a sign-out.

`gesher-launch.exe` (`compat/shell/gesher_launch.cc`) runs `gesher-app.ps1`
with the same arguments. It is a Windows-subsystem program: Explorer starting
`powershell.exe` (a console program) directly flashed a console window before
`-WindowStyle Hidden` hid it, while gesher-launch starts it with
`CREATE_NO_WINDOW`. It also calls `AllowSetForegroundWindow(ASFW_ANY)`: the
app's window belongs to a process the device's system server starts, which
Windows would otherwise keep behind Explorer.

### The splash window

A launch can take several seconds before the app's window is up (the first
one starts the device, installs and compiles the APK; every one starts the
app's process). So that something is on screen at once, gesher-launch shows a
splash window (class `GesherSplash`) right after it starts `gesher-app.ps1`:

- **When**: for `gesher-launch launch <package>`, `gesher-launch launch <file.apk>`
  and `gesher-launch <file.apk>` (a double-click) whose app's window is shown;
  not for `install`, `uninstall`, `view`, `list`, `status`, `start`,
  `shutdown`, `toast-activated` or `apk-icon`, nor with `--hidden-window`,
  `--headless`, `--single-process`, `--system-server` or
  `GESHER_APP_WINDOW=hidden|headless` (`--window` shows it again).
  `GESHER_SPLASH=0` turns it off. When the app already has its window (it
  runs), none is shown.
- **What**: the size of the app's window (`GESHER_DISPLAY`'s, else 400x800
  pixels, within the work area), centered on the monitor of the pointer, with
  the app's icon (`gesher-shell\icon.ico` of the package, else the APK's
  `apk-icons.list` entry, else `gesher.ico`), its label (`label` of
  `gesher-shell\shortcut.properties`, else the package name or the APK's file
  name) as title and text, and "Starting…". With the package known (a
  package name, or an APK `apk-icons.list` names), it carries the app's
  AppUserModelID `Gesher.App.<package>` (`PKEY_AppUserModel_ID`), so its
  taskbar button is the app's.
- **Until**: a visible top-level window (not a tool window, not a splash) with
  that AppUserModelID is up, the app's own (the app process's windows carry it);
  for an APK whose package is not known, a new window with any `Gesher.App.*`
  ID. It looks every 200 ms. It also goes when `gesher-app.ps1` ends (the
  launch failed, or the app ended without a window) and after 2 minutes. If
  it still has the foreground when the app's window is up, it hands it on to
  that window; it never takes it from the app. Closing it (its X) cancels the
  launch: gesher-launch terminates the starting processes and exits, so the app
  does not open.

CI: `tests/windows/tracks/55-shell.ps1` runs a copy of gesher-launch with a
stand-in `gesher-app.ps1` and a stand-in `%LOCALAPPDATA%` (a shortcut record
with an escaped label, an icon, an `apk-icons.list` line): `launch <package>`
and a double-clicked APK show the splash with the label, icon and
AppUserModelID, it stays while no app window is up and goes once a window with
the app's AppUserModelID is shown; closing it cancels the launch; `install` and
`--headless` show none. The package job
runs the installed `gesher-launch install uiapp.apk` (no splash) and
`gesher-launch launch com.gesher.uiapp` with its window shown: the splash,
with `Gesher UI` and `Gesher.App.com.gesher.uiapp`, is gone within 5 s of the
app's `GesherWindow` being visible.

Each `.apk`, `.apks` and `.xapk` file's icon comes from `gesher_shellext.dll`
(`compat/shell/gesher_shellext.cc`), loaded by Explorer: its app's icon. It
parses no APK: it looks the file (its size and last write time) up in
`%LOCALAPPDATA%\Gesher\data\system\apk-icons.list`, which `gesher-app install`
and launches fill with the shortcut's `.ico`. An APK not listed shows Gesher's
icon at first, while the handler runs `gesher-launch.exe apk-icon "<file>"` in
the background (no window; once per file, at most two at once, not for an empty
file or one still being written). That command draws the app's icon without
installing it into `data\system\apk-icons\<size>-<time>.ico`, records it, and
Explorer shows it. Bundles are read in a temporary directory without installing
the app: the base APK supplies package metadata and the resource splits supply
the density-specific images. The cache key remains that of the original bundle.
Temporary icon files have unique names, and complete icon lists are published by
replacement so concurrent requests do not expose partially written cache data.

Opening an APK this way runs it the way `gesher-app my.apk` does: it is
installed (or updated) on the user's Gesher device and its launcher Activity
started through the device's system server, in a window of the app's process
(ADR 0029), with the app's output in a console window (see `app/README.md` and
[0026](decisions/0026-services-persistent-device.md)). The first launch starts
the device (binderd, servicemanager, the system server) in the background; it
outlives the console and serves later launches until `gesher-app shutdown` or
10 minutes without an app. The APK must be on the same drive as Gesher.

## Installed apps in the Start menu

Each installed package with a launcher (MAIN/LAUNCHER) activity gets a Start
menu shortcut, as a launcher lists it
([0028](decisions/0028-shell-start-menu-apps.md)):

| | |
|---|---|
| Shortcut | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Gesher Apps\<label>.lnk` (FOLDERID_Programs; `<label> (<package>).lnk` if another app has that label) |
| Command | `<app>\gesher-launch.exe launch <package>`, in `<app>` (an older Gesher without it: `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "<app>\gesher-app.ps1" launch <package>`) |
| Name | the app's label in the default locale (`android:label`, else the package name) |
| Icon | `%LOCALAPPDATA%\Gesher\data\data\<package>\gesher-shell\icon.ico` |
| AppUserModelID | `Gesher.App.<package>` (`System.AppUserModel.ID` in the link) |

- **When**: `gesher-app install APK` and every launch (`gesher-app my.apk`, a
  double-clicked `.apk`) make it, or make it again when the package was
  updated (new label or icon) or the shortcut is missing; it says so in the
  console (`gesher-app: Start menu shortcut ...`). `gesher-app uninstall
  PACKAGE` removes it and its icon, and the `Gesher Apps` folder once it is
  empty. A failure never fails the install or the launch. `GESHER_SHORTCUTS=0`
  turns shortcuts off; runs from a build folder without `gesher-app.cmd` make
  none.
- **`gesher-app launch PACKAGE`** starts an installed package by name (the
  shortcut's command; the device starts if it is not running). `gesher-app
  launch APK` is `gesher-app APK`.
- **The label and icon** are read and drawn with the framework, as a
  launcher does (`com.gesher.app.shell.AppIcon`): the APK's resources for the
  default locale at xxxhdpi, so the best density of a PNG or WebP mipmap is
  picked; the application's icon, else the launcher activity's, else the
  framework's default (`sym_def_app_icon`); an adaptive icon is drawn by
  `AdaptiveIconDrawable`, its foreground over its background in the system's
  icon mask (if the framework cannot, Gesher composes the two layers in a
  circle). The `.ico` holds ten sizes, 16 to 256 pixels: 32-bit DIBs up to 48,
  PNGs above. `shortcut.properties` next to it records the shortcut.
- **The taskbar**: the system server starts the package's processes with
  `GESHER_APP_USER_MODEL_ID=Gesher.App.<package>`, and the app process
  (`android.app.GesherAppProcess`) calls `SetCurrentProcessExplicitAppUserModelID`
  with it before any window exists (`gesher-app: Windows AppUserModelID ...` in
  the app's output), so its windows group with the shortcut and get its icon.
  Window code that needs it per window reads the same variable and sets
  `PKEY_AppUserModel_ID` on the window (`SHGetPropertyStoreForWindow`).
  `--single-process` and `--system-server` runs set none.
- **Uninstalling Gesher** removes `{userprograms}\Gesher Apps` with every
  app's shortcut (`[UninstallDelete]`); the icons go with `%LOCALAPPDATA%\Gesher`.
  Nothing else of the Start menu is touched.

The natives (`com.gesher.app.shell.Shell`: `IShellLinkW`, `IPropertyStore`,
`SHGetKnownFolderPath`, `SetCurrentProcessExplicitAppUserModelID`, the URL
protocols' registry keys) are in `compat/shell`, part of libandroid_runtime's
builtin library.

## Installed apps' URL protocols (OAuth redirects)

An app that signs in with OAuth (AppAuth, as Thunderbird does) opens the sign-in
page in a browser and waits for the browser to open its redirect URL, a custom
scheme one of its activities declares (`<data android:scheme=...>` in a VIEW
filter with BROWSABLE, e.g. `net.thunderbird.android:/oauth2redirect` or
`msauth://net.thunderbird.android/...`). On Gesher the browser is Windows':

- **The browser**: apps that look for one (`queryIntentActivities` of VIEW
  `http:` BROWSABLE) find `com.gesher.browser`, a synthetic package with a full
  browser's filter (`com.gesher.server.BrowserPackageSupport`); starting it opens
  the URL with Windows' default browser (`WindowsHost.openUrl`), as any http(s)
  VIEW nothing else handles.
- **The way back**: `gesher-app install` (and launches that install or update)
  registers each custom scheme of the package's BROWSABLE VIEW filters as a URL
  protocol, per user:

  | Key (`HKCU\Software\Classes`) | Value |
  |---|---|
  | `<scheme>` (default) | `URL:<scheme> (<package>, an Android app on Gesher)` |
  | `<scheme>` `URL Protocol` | empty |
  | `<scheme>` `GesherPackage` | `<package>`: the key is Gesher's, for that package |
  | `<scheme>\DefaultIcon` (default) | the app's `.ico` (the shortcut's) |
  | `<scheme>\shell\open\command` (default) | `"<app>\gesher-launch.exe" view <package> "%1"` |

  When the browser opens such a URL (asking the user first), `gesher-app view
  <package> <url>` starts ACTION_VIEW of it (BROWSABLE) in the package through
  the device, in the app's task when it runs, where AppAuth's
  RedirectUriReceiverActivity hands it to the activity waiting below it
  (`FLAG_ACTIVITY_CLEAR_TOP | FLAG_ACTIVITY_SINGLE_TOP`: `onNewIntent`).
- **Not taking over another's scheme**: the web's, mail's, files' and Android's
  own schemes (`http`, `https`, `mailto`, `file`, `content`, `tel`, ...) are
  never registered, nor a scheme with a key of another program (in HKCU without
  `GesherPackage`, or in HKLM) or another package's; the install says so
  (`gesher-app: URL protocol <scheme>: not registered ...`).
- **Removal**: `url-protocols.properties` in the package's `gesher-shell`
  folder records its schemes; an update removes those it no longer declares,
  `gesher-app uninstall PACKAGE` removes them all (only keys whose
  `GesherPackage` still names it), and uninstalling Gesher removes every key of
  `HKCU\Software\Classes` with a `GesherPackage` value (`RemoveUrlProtocols`).
  `GESHER_SHORTCUTS=0` turns them off with the shortcuts.

CI: `tests/windows/tracks/95-browser.ps1` checks the browser package and the
delegation (`gesher.services.BrowserIntentTest`, with no browser opened); the
package job installs components.apk (CrossActivity declares
`gesher-components://callback`), reads its URL protocol, uninstalls and
reinstalls it around a key of another program's, runs a protocol command line
through `gesher-launch.exe` and, after uninstalling Gesher, checks the key is
gone.

## The Gesher device and (un)installing

The device runs `dalvikvm.exe`, `binderd.exe` and `servicemanager.exe` from the
install folder and keeps files of `%LOCALAPPDATA%\Gesher` open, so:

- the uninstaller runs `gesher-app shutdown` first (`[UninstallRun]`), then
  removes `%LOCALAPPDATA%\Gesher` with the installed packages, their data and the
  logs;
- Setup runs `gesher-app shutdown` before it replaces the files of an earlier
  version (`PrepareToInstall`).
- Explorer, and any program whose file dialog showed an APK, keeps
  `gesher_shellext.dll` loaded. Setup does not close them for it
  (`CloseApplicationsFilter=*.exe,*.chm`: closing Explorer took the desktop away
  mid-update). `PrepareToInstall` renames the loaded copy to
  `gesher_shellext.<time>.old` (a loaded DLL can be renamed, not replaced) and the
  new one is written in its place; those processes keep the old copy until they
  end. The next update deletes the `.old` copies nothing has loaded any more, and
  uninstalling deletes them too. CI loads the DLL into a process, runs Setup again
  and checks that the process is still running.

`gesher-app shutdown` needs no runtime: `gesher-app.ps1` reads the device's
state file (`%LOCALAPPDATA%\Gesher\data\system\gesher-device`), sets the system
server's shutdown event, waits for it to exit and ends whatever is left.

### Not taking over another app's association

- Gesher always joins `.apk`'s "Open with" list (`OpenWithProgids`).
- It becomes `.apk`'s default only when `HKCR\.apk` (the merged per-user and
  per-machine view) has no default ProgID, or already names `Gesher.Apk`
  (`ApkUnclaimed` in the script). An emulator or another tool that registered
  `.apk` first, for this user or machine-wide, keeps it.
- It never writes Explorer's `UserChoice`
  (`...\Explorer\FileExts\.apk\UserChoice`): a default the user picked in
  Windows' "Open with" dialog or Settings always wins, as Windows requires.
- On uninstall, `Gesher.Apk` and Gesher's `OpenWithProgids` entry are
  removed; `.apk`'s default is removed only while it still names `Gesher.Apk`
  (another app may have claimed it since), and the `.apk` key only if it is
  then empty.

APKs show their icons from Gesher's icon handler while `.apk` opens with Gesher.

### How CI checks it

After installing, the `package` job checks that `.apk`'s default is
`Gesher.Apk` and that `OpenWithProgids` lists it, reads the open command from
the registry, replaces `%1` with the installed `lifecycle.apk`, runs the
command line through `cmd.exe` as Explorer would, and expects
`gesher-app: done`. After uninstalling, it checks that the `Gesher.Apk` key,
`.apk`'s default and the `OpenWithProgids` entry are gone.

Verified in ci run 39946705097 (package job on windows-2022).

With the Gesher device (ci run 40019524773), the package job also runs the
installed `gesher-app lifecycle.apk` (the device starts, the APK is installed
and run; `gesher-app list` shows it), `--single-process`, `gesher-app
shutdown` (the device's three processes gone), the `.apk` open command (which
starts the device again) and checks that the device still runs after that
command's console is gone; after uninstalling, that its processes are gone and
`%LOCALAPPDATA%\Gesher` removed.

The Start menu shortcuts (ci run 40024847287, package job on windows-2022):
the first `gesher-app lifecycle.apk` makes `Gesher Apps\Gesher Lifecycle.lnk`
and the app process reports its AppUserModelID; `gesher-app uninstall
com.gesher.lifecycle` removes the shortcut and the icon; `gesher-app install
lifecycle.apk` makes it again, and the job reads it with `WScript.Shell`
(target `gesher-app.cmd`, arguments `launch com.gesher.lifecycle`, working
folder, icon) and its `System.AppUserModel.ID` through the link's
`IPropertyStore` (`tests/windows/shell/ShellChecks.ps1`), checks the `.ico`
(ten sizes, DIBs and PNGs, System.Drawing reads them, the xxxhdpi picture's
red at 256 pixels and transparent corners), runs the shortcut's command line
(`gesher-app: done`), installs components.apk (an adaptive icon: white
foreground, blue background, masked corners) and checks `Gesher.Apk`'s
`DefaultIcon` and the Gesher Control Center entry's icon. After uninstalling Gesher,
`Gesher Apps` is gone and a file of the job's in the Start menu is still
there. `tests/windows/tracks/55-shell.ps1` checks the label and the icon of
lifecycle.apk, components.apk and peer.apk (the framework's default icon) on
both test runners.
