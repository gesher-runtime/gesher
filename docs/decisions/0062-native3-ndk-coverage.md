# 0062. native3: remaining NDK APIs and bitmap interoperability

Status: accepted for the software Bitmap and Minikin font foundation,
verified in ci 40344682373 at 1e881b5, attempt 2 (2026-10-26).
Unicode classification/case and Process proc natives are also accepted,
verified in full ci 40386670586 at 6f6ac6f, attempt 1; both Windows suites,
package and required e2e logs personally read. See the scoped report in
handoff/tracks/native3-unicode.md. Remaining NDK/POSIX scope stays open.

## Context

native2 verifies the imports used by its real programs and the e2e apps,
but its broader NDK stub-library report still contains missing exports
(ci 40296960603). Its platform dlopen check reports libjnigraphics absent,
along with libmediandk, libcamera2ndk and libvulkan. The existing Java
Bitmap implementation already has real pixels and compression in hwui;
Android native libraries cannot reach them through the shim.

## Decision

Keep AOSP's Bitmap storage and semantics. Build the pinned hwui
apex/android_bitmap.cpp (ABitmap's API, normally Android-only) and put
System V AndroidBitmap entry points in compat/bionic/libjnigraphics.cc.
Convert each loaded library's JNIEnv proxy with RealJniEnv. Locking pins
the actual ABitmap pixels with a reference, and unlocking notifies changes
and releases that reference. Compression calls back through a Microsoft
ABI trampoline into the loaded code's System V callback. The existing
platform-library registry supplies the libjnigraphics handle when these
entry points are registered.

hwui's host Bitmap has software storage and no hardwareBuffer() method.
Patch 0620 makes ABitmap_getHardwareBuffer return null there, so the NDK
wrapper reports BAD_PARAMETER for a software bitmap, as on Android. This
does not provide HardwareBuffer rendering; that work remains open.

The tests require direct aliasing: a native pixel write must already be
visible through Bitmap.getPixel while the pixels are locked. They also
compress through a loaded-code callback, decode the PNG in Java, and check
its modified pixel. JNI-library tests run for x86_64 and arm64 in dalvikvm
with framework.jar and in APKs, single process and on the device.

## Consequences

No duplicate pixel buffer or Java-only copy emulates a native lock. The
cross-ABI compression callback, direct storage aliasing and tested RGBA
layout are verified in ci 40305973210 at e94f760 on both Windows runners,
through framework dalvikvm and APKs in both execution modes. Both Windows
summaries end with 0 test(s) failed; every job is success/skipped, package
install/run/uninstall cleanup passes, and e2e has 0 unexpected failures
(TIC-80 remains a known failure on this branch without surfaces).
All other native3 scope in docs/handoff/tracks/native3.md remains open:
public NDK export inventory, the other platform libraries, HardwareBuffer,
ABI/Unicode parity, record locks, inotify, proc metadata and file mremap.

## Native fonts (verified foundation)

NDK font matching reuses the same Minikin collection and font objects as
Typeface. Itemization and metadata follow the pinned
[system_fonts.cpp](https://android.googlesource.com/platform/frameworks/base/+/refs/tags/android-16.0.0_r4/native/android/system_fonts.cpp)
contracts; a TinyXML2 fallback reads shipped fonts.xml before the Minikin
font set is registered. Returned metadata owns its strings/axes independently
of the iterator/matcher. Tests validate actual file headers/collection
indices, available metadata, mixed-script run bounds and italic selection
through loaded native code in both ABIs/modes. This does not yet establish
standalone native-process font initialization or runtime font-install/update
behavior; those contexts remain open.

The combined foundation run verifies all 18 entries, real files, meaningful
variable-axis values, matching and lifetime on both ABIs through framework
dalvikvm and APKs in both modes. All 28 jobs are success/skipped, both
Windows summaries say 0 test(s) failed, package cleanup passes and both
required TIC-80/Puzzles e2e jobs say 0 unexpected failures. Attempt 1 failed
only the known output-tone timing check; the permitted failed-job rerun
passed. See the frozen stage report in handoff/tracks/native3-foundation.md.

## Unicode classification and case mappings (verified)

Windows' dynamically loaded ICU version and the CRT's 16-bit locale forms
are replaced by one bridge over the pinned ICU4C data. It uses bionic's
ASCII rules, Alphabetic/Lowercase/Uppercase/POSIX binary properties and
exact category bounds. Plain and locale-suffixed System V entries share
this bridge, including supplementary characters and invalid sentinels.
Missing locale aliases and invalid wctrans/towctrans EINVAL are included.

An independent CI-only reference compiles the unmodified pinned bionic
ICU4X wrapper with locked beta2 crate/data versions. ci 40374405950 records
14 whole-code-point digests and 11 boundary rows; its dependency lock was
saved from the log and hash-checked. The production host test calls actual
plain/locale shim entries for every code point and four invalid values;
NDK tests cover reference rows, handles and error paths in both ABIs/modes.
The full comparison passes in ci 40386670586 on both Windows versions,
including NDK boundary/handle/error checks in both ABIs and APK modes.
No local AOSP/ICU build ran. wcwidth/wcswidth, collation and the rest of
native3 remain open; only the scoped Unicode row is ticked.
