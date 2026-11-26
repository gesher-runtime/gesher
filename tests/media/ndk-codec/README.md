# Native AVC buffer contract

Build the same public NDK fixture as an Android ELF executable and an ELF JNI
library for x86-64 and ARM64.

```sh
bash tests/media/ndk-codec/build.sh out "$ANDROID_NDK" "$ANDROID_BUILD_TOOLS/d8"
```

Run the matching `out/media/ndk-codec/ndk-codec-*` executable on Android.
The Windows media shard loads the two JNI libraries in separate dalvikvm
processes. It exercises the app-host codec exports through the native library
loader, including the translated ARM64 guest ABI.

The fixture submits three different 128×96 I420 images to a real AVC encoder
and passes its compressed access units and codec-specific data to a real AVC
decoder. It checks each resulting image's luma, including the corresponding
64-bit timestamp, dimensions, format ownership, slot return, EOS and flush. It
also checks absent codecs, idle start and dequeue, missing parameter handles,
caller sizes on invalid buffer reads, and owned empty formats for unavailable
codec state or output slots. The action-code helpers classify transient and
recoverable actions independently of media status values.

The live-parameter test waits for an ordinary AVC picture, submits
`request-sync=0` and a `video-bitrate` target, then inspects the next picture's
actual IDR NAL unit. The Windows JNI test selects libavc and also reads the
new bitrate target after the backend accepted it. This is the configured target
and the test does not measure achieved throughput. Android 16's ARM64 AVC
provider returned success even for an idle submission and continued exposing
the original bitrate in its output format. The documented NDK contract limits
submissions to a running codec and permits controls to go unapplied. Gesher
rejects inactive submissions and handles sync and positive bitrate requests
for a running video encoder. [Android NDK parameter contract](https://developer.android.com/ndk/reference/group/media#amediacodec_setparameters).

A second decode sends the same AVC access units to a YUV ImageReader surface.
The test compares each submitted render timestamp and luma with the acquired
image. It switches the output while the second decoded buffer is held and
checks that frame reaches the successor reader. It then attempts to replace
the output with a native window whose reader has been deleted. The call
returns an error, the successor still gets the last frame, and the first image
remains readable after decoder stop. The original reader receives no additional
images. [Android NDK surface contract](https://developer.android.com/ndk/reference/group/media#amediacodec_setoutputsurface).
The frame-rendered callback carries that frame’s presentation time and a guest
monotonic timestamp. The fixture also checks callback unregistration.

The encoder input test replaces a configured producer, writes a real frame
to its successor, releases the caller's window references and waits for an
AVC slice and EOS. The Android executable draws with EGL. The Windows
fixture writes the same frame through the native window's CPU lock and post
interface, exercising its normal QueueSurface producer.

The persistent producer test submits a frame and receives AVC and EOS from
each of two successive encoders. It releases the last caller-owned native
window reference after the second submission, before signalling EOS. The
Windows fixture also checks repeat attachment by the current owner and
rejects competing attachments and ordinary input windows.

The fixtures cover synchronous CPU buffers, decoder output surfaces and
ordinary and persistent encoder input surfaces. Asynchronous buffer callbacks,
secure codecs and standalone Gesher ELF execution require separate work.
