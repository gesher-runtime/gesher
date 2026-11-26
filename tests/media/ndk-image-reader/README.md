<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native RGBA ImageReader contract fixture

The fixture uses public Android APIs to post distinct RGBA pixels through the
reader's `ANativeWindow`. It exercises library and symbol lookup, image metadata
and bytes, maxImages admission, synchronous and asynchronous acquisition, the
listener's dedicated thread, buffer removal after a producer resize, and native
`AHardwareBuffer` references retained past reader close. The Android reference
can keep buffers in free slots until a producer needs a different allocation.
The Windows builds of the fixture load through ART, so the actual
`libmediandk.so` and `libandroid.so` guest exports are required.

Build the PIE and JNI fixtures for both guest ABIs with NDK r28c and SDK D8:

```sh
bash tests/media/ndk-image-reader/build.sh out /path/to/ndk /path/to/d8
```

For the Android 16 ARM64 reference, run the public NDK PIE:

```sh
adb push out/media/ndk-image-reader/image-reader-aarch64 /data/local/tmp/
adb shell 'chmod 755 /data/local/tmp/image-reader-aarch64 && /data/local/tmp/image-reader-aarch64'
```

On Windows, use the installed Gesher ART graphics boot class path and boot
image used by `tests/windows/tracks/30-graphics.ps1`. Substitute
`image-reader.dex` for the application class path, set the main class to
`gesher.NdkImageReaderTest`, and give it the absolute Android-style path to
`libimage-reader-x86_64.so` or `libimage-reader-aarch64.so`. Each run should exit
zero and print `NDK image reader: ... checks passed`.

The `AImageReader` implementation provides CPU-readable RGBA_8888 via the
native window's CPU lock/post path. `AImageReader_newWithUsage` accepts both
CPU_READ_OFTEN and CPU_READ_RARELY for RGBA and YUV. Gesher preserves the chosen
usage in the producer's query and in the allocated hardware buffer. Android's
allocator can combine the requested usage with the producer's requirements. The
portable fixtures check CPU-readable buffers and real pixels, and the Windows
components also check their allocated buffers retain CPU_READ_RARELY. Producer pixels are
copied synchronously into the existing CPU-accessible native hardware buffer
before the reader publishes them. An acquired image keeps its own buffer
reference. Async acquisition returns the completed CPU pixels with no fence.
Async deletion takes ownership of a release fence descriptor and keeps the
image storage until the native event completes. Buffer removal is dispatched on
the reader's callback thread with a live buffer reference. The app can acquire
an independent reference in that callback. The standalone Android fixture
passes an EGL native fence when the device supports that extension. The RGBA
fixture covers the CPU window producer. The YUV fixture below covers decoder
output.

The contract follows the Android 16 public
[ImageReader and Image API](https://developer.android.com/ndk/reference/group/media).

## Decoder output in an NDK YUV reader

Add the API 36 `android.jar` path as the fourth build argument to build the
YUV comparator. This also requires FFmpeg with libx264. The build makes a
two-frame AVC test video and JNI libraries for both guest ABIs.

```sh
bash tests/media/ndk-image-reader/build.sh out /path/to/ndk /path/to/d8 /path/to/android.jar
adb push out/media/ndk-image-reader/{yuv-reader.dex,libyuv-reader-aarch64.so,red-blue.mp4} /data/local/tmp/
adb shell 'CLASSPATH=/data/local/tmp/yuv-reader.dex app_process /system/bin gesher.NdkYuvImageReaderTest /data/local/tmp/libyuv-reader-aarch64.so /data/local/tmp/red-blue.mp4'
```

On Windows, use the same ART setup described above with `yuv-reader.dex`,
`gesher.NdkYuvImageReaderTest`, the appropriate JNI library path, and the
absolute MP4 path. Java MediaCodec decodes the AVC video directly into the
NDK reader producer. The fixture compares both timestamps, image dimensions,
crop, plane layout and color samples seen through AImage and
AHardwareBuffer_lockPlanes. It holds the first image while the second is
delivered and checks its pixels again afterward.

The CPU-readable YUV_420_888 reader accepts I420 from the video decoder and
NV12 from the in-process Camera2 service. It owns the Y, Cb and Cr planes of
each published image. The camera path requires a camera and an Android camera
grant. This fixture exercises the decoder path. Native GPU dequeue and
cross-process YUV surface producers still require integration.

## PRIVATE GPU-sampled reader

Pass `private` to the public NDK PIE, or after the JNI library path to
`gesher.NdkImageReaderTest`. The fixture renders red and blue through the
reader's EGL window, acquires both images with their fences, and samples
their hardware buffers as external EGL textures. It also samples the
retained first image again after the second post. The fixture requires EGL
native buffers and `GL_OES_EGL_image_external`.

`AImageReader_newWithUsage` supports `AIMAGE_FORMAT_PRIVATE` with
`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE` when the Windows GPU can supply
a sampled buffer. The in-process producer uses RGBA staging and the existing
D3D-backed hardware buffer import path. The consumer gets zero planes,
and the hardware buffer denies CPU locks. Each acquired image owns its
buffer until release. Raw GPU dequeue slots and video-encoder usage are
outside this path.
