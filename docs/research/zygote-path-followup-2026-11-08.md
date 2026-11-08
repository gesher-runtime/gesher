# Cross-drive Windows path conversion follow-up — 2026-11-08

## Observed failure and cause

Both main Windows jobs for published source
`6c0e1976456e6214763c7bddf58229eb3d4f3f66` stopped in the mandatory offline
identity suite at `DynamicShortcutTest`: `actual Unicode ShellLink creation
failed`. The earlier cross-process dynamic shortcut suite had passed. These
are runtime assertion failures, separate from the previous GitHub GET
transport failures.

- [Windows 2022 job 128628545889](https://github.com/gesher-runtime/gesher/actions/runs/42446512495/job/128628545889)
- [Windows 2025 job 128628545984](https://github.com/gesher-runtime/gesher/actions/runs/42446512495/job/128628545984)

The Windows 2022 log records the Java temporary root on
`C:\Users\runneradmin\AppData\Local\Temp\gms-...`, while the runtime executable
is on D:. `run-microg-security.ps1::AndroidPath` correctly represents that
directory as `/mnt/c/Users/...`. Native
`compat/posix/src/path.cc::DrivePath` maps it to C:, so
`Files.createTempDirectory` creates a real directory there.

`Zygote.windowsPath`, used when passing the same directory to the Windows
shell, previously changed separators and added the executable's drive. The
result was `D:\mnt\c\Users\...`. That is a different directory, so the
real `IPersistFile::Save` call could not save the link in the directory Java
had created. The old assertion did not print its HRESULT or converted path;
the diagnostic now does, while retaining both success and file-existence
requirements.

This defect also affects other callers of the shared conversion, such as
app process log destinations and icon paths on another drive. No native COM
implementation or fixture temporary-directory placement is changed.

## Repair

`Zygote.windowsPath` now recognizes an ASCII drive letter under `/mnt/`,
followed by a separator or the end of the path, before applying the ordinary
drive-less path conversion. `/mnt/c` and `/mnt/c/` both become `C:\`.
Mounted drive letters are case-insensitive; path contents retain their
UTF-16 characters. Names such as `/mnt/sdcard`, `/mnt/cc`, `/mnt/1` and
`/mnt/c-other` remain ordinary paths. Mount recognition precedes separator
conversion, so a backslash inside the `/mnt/` prefix or immediately after the
drive letter cannot promote an ordinary path to another drive.

The mapping follows the existing native `DrivePath` contract and restores
the inverse of `Zygote.fromWindowsPath` for another drive. Primary repository
contracts are [ADR 0045](../decisions/0045-posix-paths-tz-procfs.md),
[ADR 0010](../decisions/0010-paths.md), and the production
`compat/posix/src/path.cc` implementation. This repair does not claim new UNC,
long-path, or general Windows path canonicalization behavior.

## Verification

`tools/test_zygote_paths.py` compiles the actual production `Zygote.java` and
`VmCommandLine.java` with JDK 21. The only host JNI boundary returns the
Windows runtime executable path on D:. All **23 assertions pass**, covering
the exact CI directory shape, Unicode and supplementary characters, drive
roots, upper/lowercase drives, prefix-boundary denials, ordinary and relative
paths, and forward/reverse conversion on the runtime and another drive.

With `JDK_HOME` pointing to a JDK 21 installation and a C compiler on `PATH`:

```sh
python3 tools/test_zygote_paths.py --java-home "$JDK_HOME"
```

The same fixture compiled against published `6c0e197...` fails its first
assertion with expected `C:\Users\...` and actual `D:\mnt\c\Users\...`:

```sh
python3 tools/test_zygote_paths.py --java-home "$JDK_HOME" \
  --source-ref 6c0e1976456e6214763c7bddf58229eb3d4f3f66
```

`DynamicShortcutTest` invokes the same path assertions on actual Windows ART
before its existing real Unicode ShellLink creation, existence and deletion
checks. No assertion is removed, skipped or replaced with a simulated shell
result. Full Windows rerun is required to establish the final COM/runtime
outcome; the host fixture proves only the production Java path conversion.
No historical source certificate or acceptance predicate is modified.
