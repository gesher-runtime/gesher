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
also checks that absent codecs and operations in the idle state report the
Android error behavior.

A second decode sends the same AVC access units to a YUV ImageReader surface.
The test compares each submitted render timestamp and luma with the acquired
image and verifies the first image remains readable after the decoder stops.

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
ordinary and persistent encoder input surfaces. Asynchronous callbacks,
secure codecs and standalone Gesher ELF execution require separate work.
