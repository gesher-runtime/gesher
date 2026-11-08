# UNC paths through Java and native file APIs — 2026-11-08

## Concrete failure

The previous native `LinuxPath` and Java `Zygote.fromWindowsPath` returned a
Windows share as `//server/share/path`. Android's pinned `UnixFileSystem` and
`UnixPath` both remove repeated slashes, including at the beginning. Passing that
result to `File` or `Paths.get` changed it to `/server/share/path`: a rooted path
on the current Windows drive rather than the network share. A valid network
temporary directory therefore could not safely be exposed to Java in this form.

Sources:

- [Android 16 UnixFileSystem normalization](https://android.googlesource.com/platform/libcore/+/refs/tags/android-16.0.0_r4/ojluni/src/main/java/java/io/UnixFileSystem.java)
- [Android 16 UnixPath normalization](https://android.googlesource.com/platform/libcore/+/refs/tags/android-16.0.0_r4/ojluni/src/main/java/sun/nio/fs/UnixPath.java)
- [Microsoft: file, UNC, extended and device namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)

## Implemented path identity

UNC file roots now have a single-leading-slash guest namespace:

| Native Windows file path | Guest path |
| --- | --- |
| `\\server\share\folder\file` | `/mnt/unc/server/share/folder/file` |
| `\\?\UNC\server\share\folder\file` | `/mnt/unc/server/share/folder/file` |
| `\\localhost\C$\Temp\file` | `/mnt/unc/localhost/C$/Temp/file` |

The portable production helper in `compat/posix/include/gesher/unc_paths.h`
handles both directions. Native `MapPath` opens the guest name as its actual UNC
path. `LinuxPath` emits the stable guest form for native file identities, so
existing callers such as `realpath`, `getcwd`, directory-FD resolution and
native link targets can return a path Java will preserve. Configured system-font
roots use the same mapping; a UNC `ANDROID_ROOT` does not become a local
`\mnt\unc\...` directory. The temporary-directory entry adapter also uses this
shared helper rather than maintaining a separate UNC grammar.

`Zygote` implements the same root rules for file-picker/launcher conversions. It
recognizes raw UNC input before constructing `File`, and maps extended UNC roots
without treating device prefixes as share names. Existing own-drive and
`/mnt/<drive>` behavior remains covered by the original 23 Java assertions.

Server/share text, case, spaces and Unicode are retained. Both components must
be present and cannot be `.` or `..`, contain control characters, or contain
Win32 separator/namespace characters such as `\`, `:`, `?` and `*`. The exact
`/mnt/unc` namespace is reserved; lookalikes such as `/mnt/uncx` remain ordinary
paths. Native malformed entries use the existing unavailable-path result:
lookups fail with `ENOENT`, and `open` with `O_CREAT` fails with `EACCES`.
They do not fall through to a current-drive file. Java conversion methods reject
malformed UNC and unsupported device-namespace input with
`IllegalArgumentException`.

Raw `//server/share` and backslash UNC native inputs remain supported. Native
handling of already unsupported device/NT namespaces is unchanged; they are not
converted into the new share namespace. This work does not add arbitrary NT
device access, network discovery, credentials or a synthetic directory listing
for `/mnt/unc`. Windows continues to decide share existence, access and file
operation results. Extended-path-only filename semantics are not a new claimed
capability.

## Executed checks

The production helper's host check is reproducible with:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -idirafter compat/posix/include \
  tests/paths/unc_paths_test.cc -o /tmp/unc-paths-test
/tmp/unc-paths-test
```

**82 assertions passed.** These check actual helper code for both conversion
directions, ordinary/extended roots, Unicode, long strings, unrelated paths,
malformed root components and device namespace exclusions. They are string
mapping evidence, not SMB or Windows filesystem execution.

```sh
python3 tools/test_zygote_paths.py --java-home /path/to/jdk21
```

**49 Java host assertions passed**, including all previous 23 and 26 additional
UNC/namespace checks. The existing native executable-path adapter remains the
only adapted boundary. The same command with `--source-ref 8af8d702` fails at
the first new UNC assertion because the old production code returns the unstable
double-slash form. `DocumentPickerTest` also asserts the new canonical name and
preservation through both `File` and `Path`, retaining its existing runner
markers.

The complete production `path.cc` compiled with strict warnings as errors for
Windows x64 and ARM64. Updated `Zygote` and all three affected Java fixtures
compiled against the verified Android framework and core module.

## Actual Windows fixture

`gesher.services.UncFileContractTest <native UNC share root>` requires a real,
private writable SMB share supplied by the Windows runner, for example
`\\localhost\GesherUncTests$` without a trailing separator. It creates only a
fresh child directory and tests Java `File`/NIO writes, reads, Unicode names,
directory enumeration, rename, canonical paths and native `realpath`. It reads
the same bytes through raw backslash UNC, raw slash UNC and extended UNC native
paths, checks malformed-namespace errors, and removes its owned files. Its
success marker is `UncFileContractTest all passed:`.

No SMB share was available in the local Linux environment. The fixture compiled
but has not executed locally; passing host mapping checks does not establish
network-share access, credentials, Windows path canonicalization or SMB I/O.
Those require the mandatory Windows fixture and the separate fresh-VM temporary
directory check using an actual UNC temporary directory.
