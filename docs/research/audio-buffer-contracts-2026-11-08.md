# Audio buffer follow-up, 2026-11-08

This follow-up starts from PR #8 commit
`3843241968edfd124d3b9596c0e6381f45f5c381`. It repairs the JNI adapters that
transfer app buffers to the existing playback and capture engines. It does
not replace audio operations with fabricated successful return values.

## Implemented behavior

The pinned Android `AudioRecord.read(ByteBuffer, ...)` JNI path clamps a
request larger than a direct buffer to that buffer's capacity. Gesher
previously rejected that valid request. The repaired adapter transfers from
the buffer's origin, preserves its Java position and limit, and bounds the
actual transfer by the capacity. This behavior was checked against the
[Android 16 r4 AudioRecord JNI source](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/jni/android_media_AudioRecord.cpp).
The same source returns `ERROR_BAD_VALUE` for a non-direct buffer, despite
different historical wording in the Java documentation; that native contract
is preserved.

Three array-read adapters added `offset + size` in a signed `jint`. Large
arguments could overflow before checking the array bound. They now validate
nonnegative arguments and compare against `arrayLength - size`, which is
representable over those input ranges. Playback arrays receive the same
explicit bound check before acquiring their elements.

Playback previously allocated an additional C++ vector before copying the
Java array. An allocation failure in this no-exceptions runtime could
terminate the process across JNI. The implementation now uses the VM's
primitive-array access API, invokes the existing synchronous `Track::Write`
with the requested slice, and releases the array with `JNI_ABORT` because
playback does not modify the source. Capture retains copy-back on release.
The approach follows the pinned
[AudioTrack JNI array access](https://android.googlesource.com/platform/frameworks/base/+/android-16.0.0_r4/core/jni/android_media_AudioTrack.cpp).
No native pointer is retained beyond its JNI lifetime.

Direct playback now rejects null, non-direct, negative and out-of-capacity
inputs before pointer arithmetic or engine invocation. Null direct buffers
previously reached a fatal checked-JNI operation in the host diagnostic.
These native defenses supplement the framework's public argument checks;
they are not presented as evidence that every such invalid value can reach
JNI through an ordinary public call.

Internal transfer counts now remain `ssize_t` until short/float byte counts
are converted to Java sample counts. The former early `jint` conversion
could turn a valid transfer exceeding 2 GiB of bytes into a negative result.
Both engines already use wide counters bounded by the requested byte count.
This repair is source-verified; no multi-gigabyte Java array was allocated in
the test environment.

## Executed regression evidence

```sh
python3 tools/test_audio_buffer_contracts.py --java-home /path/to/jdk-21
```

The harness compiles the eight production buffer adapters and their actual
native registrations verbatim into a host JNI library. A real JDK 21 runs
with `-Xcheck:jni`; the native code runs with UndefinedBehaviorSanitizer.
Only the playback/capture engine boundary is replaced by a bounded transfer
observer. Tests inspect transferred payload bytes, array offsets, unit
conversion, copy-back, preserved source bytes, direct-buffer contents,
blocking flags, invalid argument isolation and clamped capacity.

**72 assertions pass:** 31 ordinary transfers, 22 direct-read capacity
checks, 6 capture bounds, 5 array-write bounds, 5 direct-write bounds and 3
null-buffer checks. Child JVMs are separate and time bounded. Negative
controls against the starting PR fail five of the six groups, reproducing
the valid capacity mismatch, signed overflow, a JNI array exception, invalid
direct-buffer forwarding and fatal checked-JNI null handling. The ordinary
transfer group also passes against the old source.

Both complete modified media translation units compile to Windows x86-64
objects with the repository's pinned llvm-mingw toolchain, pinned AOSP JNI
header plus the repository's JNI patches, and the pinned liblog header.
This catches the actual Windows types and System V JNI declarations, beyond
the Linux diagnostic envelope. It does not link or run a full media runtime.
The exploratory native Windows ARM64 media compile is outside the existing
ARM64 probe workflow and fails at its pre-existing x86-only `JNICALL`
attribute; it is not recorded as an ARM64 media success.

The host fixture does not prove WASAPI behavior, ART behavior, hardware
capture/playback, native allocation-failure injection, or translated ARM64
execution. Those integration paths remain distinct from these buffer
contracts. The CI preflight now runs this same checked-JNI/UBSan fixture.
