# The Windows installer

`tools/package/gesher.iss` (Inno Setup 6) is built and tested by the
`package` job in `.github/workflows/ci.yml`. It installs per user, without
administrator rights, into `%LOCALAPPDATA%\Programs\Gesher`, puts `gesher`
and `gesher-app` on the user's `PATH`, adds Start menu entries, and
associates `.apk` files with gesher-app.

## The `.apk` association

Everything is per user, under `HKCU\Software\Classes`:

| Key | Value |
|---|---|
| `.apk` (default) | `Gesher.Apk`, only when `.apk` is unclaimed (see below) |
| `.apk\OpenWithProgids` | `Gesher.Apk` (empty string) |
| `Gesher.Apk` (default) | `Android app (Gesher)` |
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

There is no `DefaultIcon`: the installer ships no icon yet, so APKs show
Windows' generic icon.

### How CI checks it

After installing, the `package` job checks that `.apk`'s default is
`Gesher.Apk` and that `OpenWithProgids` lists it, reads the open command from
the registry, replaces `%1` with the installed `lifecycle.apk`, runs the
command line through `cmd.exe` as Explorer would, and expects
`gesher-app: done`. After uninstalling, it checks that the `Gesher.Apk` key,
`.apk`'s default and the `OpenWithProgids` entry are gone.

Verified in ci run 39946705097 (package job on windows-2022).
