# The Windows installer

`tools/package/gesher.iss` (Inno Setup 6) is built and tested by the
`package` job in `.github/workflows/ci.yml`. It installs per user, without
administrator rights, into `%LOCALAPPDATA%\Programs\Gesher`, puts `gesher`
and `gesher-app` on the user's `PATH`, adds Start menu entries, and
associates `.apk` files with gesher-app. Apps installed with gesher-app get
Start menu shortcuts of their own ([below](#installed-apps-in-the-start-menu)).

Gesher's icon, `tools/package/gesher.ico` (a bridge, drawn by
`tools/icons/make-icons.py`), is Setup's (`SetupIconFile`), Add/Remove
Programs' (`UninstallDisplayIcon`), the Start menu entries' and the `.apk`
type's.

## The `.apk` association

Everything is per user, under `HKCU\Software\Classes`:

| Key | Value |
|---|---|
| `.apk` (default) | `Gesher.Apk`, only when `.apk` is unclaimed (see below) |
| `.apk\OpenWithProgids` | `Gesher.Apk` (empty string) |
| `Gesher.Apk` (default) | `Android app (Gesher)` |
| `Gesher.Apk\DefaultIcon` (default) | `<app>\gesher.ico,0` |
| `Gesher.Apk\shell\open` (default) | `Run with Gesher` |
| `Gesher.Apk\shell\open\command` (default) | `"<app>\gesher-app.cmd" "%1"` |

`ChangesAssociations=yes` makes Setup and the uninstaller notify the shell
(`SHChangeNotify`), so Explorer picks up the change without a sign-out.

Opening an APK this way runs it the way `gesher-app my.apk` does: it is
installed (or updated) on the user's Gesher device and its launcher Activity
started through the device's system server, headless, with the app's output in
a console window (see `app/README.md` and
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
| Command | `"<app>\gesher-app.cmd" launch <package>`, in `<app>` |
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
`SHGetKnownFolderPath`, `SetCurrentProcessExplicitAppUserModelID`) are in
`compat/shell`, part of libandroid_runtime's builtin library.

## The Gesher device and (un)installing

The device runs `dalvikvm.exe`, `binderd.exe` and `servicemanager.exe` from the
install folder and keeps files of `%LOCALAPPDATA%\Gesher` open, so:

- the uninstaller runs `gesher-app shutdown` first (`[UninstallRun]`), then
  removes `%LOCALAPPDATA%\Gesher` with the installed packages, their data and the
  logs;
- Setup runs `gesher-app shutdown` before it replaces the files of an earlier
  version (`PrepareToInstall`).

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

APKs show Gesher's icon (`DefaultIcon`) while `.apk` opens with Gesher.

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
`DefaultIcon` and the Gesher Hello entry's icon. After uninstalling Gesher,
`Gesher Apps` is gone and a file of the job's in the Start menu is still
there. `tests/windows/tracks/55-shell.ps1` checks the label and the icon of
lifecycle.apk, components.apk and peer.apk (the framework's default icon) on
both test runners.
