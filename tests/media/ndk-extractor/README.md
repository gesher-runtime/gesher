# NDK extractor contract tests

These fixtures check Android-visible extraction behavior. They use explicit test
media and callbacks; ordinary extraction continues to use supplied media through
the existing AOSP demuxers and Windows data sources.

## Android contract references

The source oracle is Android 16, tag `android-16.0.0_r4`, frameworks/av commit
`22fd1c5d895e45a49df5582a4b345d8e3b75e46b`:

- [NdkMediaDataSource.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/ndk/NdkMediaDataSource.cpp)
  copies callbacks and userdata on attachment and forwards every explicit close.
  Deleting the caller's descriptor does not delete an extractor's copy.
- [NdkMediaDataSource.h](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/ndk/include/media/NdkMediaDataSource.h)
  permits close to unblock an in-flight read, requires thread-safe callbacks and
  permits later close callbacks after the first close.
- [NuMediaExtractor.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/libstagefright/NuMediaExtractor.cpp)
  queries availability with offset `-1`, retains total bitrate/duration, and
  rejects a track whose backend fails to start before retaining its selection.
- [MediaTrack.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/libstagefright/MediaTrack.cpp),
  [NdkMediaErrorPriv.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/libmedia/NdkMediaErrorPriv.cpp)
  and [NdkMediaExtractor.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/ndk/NdkMediaExtractor.cpp)
  determine the observable status values. Repeated setup, invalid track indexes
  and an unrecognized container translate to `AMEDIA_ERROR_UNKNOWN`; a null
  destination to `getSampleFormat` explicitly returns `INVALID_PARAMETER`.
- [DataSourceFactory.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/libdatasource/DataSourceFactory.cpp)
  recognizes supported URI schemes without regard to letter case. URL paths,
  file paths and headers keep their own case, and HTTP still requires INTERNET.
- [Utils.cpp](https://android.googlesource.com/platform/frameworks/av/+/22fd1c5d895e45a49df5582a4b345d8e3b75e46b/media/libstagefright/Utils.cpp)
  extracts decoder configuration from ESDS for AAC as well as MPEG-4 video.

## Guest API fixture

Build both guest ELF ABIs with FFmpeg, FFprobe and NDK r27c installed:

```sh
bash tests/media/ndk-extractor/build.sh out /path/to/android-ndk-r27c
```

The build produces a valid AVC/AAC MP4, WAV and bounded-FD fixture. FFprobe reads
the generated AAC AudioSpecificConfig into `fixture_csd.h`; the guest assertion
compares returned `csd-0` to those exact bytes. AVC SPS/PPS assertions remain in
place. Other cases cover descriptor snapshots and deletion, failed-setup retry,
repeated explicit close, a blocked source read, exact error codes, mixed-case
file URIs, track interleaving, seeking, sample boundaries and PSSH.

The media workflow builds these files, and
`tests/windows/tracks/87-ndk-extractor.ps1` runs both ELF ABIs through the actual
Gesher loader. Building the ELF files alone does not execute the guest API,
demuxers or Windows callback bridges. The same generated binary/media set can
also be run on a matching Android device to check the public API oracle.

## Host start and sample-read errors

```sh
python3 tools/test_extractor_start_contracts.py --sanitize
```

This fixture inserts the current `AospTrackReader` class, `Format` methods, and
NDK track selection and sample-format functions into a host executable. It
exercises reader construction and destruction around controlled plugin
start/stop/free operations. It verifies immediate I/O failure, unknown-status
translation, cleanup after failure, retry, repeated selection, and exactly one
stop/free on successful selection release. Sample-format queries distinguish
backend read errors from end of stream and leave the destination format intact
on failure. `--out /path/to/evidence` retains the generated source, compiler
output and run output. The runner prints production hashes.

The buffer group, format storage and selected error-translation cases are host
seams, and the extractor object supplies controlled track metadata. This fixture
does not run an AOSP demuxer, perform network I/O or establish Windows/ART,
translated ARM callback, complete linking or device behavior. Those boundaries
require the corresponding product and device runs.
