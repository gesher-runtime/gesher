# Notification image preparation and ownership — 2026-11-22

## Scope and remaining acceptance

This is the first JAVA-07 leaf of [issue178](https://github.com/gesher-runtime/gesher/issues/178),
based on `638e19b4a6c109ec8881ba90a94d64b1817fc095`. It addresses the bitmap large-icon
path in [NotificationIcons](../../app/java/com/gesher/server/NotificationIcons.java)
and its [notification publication owner](../../app/java/com/gesher/server/NotificationManagerService.java).
JAVA-07 and the whole performance issue remain open.

| Requirement | This leaf |
| --- | --- |
| Avoid whole encoded-image copies | PNG compression streams into an owned staging file; the large-icon path no longer builds a `ByteArrayOutputStream` and `toByteArray` copy. |
| Reuse unchanged images with explicit invalidation | A 256-entry optional metadata LRU matches package generation, source content/format and the exact current PNG content. No Bitmap or PNG body is retained in that cache. |
| Keep slow image preparation outside the global publication fence | Pixel hashing, PNG encoding, file verification and staging writes occur outside it. Same-key preparation has an independent lane and at most four preparations hold encoder credits. |
| Preserve cancellation, replacement, uninstall and foreground/job ownership | Preparing posts carry invalidatable tokens; publication rechecks owner, delegate, permission and channel generation. Foreground/job work already holding the lifecycle fence prepares on its caller after the outer fence is released. |
| App icon preparation and cache lifetime | **Remaining.** APK icon drawing and the existing `sDrawn`/`sStamps` maps are unchanged. |
| Store serialization and native show under the publication fence | **Remaining.** Atomic PNG publication, `NotificationStore.save`, app registration and native show remain serialized. |
| Whole-path memory/latency/CPU and hardware acceptance | **Remaining.** Local counts establish ownership and eliminated encoder calls at the stated fixture boundary. Windows/ART component samples and a real notification-update workload must establish performance on the target. |
| Global retained-toast disk quota | **Not supplied.** Live/persisted images are required resources, not evictable cache entries. |

The package/delegate/channel/permission policy and the actual supplied bitmap remain
authoritative. PNG quality, dimensions and application content are not reduced to
meet a performance threshold. Sources for the Android-side operations are the
[Bitmap generation and pixel APIs](https://developer.android.com/reference/android/graphics/Bitmap)
and the existing repository [toast ownership rules](../../tests/notification-identity/TOAST_MIGRATION.md).

## Image and file lifetime

[NotificationImageStore](../../app/java/com/gesher/server/NotificationImageStore.java)
owns a synchronous preparation from lane acquisition through close. A unique
`.part` file is private to that preparation. Publishing uses an atomic same-directory
move after lifecycle revalidation; stale, failed and abandoned preparations close
their output and remove their own stage. A failed atomic move does not delete the
prior target as a fallback. Filesystem refusal to delete is reported; the code
does not claim cleanup can override OS errors.

A successful published PNG belongs to the Windows toast and survives Java owner
close, metadata-cache eviction and process exit. The published pathname remains
mutable per canonical notification key, as in the baseline. This leaf does not
introduce immutable published generations or transactional rollback of a PNG if
the subsequent native show fails. A show failure preserves the file because an
earlier retained toast may still reference that pathname.

New large icons live in a bounded-name directory derived from the package digest;
the basename remains derived from the complete canonical notification identity.
The old root-level pathname is retained until the corresponding legacy toast is
successfully retired. Failed native removal preserves both image layouts. A
repeated cancellation with no surviving metadata is not proof that Windows
removed a previously retained toast. Successful package-wide history removal
can clean the new package directory. Historical root-level files whose owner
records are already absent cannot safely be enumerated by package and remain
a migration cleanup limitation.

Removal never joins an active encoder. It marks deletion against the same lane;
the preparation which still owns that lane performs deletion when it closes.
A later accepted publication clears an earlier deletion request. Notified
same-key waiters keep the same lane during intervening deletion, so no second
owner can encode/publish that pathname concurrently.

## Cache contract and bounds

Source fingerprints include dimensions, pixel format, alpha/premultiplication
flags, color-space identity and all pixels. The reader uses a 1024-pixel scratch
chunk, not a second full bitmap. Only ordinary ARGB8888/RGB565 sources with the
canonical sRGB color space (or absent color-space metadata) are reusable. Other
formats, wide gamut, HDR and gainmaps still go through the original PNG encoder
but do not use this fingerprint to infer equivalence. Bitmap generation and
format metadata are checked again after preparation; a changing source is not
inserted into the reusable cache.

Each optional entry contains two SHA256 digests, the encoded length and a weak
package-generation reference. A candidate hit also reads and hashes the actual
published file. A same-size/same-timestamp replacement therefore cannot be
mistaken for the previously published PNG. A package-generation change misses
even if the pixels match. Eviction removes metadata only, never a toast's file.

| Resource | Explicit bound or ownership |
| --- | --- |
| Optional fingerprint metadata | 256 entries across packages |
| Simultaneous preparation/encoder credits | 4, held until preparation close |
| Source fingerprint scratch per preparation | 1024 ints + 4096 bytes, plus small metadata/digest state |
| File verification/write buffering | 8192-byte streaming buffer per active operation |
| Additional cached encoded-image files | None |
| Published PNGs | One current-layout file per retained key; required files are not evicted for the metadata limit |
| Native codec scratch and supplied Bitmap storage | Owned by the codec/caller; no new absolute size cap is claimed |
| Waiting callers and their lane/token records | Owned by existing synchronous calls; no worker pool or independently scheduled task queue is added. Their total is not bounded by the four encoder credits. |

Moving preparation off the global fence permits unrelated packages to progress
while fewer than four preparation slots are occupied. Exhausting all four slots
can still make another caller wait. Hashing source pixels and validating PNG bytes
cost CPU and I/O; eliminating compression is not, by itself, a latency claim.

## Verification

The following local runs use Java21, the supplied Android framework/core compile
inputs, and the complete production owner/service code. Native adapters and
constructed metadata are identified explicitly by their runners.

| Check | Local result and boundary |
| --- | --- |
| `tools/test_notification_images.py` standalone | 446 checks: actual filesystem, production owner/cache/admission code, explicit synthetic encoder bytes |
| The same runner with framework/core/app/apksig inputs | 46 checks: actual NotificationManagerService and ImageStore; constructed packages and explicit codec/toast/JNI boundaries |
| Baseline cancellation negative control | Main `638e19b4` fails the unchanged pre-publication probe with `cancellation was undone by an earlier preparing post` |
| Existing `test_notification_channel_groups.py` | 88 host checks |
| Existing `test_notification_identity.py` | 1415 host identity/toast/format checks across write/read on the initial `4e3595e035` candidate; explicit host Parcel encoding. The subsequent correction leaves storage and published-poster authorization unchanged. |
| Extended `test_notification_delegation.py --with-storage` | 270 protected-profile and 260 legacy-profile host checks, including pending-poster and real prepared-file ownership |
| Complete app and services-test DEX compilation | Uses `app/build.sh` and `tests/dex-services/build.sh` against the supplied framework/core interfaces |

The ownership suite verifies 100 unchanged updates after an initial encode still
have one encode total, distinct same-content source reuse, changed source/owner
misses, corrupt-file misses, false/throwing encoders, bad directories, failed
atomic publication, eviction without live-file deletion, four blocked encoders,
same-key interruption, cancellation and complete credit/lane retirement. Those
are exact call/resource observations from a synthetic encoder; they are not PNG
or Windows speed measurements.

The service suite blocks one package inside the preparation encoder while another
package posts. It also exercises cancellation while preparing, two pending
same-key posts followed by cancellation and a later replacement, uninstall and
reinstall, failed native removal followed by repeated cancellation, foreground/job
lifecycle reentrancy, and cleanup when an earlier deferred callback fails.

Independent QA of `4e3595e035da8b8b18638deba80672440ebbde06` found two regressions
introduced by the preparation window. A delegate could invalidate another poster's
pending generation, and the final UIJ association could detach before publication
without clearing the pending notification's UIJ flag. The correction filters
pending cancellation by each generation's `opPkg`; owner/system cancellation
still covers all posters, while authorization of an already-published generation
remains unchanged. Final-UIJ detachment now updates matching pending records as
well as the visible record, preserving the notification and its prepared image.
A visible record still awaiting image-owner close is handled separately so its
persisted flags and listener update are not skipped.

Eight committed delegation cases exercise owner, delegate and system cancellation,
published-poster rejection, and an owner replacing an existing delegate post.
The latter runs both a cache hit and a newly encoded image through the complete
production `NotificationImageStore`, with an explicit synthetic encoder. It checks
the actual surviving file contents and final cleanup, including deferred deletion
of the prior delegate image. This matrix runs in both protected and legacy profiles.
The extended suite fails against the initial `4e3595e035` service at
`Pending-poster cancellation crossed ownership for 191`.

The independent QA probes also pass after the correction: five delegated cases in
each profile (37 checks each), the original owner-pending probe (11 each), and the
UIJ probe (5 checks, detachment completes during preparation and publishes flags0).
The committed service fixture checks pending UIJ detachment with and without a
visible predecessor, and a still-active key unaffected by another key's detachment.
The Windows `JobNotificationLifecycleTest` additionally performs actual Run
completion while a post is preparing, for both final DETACH and a remaining shared
UIJ owner. Its source/DEX compiles locally; that complete job/Binder/native-Parcel
case still requires the Windows gate.

Required Windows track94 now executes the service race fixture and
`NotificationImageBitmapTest` on the real ART/Bitmap/PNG/filesystem boundary.
The latter checks opaque and translucent pixels, RGB565, alpha metadata changes,
mutable content, package-generation changes, file corruption, abandoned stages
and uncached wide-gamut encoding. The existing native format gate retains every
format assertion and additionally runs the full identity oracle in two processes,
including persisted red/blue PNG reads after restart. The Linux host results do
not establish that these Windows checks have passed.

The Bitmap fixture prints `NOTIFICATION_IMAGE_BITMAP_METRICS`: all 31 elapsed
samples for the old compress/toByteArray/write component and the candidate warm
reuse component, their medians, and the encoded byte copies removed from those
loops. Input is a fixed-seed opaque 256x256 bitmap. The baseline omits the old
temporary-file rename; the candidate includes its wrapper/check overhead. These
are component elapsed samples under the track's interpreter configuration,
not process CPU, RSS, ordinary APK notification latency, or a whole-app result.
No target improvement percentage is asserted before observing those results.

## Windows PNG oracle correction

The first published head `4c9625ca8554bc02bbd4c6e29a8f1ea539587e67` failed both new
PNG checks on Windows 2022 and 2025 in [CI run 45026032260](https://github.com/gesher-runtime/gesher/actions/runs/45026032260):
jobs [137227756931](https://github.com/gesher-runtime/gesher/actions/runs/45026032260/job/137227756931)
and [137227756917](https://github.com/gesher-runtime/gesher/actions/runs/45026032260/job/137227756917).
The combined assertions reported a dimension change and failed restart retention;
both were immediately preceded by HWUI's `Not supported on Windows` message. The
artifacts retained logs, not the PNG files or actual decoded dimensions, so those
logs alone did not prove a bad encoder, lost file or dimension mismatch.

The exact pinned Android 16 source explains the failure. [BitmapFactory.java](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/graphics/java/android/graphics/BitmapFactory.java)
opens `decodeFile`'s path with `IoBridge.open` and dispatches a seekable descriptor
to `nativeDecodeFileDescriptor`. [BitmapFactory.cpp](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/libs/hwui/jni/BitmapFactory.cpp)
returns null unconditionally under `_WIN32` in that method, with the observed
message. Gesher builds that host source in `cmake/aosp/hwui.cmake`; its JNI table
patch does not replace this branch. The cached framework compile artifact's
`javap` output independently confirms the same Java call chain. This is a
pre-existing **unimplemented file-descriptor Bitmap decoder API**, which remains
unfixed by this notification work. Neither open #226 (clipboard fixture) nor
#235 (WebView geometry) supplies that API.

The notification production path does not decode its PNG with BitmapFactory:
`NotificationManagerService.showToast` gives the saved file URI to the Windows
toast XML, or the same path to the native notification bridge. The PNG creation
and file-lifetime oracle now reads the actual saved file using `Files.readAllBytes`
and passes exactly those bytes to the existing real native
`BitmapFactory.decodeByteArray`/Skia decoder. It still fails for missing files,
invalid PNG headers, null native decoding, dimension changes and every prior
opaque/translucent/RGB565 pixel assertion. Mutable-source, alpha-metadata,
same-length corruption, abandoned-stage and cache-decision assertions remain.
This is a test-boundary correction, not evidence that `decodeFile` works or that
Windows has displayed a toast.

An explicit old-writer control uses the original compress-to-byte-array,
temporary-file write and rename sequence on the same actual Bitmap before testing
the new store. It requires the saved PNG's native byte decoding and all pixel
comparisons to pass; it separately prints whether `decodeFile` returned null.
If that API is available, its dimensions and pixels must also agree. On the
current Windows runtime the expected diagnostic is `byte_decoder=passed
file_descriptor_decoder=null`; its first observed target result remains pending.

Addendum (2026-11-24): `patches/frameworks/base/0786-hwui-decode-file-descriptors-gesher.patch`
builds the device path of `nativeDecodeFileDescriptor` and `ImageDecoder`'s
descriptor source on Gesher, whose POSIX layer provides `fstat`,
`fcntl(F_DUPFD_CLOEXEC)`, `fdopen` and `lseek`. `decodeFile` then returns the
image, so the old-writer control above checks its dimensions and pixels too;
`tests/dex-graphics` covers the descriptor decoders directly.

Every saved-image check emits its canonical pathname, byte count, SHA256, raw
33-byte PNG header and native decoded dimensions. Restart write records the
canonical path, length and SHA256 in a separate fixture receipt. The second VM
must read that receipt without recreating it, verify the exact persisted file
identity/content, decode that file's bytes and check all four pixels of both red
and blue icons. Existing notification and toast-format migration assertions are
unchanged. Track94 now retains these diagnostics and all 31 component samples in
`results.txt` even when the gate passes; its shared `Check` helper ordinarily
suppresses successful child output. The artifacts preserve diagnostic text,
headers, hashes and raw samples, not complete PNG binaries.

No production source, native patch, timeout, retry or accepted test failure is
changed by this correction. A fresh Windows 2022/2025 run remains necessary to
establish actual PNG fidelity, restart retention and the old-writer control.

Local correction checks pass: complete services javac/D8 compilation against the
actual framework and Android libcore inputs; the unchanged owner/service suites
(446/46 checks); PowerShell parsing of track94; and syntax checking of the narrow
ART runner. Independent QA passed the exact correction tree
`28e4484bb270f88404f1bc11372003d99e1a69e6` and ran a corrected 17-check host
file/header/receipt probe with a known 74-byte 2x2 PNG and an independently
calculated SHA256. Its rejection helper asserts outside the catch, so it cannot
swallow its own acceptance failure. It verifies rejection of missing, truncated
and malformed files and changed receipts for a different pathname, length and
same-length content corruption. The author's earlier 12-check probe had that
catch defect and is not relied on as negative-control evidence. The independent
probe does not invoke an Android native decoder and is not target PNG-fidelity
evidence.

## Reproduction

Run the standalone owner with a JDK on PATH:

```sh
python3 tools/test_notification_images.py
```

After building the current app compile-classes jar, supply the real interface
inputs for the host service suite:

```sh
python3 tools/test_notification_images.py \
  --java-home "$JAVA_HOME" \
  --framework-classes /path/to/framework-classes.zip \
  --core-classes /path/to/coremod \
  --app-classes /path/to/gesher-app-classes.jar \
  --apksig /path/to/apksig.jar
```

The same command with `--service-source-ref 638e19b4a6c109ec8881ba90a94d64b1817fc095`
must fail specifically at the cancellation assertion. It substitutes only the
original NotificationManagerService/NotificationIcons sources; the cancellation
oracle uses the already-existing publication hook rather than the new codec seam.

Build the complete test DEX using `tests/dex-services/build.sh` with the current
app/framework/core inputs. Run required Windows track94 on the resulting runtime
for the real PNG/native Parcel evidence. The existing narrow Windows runner
`tools/test_notification_identity_art.py` also includes the new production owner
in its overlay, allowing its supplied runtime to predate this Java change.
