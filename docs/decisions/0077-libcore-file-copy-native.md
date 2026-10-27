# 0077 - Expose the linked UnixCopyFile native

**Status:** proposed, 2026-10-27. No CI acceptance yet.

## Context

Full40503913306 reached Files.copy and threw UnsatisfiedLinkError for
sun.nio.fs.UnixCopyFile.transfer(int,int,long) on both Windows versions.
The pinned libcore ojluni Android.bp already includes UnixCopyFile.c in
libopenjdk_native_srcs. Its native is found by name, and Gesher's static
libopenjdk table omitted that symbol.

## Decision

Add the real upstream JNI entry to the existing built-in symbol table,
using its existing JNICALL signature. Retain the pinned transfer body,
gesher-base read/write and UnixException behavior. No fallback copy body
or synthetic cancellation result is added.

Test public Files.copy across empty,8192-byte-boundary,large and UTF8-path
files, replacement/preservation and missing-source failure. Invoke the same
native with actual descriptors/offsets and an allocated cancellation word;
verify the pinned read-before-cancel, EOF-before-cancel and actual EBADF
ordering as well as caller ownership and successful reuse after clearing
the flag. Tests build and execute only in CI.

## Evidence and limits

Primary android-16.0.0_r4 sources read before editing:
UnixCopyFile.c SHA25648abb948c7dcf192e199e804e57ac089485d8c713c5c901ff6c7a66ebd87f6c9;
UnixCopyFile.java SHA2560cc4bd476e3e5fdf6f202fdf4350ba94206501e814b0a03387708dfd346a9a2f;
ojluni/native/Android.bp SHA2563c38ef17fe99d0e75e55a005ed4beb6af9608361cfe2d694de945eb492f68b9e.
Primary links:
[native body](https://android.googlesource.com/platform/libcore/+/refs/tags/android-16.0.0_r4/ojluni/src/main/native/UnixCopyFile.c),
[Java caller](https://android.googlesource.com/platform/libcore/+/refs/tags/android-16.0.0_r4/ojluni/src/main/java/sun/nio/fs/UnixCopyFile.java),
[source list](https://android.googlesource.com/platform/libcore/+/refs/tags/android-16.0.0_r4/ojluni/src/main/native/Android.bp).
They were fetched from the tag on android.googlesource.com; no local AOSP
checkout/build or native/JVM fixture execution was used.

Compilation, public copy and native runtime checks are pending. Short-write
and EINTR loops retain upstream source but have no induced-runtime proof in
this slice. This does not establish completeness of all libcore name-found
natives, filesystem metadata, symbolic-link or asynchronous cancellation
contracts; further observed gaps remain assigned to the track.
