# Media: software video decoders and the Matroska/WebM extractor

Part of the media track's stage 2 (docs/media.md "Video", ADR 0052). Media
Foundation on the CI runners decodes H.264 and MPEG-4 part 2 only; the other
codecs apps meet come from AOSP's software decoders, built for Windows.

| Codec | MIME | Decoder (built from) | Container in the test |
|---|---|---|---|
| VP8 | `video/x-vnd.on2.vp8` | libvpx (`external/libvpx`, generic C config) | WebM |
| VP9 | `video/x-vnd.on2.vp9` | libvpx | WebM |
| AV1 | `video/av01` | libdav1d (`external/libdav1d`, C only, 8 and 16-bit templates) | MP4 |
| H.265 | `video/hevc` | libhevcdec (`external/libhevc`, SSSE3/SSE4.2 intrinsics) | MP4 |

- `cmake/aosp/media_video_codecs.cmake` builds the three libraries (source
  lists from their Android.bp files; third-party warnings silenced, `-w`).
- `cmake/aosp/media_codecs.cmake`: `gesher_media_extractor(mkv ...)`, AOSP's
  MatroskaExtractor with libwebm's mkvparser, libstagefright_flacdec
  (FLACDecoder.cpp, over libFLAC) and ColorUtils_ndk.cpp built in;
  `compat/media/player/extractor.cc` lists `GesherExtractorDef_mkv`.
- `compat/media/video/soft_video.cc`: `SoftVideoCodecs()` and
  `CreateSoftVideoDecoder()`. Each decoder gives 8-bit I420 frames in
  presentation order (deeper samples shifted down, 4:2:2/4:4:4 chroma
  subsampled, 4:0:0 gray chroma):
  - libvpx: one `vpx_codec_decode` per sample, the frames it shows carry the
    sample's time; Drain/Flush decode a null buffer.
  - dav1d: `max_frame_delay` 1, the picture's `m.timestamp`; Flush is
    `dav1d_flush`.
  - libhevc: the H.265 parameter sets (`AnnexBParameterSets`) go in front of
    the first access unit; header decode, then frame decode of the same bytes
    (as C2SoftHevcDec); the decoder reorders for display and returns each
    picture's `u4_ts` marker, mapped back to its time. Drain/Flush: flush mode
    until no picture is left, then a reset (parameter sets sent again).
- `VideoDecoder::Available()` prefers Media Foundation, these otherwise;
  MediaCodecList names them `c2.gesher.<vp8|vp9|av1|hevc>.decoder`. Their
  capabilities use 8x8 blocks for H.265/VP9/AV1 (MediaCodecInfo scales the
  block counts by the codec's block size against the level limits').
- `aosp.lock`: `external/libvpx`, `external/libdav1d`, `external/libhevc`
  (sparse), `frameworks/av-media` + `/media/module/codecs/flac/`.

Verified: ci run 40244779791, both Windows runners, `tests/windows/tracks/38-media.ps1`:
`VideoTest codec <vp8|vp9|av1|hevc> sync: c2.gesher.<name>.decoder, 320x240 color 19,
60 frames, ordered true, colors [red, lime, blue, white], image 35 320x240 3 planes red`.

Not done: libavc (an H.264 fallback where Media Foundation has none), MPEG-4
part 2/H.263 software fallbacks, VP9/AV1 10-bit output as 10-bit (decoded, but
shifted to 8 bits), MatroskaExtractor's audio tracks beyond what the existing
decoders take (Vorbis and Opus in WebM go to the existing decoders; not tested).
