# 0062. native3: remaining NDK APIs and bitmap interoperability

Status: proposed for the remaining native3 scope; the software Bitmap
bridge is verified in ci 40305973210 (2026-10-26).

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

## Native fonts (next slice, pending CI)

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
