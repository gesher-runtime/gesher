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

This fixture covers synchronous CPU buffers. Surface input, asynchronous
callbacks, secure codecs and standalone Gesher ELF execution require separate
implementation and validation.
