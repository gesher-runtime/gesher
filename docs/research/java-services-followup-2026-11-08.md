# Storage and user service follow-up, 2026-11-08

This follow-up starts from PR #8 head
`3843241968edfd124d3b9596c0e6381f45f5c381` and compares the actual framework
callers with Android 16 r4. It implements a bounded view of the runtime's
existing filesystem and owner user. It does not provide an Android volume
daemon, encryption, removable-storage management, cache reclamation or OBB
mounting.

## Implemented behavior and reproduced failure

The original `IStorageManager.Default.getVolumes()` result was null. The
real public framework `StorageManager.getUuidForPath()` iterates that result
for a path outside the internal data directory. With the original production
service and actual framework class, the host diagnostic reproduced:

```text
getVolumes = null
java.lang.NullPointerException: Cannot read the array length because "<local3>" is null
    at android.os.storage.StorageManager.getUuidForPath(StorageManager.java:953)
```

The repaired service exposes the private data directory and its emulated
shared-storage view. The framework now resolves paths on that backing storage
and produces the documented `IOException` for the unmanaged test path. The
volume inventory also supplies `findVolumeById`, `findPathForUuid`,
private/emulated association and writable-private-volume queries.

| Service methods | Implemented source of truth |
| --- | --- |
| `getVolumes`, `getVolumeList` | The actual `ANDROID_DATA` directory and its `media/0` shared view. Internal and shared storage refer to one host filesystem. The shared `VolumeInfo` path is `media`, because `getPathForUser(0)` appends the user ID; the corresponding `StorageVolume` contains `media/0`. |
| `getDisks`, `getVolumeRecords` | Empty inventories: Gesher has no Android-managed physical/adoptable disks and no remembered removable-volume records. These arrays describe that absence; they do not stand in for unimplemented physical devices. |
| `isCeStorageUnlocked` | The existing desktop owner session is already unlocked. Only user 0 exists. This agrees with the existing user service and does not claim Android file-based encryption. |
| `getAllocatableBytes` | Actual usable host bytes, minus the low/full-storage reserve. Current numeric global settings are read from the existing settings provider, with Android's default 5%/500 MiB low threshold and 1 MiB full threshold. Only the default internal volume is managed. |
| `allocateBytes(UUID, ...)` service operation | Checks the requested capacity against that actual space; negative sizes, missing/read-only backing storage, unknown volumes and impossible requests do not silently succeed. It does not promise cache reclamation. |
| `getUsers`, `getUserName`, `getUserSerialNumber`, `getUserHandle` | The existing owner record and its stable serial 0. Unknown users/serials produce -1, which the public framework turns into null where specified. |
| `getUserRestrictions` | A fresh empty restriction bundle for the currently unmanaged user, consistent with the existing application-restriction model. |

Volume state is derived from directory existence and read/write access.
Missing or obstructed paths are unmounted; read-only directories are reported
read-only. Failed creation of the shared directory cannot produce a mounted
result. A query does not recreate a missing `ANDROID_DATA` root. Independent
arrays and volume objects prevent a local caller from changing later query
results. Other user IDs do not acquire shared storage.

The primary emulated `StorageVolume` now carries `UUID_DEFAULT`, while its
filesystem UUID string remains null, as for internal emulated storage. It is
not described as an unadopted portable disk.

The original user service also returned null from `getUserRestrictions`,
null from `getUserName` and `getUsers`, and the valid owner ID/serial 0 for
unknown serial/user requests through inherited defaults. The new wrapper
fixture fails against the old service at the first newly tested restriction
bundle query. The repair returns the actual single-user model and does not
invent additional profiles or policy restrictions.

`SettingsProvider.sInstance` is now published after its tables and provider
metadata have been initialized. This lets the new server-local numeric reader
use a fully initialized provider and preserves synchronized access to the
existing in-memory settings state.

## Allocation and unlock details

Android distinguishes its UUID overload from its file-descriptor overload.
The UUID operation makes capacity available; it does not allocate a named
file or reserve bytes against other processes. The framework's descriptor
overload calls `Os.posix_fallocate` after requesting enough capacity and falls
back to `ftruncate` only for ENOSYS/ENOTSUP. The new Windows fixture exercises
that existing real framework path, checks real file growth, and verifies that
the original contents and zero-filled extended tail survive.
Gesher's current native path grows the logical file; it does not establish
sparse/range physical preallocation or parity under concurrent file resizing.
The fixture's growth/content checks must not be read as proof of those
unimplemented guarantees.

The current service offers only free bytes already available on the backing
filesystem. It does not add an estimate for unimplemented cache reclamation;
`FLAG_ALLOCATE_CACHE_ONLY` therefore contributes zero. Aggressive allocation
requires the runtime's existing package/caller permission policy and uses the
full-storage reserve. Android's cache-eviction policy remains separate work.

`StrictMode` independently queries `IStorageManager.isCeStorageUnlocked` while
checking access to credential-protected paths. The former inherited false
answer contradicted the already unlocked owner in `IUserManager`; an app that
enables that detector and a fatal penalty can terminate on this mismatch.
The Windows fixture checks the real private detector query and public storage
wrapper. It does not enable a fatal penalty or claim that an Android key was
created or unlocked.

## Regression evidence and limits

The modified production classes and fixtures compile with the verified
framework/core inputs documented in the
[main audit](android-crash-contracts-2026-11-08.md), using JDK 21 with the actual
Android core module as `java.base` for compilation.

- `StorageQueryContractTest` is a Windows ART fixture using the production
  service, generated `IStorageManager` stub/proxy, Parcel transport and the real
  `StorageManager`. It also tests absent and obstructed backing directories in
  a disposable private directory, unlock-state consistency, and native FD
  allocation. It never replaces or removes the running device's data root.
- `UserManagerProfilesTest` now has **49 assertions** through the production
  user service and real `UserManager` wrappers.
- A supplemental Linux host diagnostic ran **45 storage assertions** and all
  **49 user assertions** successfully. The storage count includes eight
  backing-path state assertions; one healthy-capacity assertion is conditional
  on the observed free space. The full Windows fixture has additional
  Binder/StrictMode/native-allocation coverage.
- The original service was selected ahead of the modified classes as a
  negative control. It reproduced the public storage NPE above and failed the
  new user test with `public user restrictions query returned null instead of
  an empty Bundle`.
- `git diff --check` passed for the combined working tree at this point.

The host diagnostic executes actual service and framework Java classes. A
small temporary JNI envelope supplies logging, monotonic time, basic build
properties and primitive-array creation needed for framework initialization.
Its storage-manager instance bypasses the Android constructor and receives
the actual service directly. Thus these results establish Java behavior and
real host filesystem queries, **not** ART, Binder/Parcel, Windows filesystem
semantics, encryption, or an installed third-party APK result. The Windows
fixture was compiled locally; Windows execution is left to the normal CI
runner. The temporary host envelope is not part of the shipped runtime.

## Backend work still missing

OBB mount/unmount has no actual image-mount backend. This follow-up does not
claim that error callbacks implement OBB support. Android cache quotas,
per-UID cache accounting, package-manager cache deletion and ordered
free-storage reclamation are also not implemented here. Some other user
service methods, including the permission-filtered user-properties family,
still inherit defaults. These remain concrete review/implementation targets;
the repaired methods do not establish that all Android APIs are implemented.

## Pinned references

All references below are from `android-16.0.0_r4`:

- [StorageManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/os/storage/StorageManager.java): `getUuidForPath`, array-consuming inventory helpers, storage thresholds, and both allocation overloads.
- [StorageManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/StorageManagerService.java): internal-volume creation, inventory, allocation permission checks and capacity calculation.
- [VolumeInfo](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/os/storage/VolumeInfo.java) and [StorageVolume](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/os/storage/StorageVolume.java): emulated paths, mounted states and UUID meaning.
- [StrictMode](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/os/StrictMode.java): the independent CE-unlock query in the credential-protected-path detector.
- [UserManager](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/os/UserManager.java) and [UserManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/UserManagerService.java): user inventories, serial mappings, non-null name/restriction values and defensive copies.
