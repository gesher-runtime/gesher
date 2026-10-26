# 0062. native3: remaining NDK APIs and bitmap interoperability

Status: proposed (native3 track, 2026-10-26; awaiting CI)

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
cross-ABI compression callback and exact data layouts need CI verification;
local Java/PowerShell/patch checks do not establish those behaviors.
All other native3 scope in docs/handoff/tracks/native3.md remains open:
public NDK export inventory, the other platform libraries, HardwareBuffer,
ABI/Unicode parity, record locks, inotify, proc metadata and file mremap.
