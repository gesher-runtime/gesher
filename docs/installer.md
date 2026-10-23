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

Opening an APK this way runs it the way `gesher-app my.apk` does: headless,
in a console window, to the launcher Activity's `onResume` (see
`app/README.md`). The APK must be on the same drive as Gesher.

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
