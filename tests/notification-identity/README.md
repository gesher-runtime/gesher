# Notification identity regression

Different Android notification keys can have identical Java `String.hashCode`
values, for example tags `Aa` and `BB` for the same package and ID. The old storage
and large-image filenames then alias even though Windows displays distinct toasts.
The second saved record replaces the first, so the first toast loses its stored
activation state after the system server restarts.

New records and large images use SHA-256 over the exact Java UTF-16 code units.
Image basenames hash the length-delimited package/key pair, keeping the complete
basename at 81 characters even for long valid Android package names.
The Parcel record format and Android-visible notification key remain unchanged.
Existing records still load through the old hash filename only after the existing
reader verifies their full key and package/owner identity. Updating or canceling a
notification removes its legacy record only if that verification succeeds. It
never deletes another key's colliding legacy record. Existing image files remain
in place because retained Windows toast XML can still reference those paths.

`tools/test_notification_identity.py` compiles current production classes against
real application/framework interfaces. It then executes the production save/load,
delete and image-path methods in separate write/read JVM processes. Its explicit
host `Parcel` adapter preserves fields but **does not implement Android's Parcel
wire format**. The Android Notification object is allocated without its constructor;
no PendingIntent, Windows toast or bitmap compression is exercised. Native array,
log, identity and clock initialization uses the existing host JNI boundaries.
These limitations are separate from the actual file ownership and migration paths
being tested.

Run with the usual `--java-home`, `--framework-classes`, `--core-classes` and
`--app-classes` inputs. On Windows also supply `--cc <llvm-mingw C compiler>`.
The original filename-control result against `473beec7` was produced with the
runner at `ae069416`. The current runner also includes Windows-tag migration;
see [TOAST_MIGRATION.md](TOAST_MIGRATION.md) for its source-compatible control.

The added `notificationKeyCollisions` case in the existing Windows
`NotificationChannelGroupsTest` uses real Android Parcel/AIDL and persisted files,
including a new service instance, legacy fallback and colliding-key cancellation.
That fixture is compiled by the host runner and built/run by the ordinary Windows
services gate. Host tests do not establish that this Windows/ART gate has passed.

## Narrow local Windows/ART validation

`tools/test_notification_identity_art.py` additionally builds a DEX overlay of
the changed production Store/Icons/Identity classes and the separate
`NotificationIdentityDevice` fixture. It places that overlay before the existing
application jar on the supplied runtime's boot classpath. The runtime executes
without a boot image, in the interpreter, using fresh profile directories. Full
runtime logs are kept under the required new `--output` directory.

This check executes actual Android Parcel marshalling/unmarshalling, production
disk persistence across two VM processes, Bitmap allocation, PNG compression,
legacy fallback and collision-safe cancellation/update. The write phase creates
red and blue images and verifies distinct paths/content. It performs no toast UI,
PendingIntent activation or AIDL transport. It uses existing native/runtime builds,
so it is not a full clean build of the complete application.

Run with the normal Java/framework/core/app compile inputs plus `--runtime`
(a built directory containing dalvikvm.exe, boot jars, build.prop, fonts,
framework-res.apk and ICU data), `--d8-jar`, and a fresh `--output` directory.
The 2026-11-18 local Windows run passed 8 write and 13 read checks, including an
actual PNG write for a 204-character package name accepted by Android's package
validator (the AOSP-derived validator's filename limit is 223 characters). The same
command at `ae069416` with `--source-ref 473beec7` failed at
`Colliding keys share storage path`. The current runner additionally exercises
Windows-tag metadata migration as documented in [TOAST_MIGRATION.md](TOAST_MIGRATION.md).
