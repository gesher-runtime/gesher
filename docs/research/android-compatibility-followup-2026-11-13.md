# Android compatibility follow-up: map, measured gaps and Windows repairs

This pass starts from `2bc86c0ae067b43a74bf76c5010a52b4564e81e5` and the pinned
`android-16.0.0_r4` platform. Three investigation agents, three implementation
agents and separate framework/media/native QA reviewers examined the selected
contracts. An additional reviewer checked shared CI integration. Commits and the
PR use `palmoni5`.

The [machine-readable inventory](android-compatibility-followup-2026-11-13.json)
lists every remaining absent SDK boot class and absent public/protected member
on a present class, all 21 restored members, input hashes and review-queue counts.
These measure availability, not crashes. A scan or a passing regression suite
cannot establish that every Android APK runs correctly.

## Existing PRs and scope

PR [#11](https://github.com/gesher-runtime/gesher/pull/11) owns the earlier
PendingIntent, observer, scheduler, native wait/key/DNS/clock, finalizer,
statvfs/sendfile/stdio, MediaStore/connectivity and other repairs. Its refreshed
head was `8d52fdca8241447ac4d0a83795a450aa92c45391`. PR
[#10](https://github.com/gesher-runtime/gesher/pull/10), refreshed at
`165bcbcd7489a247d1f86a9e941121c17b8a9a1c`, owns account/GMS, NFC and domain
verification work and includes #11 at that snapshot. This branch does not copy
their feature implementations. Shared source/workflow files receive separate
changes; merging must retain both sets of tests.

The long-path change here repairs the runtime's relative-path conversion.
PR #11's newer statvfs test constructs its fixture with absolute paths. Those are
different contracts. Timestamps were explicitly left open by the earlier audit.

## Repository map

The baseline contains **2,743 tracked files**: `compat` 507, `app` 305, `tests`
1,041, `tools` 290, `docs` 315, `.github` 83, `probes` 83, `patches` 73, `cmake`
33 and other/root files 13. These are navigation counts, not dynamic coverage.

| Layer | Main paths | Boundary and review focus |
| --- | --- | --- |
| Platform/build | `aosp.lock`, `patches`, `cmake/aosp`, `tools/framework`, `tools/gen` | Selected AOSP sources, generated Binder defaults, descriptors, native registration and provenance. |
| App and services | `app/java/com/gesher/{app,server}` | Package parsing, launcher/process lifecycle, identity, permissions, callbacks and retained registrations. |
| Framework substitutes | `app/java/android` | Public SDK linkage, value semantics and explicit unavailable backends; Bucket and NetworkRequest repairs. |
| Filesystem | `compat/posix/src/{path,fsops,links,stat,dirent}.cc` | UTF-8/UTF-16, drives/UNC, long paths, reparse points, handles and timestamps. |
| Memory/descriptors/locks | `compat/posix/src/{fd,mman,record_lock_*}.cc` | Windows sections, descriptor generations, duplicate ownership and locks; existing supplier/source policies preserved. |
| Network/waits | `compat/posix/src/{socket,winsock,netdb,select,epoll}.cc` | Winsock, logical fds, readiness, deadlines and DNS. Host limitations are distinct from #11's guest fixes. |
| Bionic/guest ABI | `compat/bionic` | LP64/LLP64 values, libc imports, stdio/pthreads, JNI/NDK callbacks and filesystem routing. |
| ELF/TLS/ARM64 | `compat/elf`, `compat/arm64` | Relocations, TLS and guest-host calls. x64-hosted ARM translation does not prove native Windows ARM64 execution. |
| Framework JNI | `compat/framework`, `compat/bionic/jni*` | Registration, shared memory, LocalSocket and Java/native ownership. A name in a table is not behavior proof. |
| Rendering/desktop | `compat/hwui`, `compat/opengl`, `compat/windows` | Skia/HWUI, ANGLE, Win32 windows, buffers, clipboard and notifications. |
| Media | `compat/media/{player,jni,audio,video,encode,mux,ndk}` | FD/WinHTTP sources, AOSP extractors, software/Media Foundation codecs and WASAPI. |
| Hardware | `compat/devices`, `compat/camera` | WinRT location/sensors and Media Foundation cameras; deterministic fixtures do not prove all physical-device behavior. |
| Input/text | `compat/input` | Windows key/touch/pen input, Android channels and TSF/IME; full hardware parity remains separate. |
| WebView/shell | `compat/webview`, `compat/shell`, app desktop code | WebView2 and host integration; not Chromium Android equivalence. Upload-path fixes belong to #11. |
| Verification | `tests`, `probes`, `.github/workflows`, `tools/ci` | Linux references, declared host adapters, SDK callers, Windows ART/native processes, both guest ABIs and artifact identity. |

The native mapping indexed 51 POSIX implementation files (17,238 lines) and 25
bionic implementation files (16,767 lines), then followed selected public entry
points through their host calls, ownership and error paths.

## Measured API and service surface

The official input is SDK36 `platform-36_r02.zip`. CI checks its archive and
extracted `android.jar` SHA-256 values; both are recorded in the JSON. The local
comparison reused verified core/framework artifacts from run `43302471272` and
**separately rebuilt** the baseline and candidate app DEX from their own sources.
No previously built app implementation was substituted.

| Measurement | Rebuilt baseline | Rebuilt candidate |
| --- | ---: | ---: |
| SDK class definitions | 6,227 | 6,227 |
| SDK classes present on selected boot path | 5,303 | 5,303 |
| SDK classes absent from selected boot path | 924 | 924 |
| Public/protected SDK declarations compared | 90,630 | 90,630 |
| Exact descriptor without reported modifier mismatch | 82,055 | 82,076 |
| Absent declaration on a present class | 557 | 536 |
| Declaration whose class is absent | 7,516 | 7,516 |
| Potential modifier mismatch rows | 502 | 502 |
| Present descriptors referencing absent boot types | 9 | 9 |

All 21 changed rows are sixteen Bucket constants and five getters. The other
repairs correct behavior behind existing signatures. A default Bucket UID now
matches AOSP's zero initialization. Network accounting remains unavailable;
this patch does not invent traffic measurements.

The 924 absent classes comprise connectivity modules (266), other Android
framework families (212), Health Connect (207), AdServices (154), Bluetooth (41),
NFC (33), SDK sandbox (9) and AndroidX annotations (2). The boot scan does not
rule out external/module providers. PR #10's NFC additions are outside this
branch's baseline; its future counts must not be mixed with these.

The nine dependency rows reference `GenericDocument`, `Session2Token` or
`IkeTunnelConnectionParams`. The 502 modifier flags are a review queue: 498 are
method-final differences in already-final classes, and four are class-final
restrictions on classes without accessible constructors. They are not 502
demonstrated failures. Eight duplicate boot definitions are resolved in boot
path order.

A fresh baseline service/JNI source inventory found 69 service classes, 66
resolved interfaces and 1,048 explicit methods: 864 implemented but unverified
by this scan, 12 empty-collection answers, 80 default answers, 78 no-ops and 14
explicit unsupported paths. It recorded 2,768 name-based inherited-default
candidates, 50 direct registrations and 198 Context key candidates. Of 174
upstream JNI registration rows, 50 have direct local references, four alternate
references and 120 no direct reference; 523 stand-in methods were recorded.
Alternate registration/build selection must be checked before calling any of
those 120 rows a missing runtime binding.

The documentation index examined 282 selected documents, 2,757,438 bytes and
475 candidate sections, with zero parser errors. Those sections are review
leads, not 475 newly demonstrated defects.

## Repaired contracts

| Contract | Previous failure | Repair and regression evidence |
| --- | --- | --- |
| Bucket linkage | SDK callers reached missing fields/getters; empty UID differed. | Complete constants/getters; 61 assertions with separately SDK-compiled calls, reflective constants and inheritance. Old code fails with `NoSuchMethodError`. |
| NetworkRequest reset | Clear retained transport, specifier, bandwidth, signal and other filters. | Existing `clearAll`; 15 assertions for reset, matching, reuse and prior snapshot isolation. |
| Partial shortcuts | Label/rank-only updates used creation validation and replaced stored records. | Validate supplied fields, sanitize supplied intents, merge with real `ShortcutInfo.copyNonNullFieldsFrom`; 45 assertions plus the actual durable Windows path. |
| Location registrations | A second provider removed the first; invalid replacement could discard existing state. | Provider/client/kind keys, validation before mutation, link before publication, exact-record retirement and balanced unlink; 47 assertions plus real ART Binder/public-client coverage. |
| Timestamp APIs | Nofollow changed targets; flags/empty paths were lost; wide guest nanoseconds became valid after narrowing; arithmetic overflowed. | Linux validation order, OMIT/NOW, borrowed-handle ownership, directory reopen flags and checked FILETIME conversion; 406 sanitizer assertions across seven groups. |
| Long relative paths | Input appeared short before the working-directory prefix exceeded Windows limits. | Resolve absolute length before choosing extended syntax; preserve absolute and relative deep-path fixtures. |
| Guest filesystem | CRT chmod bypassed Unicode/mapped paths; remove misclassified directory links; rename allowed cross-volume copying. | Shared host routing with flags and descriptor identity; same real fixture on Linux, Windows host and both Android ABIs. |
| Cross-volume directory rename | Shared rename could remove an empty destination before failing with EXDEV. | Check opened source/target volumes before removal; 32 focused assertions and real directory identity/content preservation checks. |
| FD media ranges | Signed range addition overflowed; unsigned requests narrowed before clamping. | Subtraction-based bounds, preserved internal EOF sentinel and checked physical offset; 29 real-file and 16 syscall-boundary assertions. |
| HTTP position | Later positive range receiving 200 was treated as bytes at the requested offset. | Require 206 for positive base, preserve initial whole-resource fallback and earlier partial progress. Actual WinHTTP uses an owned loopback peer. |
| HTTP ownership | Raw chunk pointer escaped the lock and could outlive LRU eviction. | Shared immutable chunk owner through lookup/copy/eviction; whole-source adapter, concurrent reads and deterministic retention controls. |
| Surface timestamps | Zero meant auto; explicit values reset; Android's auto sentinel could reach output. | Use the actual sentinel and persistent explicit state; 20 host assertions and an actual QueueSurface/native-window executable. |
| Codec output window | Rendering borrowed a window after releasing its owner's lock. | Acquire under that lock and scoped release after rendering; 31 exact-method assertions with declared JNI/refcount boundaries. |

All six Java old-source controls and all five media old-source groups compile
the original production implementation against the same harness and fail at the
intended contract. All seven timestamp/path groups fail on the original source.
The directory guard control fails eight of its 32 checks. Lifetime controls
observe ownership without requiring a destructive memory-corruption crash.

The real Linux filesystem fixture passes 124 checks, including symlinks,
directory fds, Unicode, long relative paths and cross-filesystem file/directory
preservation. It uses raw Linux `utimensat` where glibc adds a different null-path
wrapper policy, matching bionic's thin wrapper.

Independent QA additionally corrected shortcut shared-memory initialization,
a graphics CMake helper that excluded the test executable, and atomic HTTP peer
readiness publication. The full app and services-test Java/D8 builds passed.
Inventory parser tests (31), the selected TCP source policy and 150 coherent
context negative cases passed without weakening existing gates.

## Windows and CI gate

`tests/windows/tracks/95-compatibility-followup.ps1` runs in the full and dedicated
framework matrices on Windows 2022 and 2025. It exercises actual ART services,
host POSIX calls, x86-64 Android ELF calls, translated ARM64 Android ELF calls,
production FD/HTTP sources and real QueueSurface. Symlinks and genuinely distinct
volume IDs are required; missing capabilities fail instead of silently skipping.

The loopback peer uses an OS-assigned port, atomically publishes readiness and
is stopped by its owned process handle. Bounded runners restore environment
values, including absence, before deleting owned directories. Shortcut
publication uses the existing shared-memory wrapper. All four new native
executables are default build targets collected with runtime artifacts.

Linux adapters declare their substituted Win32/JNI boundaries and do not
establish Windows behavior. Local sanitizer runs disable only LeakSanitizer
because of the managed executor's inspection limitation; CI retains its normal
sanitizer environment. Actual CI status and exact head are reported in the PR,
separately from these local results.

## Remaining material work

| Area | Remaining contract / evidence needed |
| --- | --- |
| Provider connections | Stable/unstable counts, connection tokens, reacquisition, provider/client generations and teardown. Coordinate with the older PRs' activity-management work. |
| Public modules | Implement real behavior and dependencies for the exhaustive JSON rows. Compiler-only classes and success stubs do not close those contracts. |
| Location/sync | Ordinary expiry without a delivered fix, nonparallel sync declarations and SyncResult retry/backoff require focused lifecycle oracles. Mapped here, not fully reproduced. |
| Filesystem | O_PATH identity, Unix ACL/ownership, all `*at` flags, nftw mount/chdir/fd-budget/cycles, and atomic same-volume directory replacement remain incomplete. The new volume guard is not a directory transaction. |
| Timestamp range | Unusable nonpositive/out-of-range signed FILETIME returns EOVERFLOW; exact zero cannot become Windows' leave-unchanged sentinel. Filesystem precision and full Linux date ranges differ. |
| Waits/futex/signals | Host select deadlines are distinct from #11's guest repair. Complete futex bitset/requeue/wake counts, alternate signal stacks and ARM guest context need concurrency tests. |
| Codec/HTTP lifecycle | Window retention does not fix raw Codec pointer/concurrent release lifetime. Whole-resource memory limits, mutable HTTP resources and every malformed Content-Range remain separate. |
| Graphics/memory | SharedMemory protection masks, SurfaceTexture external GL texture behavior and ignored transform/dataspace/framerate operations remain significant. |
| Native/hardware | Native Windows ARM64, all ELF unload/namespace behavior, NDK media families, protected media and physical camera/sensor/audio hotplug are not established by x64 fixtures. |
| Process model | General fork, mount/namespaces, some procfs/accounting, scheduler operations and full system-service/module coverage still require real designs. |

One suspected issue was rejected: a raw ARM guest callback address is not
inherently missing dispatch. The translator's registered DEP execute handler
can route it through a host entry. This differs from the finalizer issue already
owned by #11.

## Primary references

- [Android16 NetworkStats](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework-t/src/android/app/usage/NetworkStats.java), [NetworkRequest](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkRequest.java) and [NetworkCapabilities](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-16.0.0_r4/framework/src/android/net/NetworkCapabilities.java).
- [ShortcutService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/pm/ShortcutService.java), [ShortcutInfo](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/core/java/android/content/pm/ShortcutInfo.java) and [LocationManagerService](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/services/core/java/com/android/server/location/LocationManagerService.java).
- [Bionic futimens](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/futimens.cpp), [timeval wrappers](https://android.googlesource.com/platform/bionic/+/refs/tags/android-16.0.0_r4/libc/bionic/sys_time.cpp) and [Linux v6.6 ordering](https://github.com/torvalds/linux/blob/v6.6/fs/utimes.c).
- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew), [ReOpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-reopenfile), [SetFileTime](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfiletime), [FILETIME](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime) and [native rename](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information).
- [AOSP FileSource](https://android.googlesource.com/platform/frameworks/av/+/refs/tags/android-16.0.0_r4/media/libdatasource/FileSource.cpp), [MediaHTTPConnection](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/media/java/android/media/MediaHTTPConnection.java), [Surface](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/gui/Surface.cpp) and [native window contract](https://android.googlesource.com/platform/frameworks/native/+/refs/tags/android-16.0.0_r4/libs/nativewindow/include/system/window.h).
