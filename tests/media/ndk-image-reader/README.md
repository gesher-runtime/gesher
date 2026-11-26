# Native RGBA ImageReader contract fixture

The fixture uses public Android APIs to post distinct RGBA pixels through the
reader's `ANativeWindow`. It exercises library and symbol lookup, image metadata
and bytes, maxImages admission, acquire-next and acquire-latest behavior, the
listener's dedicated thread, and a native `AHardwareBuffer` retained past reader
close. The Windows builds of the fixture load through ART, so the actual
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

The `AImageReader` implementation in this slice provides CPU-read-often
RGBA_8888 via the native window's CPU lock/post path. Producer pixels are
copied synchronously into the existing CPU-accessible native hardware buffer
before the reader publishes them. An acquired image keeps its own buffer
reference. Further formats, other consumer usages, GPU buffer dequeue, sync
fence APIs, and camera/codec integration are outside this fixture.

The contract follows the Android 16 public
[ImageReader and Image API](https://developer.android.com/ndk/reference/group/media).
