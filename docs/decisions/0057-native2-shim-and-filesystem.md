# 0057. native2: the shim's gaps, the platform's libraries by dlopen, flock and links

Status: accepted (native2 track, 2026-10-26)

## Context

The native-libraries track (0014, 0017, 0020, 0023, 0032) built the bionic
shim from what its test libraries and the first apps imported. The arm64
track (0047) then met real arm64 libraries (TIC-80's) importing what the shim
lacked, and gave its translator DLL ~50 guest-only fallbacks (sem_*, the
32-bit `wchar_t` functions, libm extras), so x86-64 libraries importing the
same names still bound to aborting stubs. `dlopen("libEGL.so")` (SDL,
engines, audio libraries) failed for x86-64: the shim's `dlopen` only loaded
ELF files; the translator answered it with pseudo handles for the guest only.
The posix track (0045) left: `stat` without execute bits while
`access(X_OK)` holds; ART's `DexFileLoader` not resolving symbolic links and
`LockedFile` (ScopedFlock) unsupported under `_WIN32`; inotify's `IN_MODIFY`
only when NTFS updates the directory entry, often at close.

## Decision

### The shim grows by gap-filling files registered first
`extra_bionic.cc`, `extra_sys_bionic.cc` and `zlib_bionic.cc` register before
every other part of the shim (`RegisterExtra`, first in `Table()`): a name the
core files define keeps their definition, so these files only fill gaps and
other tracks' edits to the core files never conflict with them. The arm64
translator's fallbacks moved here; `guest_libc.cc` keeps only what is
arm64-specific (struct layouts, syscall numbers, setjmp, `getauxval`, and now
the floating-point environment, whose `fenv_t` and `FE_*` values differ and
whose state is the guest's FPCR/FPSR).

### libz.so is the zlib Gesher builds, behind shadow streams
`uLong` is 64-bit in the app and 32-bit in the host build, so `z_stream`
differs. Each app stream gets a host `z_stream` its `state` field (zlib's
internal pointer, opaque to callers) points at; the public fields are copied
in and out around each call, and the app's System V `zalloc`/`zfree` are
called through Microsoft-ABI trampolines. `gzopen` opens with gesher-base's
`open` (Linux paths) and `gzdopen`s the fd. Rejected: a second zlib built with
`long` redefined (fragile, and the allocator callbacks would still be the
wrong ABI).

### System properties come from the runtime
`__system_property_*` read what gesher-app loaded from build.prop (libandroid
installs android::base's accessors in dalvikvm); elsewhere the shim reads
`$ANDROID_ROOT/build.prop` itself. Before, every property read as "", so
`android_get_device_api_level()` said 0 and libraries took pre-Lollipop paths.

### dlopen of the platform's libraries
A soname the shim implements (`libc.so`, `libm.so`, `libdl.so`, `liblog.so`,
`libz.so`, `libandroid.so`, `libnativewindow.so`, `libEGL.so`,
`libGLESv1_CM/v2/v3.so`, `libOpenSLES.so`, `libaaudio.so`, and the others
whose probe symbol the process has) opens by name or by a system path
(`/system/lib64/...`, `/vendor/`, `/apex/`, ...) as a handle of its own; `dlsym`
on it, on `RTLD_DEFAULT`, or on an app library's handle falls back to the
shim's table, as bionic's search reaches libc through a library's
dependencies. A bare name not loaded yet is searched in the directories app
libraries were loaded from (the JNI bridge notes them) and `$LD_LIBRARY_PATH`,
as the app's linker namespace does. A library the process does not provide
(`libvulkan.so`) fails to open, so apps take their fallback.

### flock over LockFileEx, on a byte beyond the data
Windows byte-range locks are mandatory: a lock over the file's data would
block other handles' reads and writes. `flock` locks one byte at offset
2^63 - 2 instead, which only other locks see. Windows keeps a lock per open
file object, as Linux keeps flock's per open file description (`dup`s share
it; it goes with the last handle); converting a lock drops it first, as Linux
may. ART's `LockedFile` uses it (patch 0571), so ScopedFlockTest runs.

### stat's execute bits
Regular files show `0755` (`0555` read-only), as an NTFS volume mounted on
Linux does, consistent with `access(X_OK)`, which holds for every file (apps
check `File.canExecute()` before starting their programs).

### Symbolic links in dex locations
`DexFileLoader::GetDexCanonicalLocation` resolves a location through
`realpath` only when one of its components is a symbolic link (patch 0570):
realpath on Windows also normalizes name case and 8.3 short names, which must
not move ordinary dex locations (oat and boot image matching).

### IN_MODIFY at the write
gesher-base's `write`/`pwrite` tell this process's inotify instances about a
write to a watched file or directory at once (only while an instance exists);
NTFS's later directory-entry report of the same change is another
`IN_MODIFY`, as a second write would be on Linux. Writes from other processes
are still reported when NTFS updates the entry.

## Consequences

- Verified at d380a3e (ci 40296960603): every job success/skipped, both
  Windows summaries zero failures, package and e2e pass. Both ABIs exercise
  dalvikvm, APKs in both modes, real executable tests and the import gate.
  The public NDK API coverage figure is broader than that gate and still
  reports missing functions. native3 (ADR 0062 reserved) owns those gaps,
  missing platform handles, fcntl record locks, cross-process inotify and
  the remaining ABI/filesystem limits; see its brief.
- x86-64 and arm64 libraries see the same libc; the translator DLL no longer
  carries libc code beyond the ABI.
- The shim has a zlib, so `-lz` libraries (libpng, freetype, engines) run.
- `fcntl` record locks are still always granted (not modelled), independent
  of `flock`'s.
- The test programs of zlib, libpng, libjpeg-turbo, SQLite, OpenSSL and a
  libc++ test run in CI through gesher-exec for both ABIs, and a symbol check
  lists every import of them and of the e2e apps' libraries the shim lacks
  (tests/windows/tracks/44-native2.ps1).
