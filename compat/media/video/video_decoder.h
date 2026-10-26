// Gesher media: video decoders (docs/media.md "Video", ADR 0051).
//
// A decoder takes a video track's samples (Annex B NAL units for H.264 and
// H.265, as AOSP's MPEG4Extractor gives them; frames for VP8/VP9/AV1/MPEG-4)
// and gives frames in presentation order as 8-bit I420 (three planes, 4:2:0),
// whatever the backend produces (Media Foundation's NV12, a software
// decoder's I420). Decoders: Media Foundation's (mf_video.cc) where Windows
// has one for the codec, AOSP's software decoders otherwise (soft_video.cc).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../player/extractor.h"

namespace gesher::media {

// One decoded picture, I420: Y (stride_y a row), then U and V (stride_uv a
// row, each (height + 1) / 2 rows). width x height is the buffer; the
// visible part is crop_* (inclusive right/bottom, as MediaFormat's crop keys).
struct VideoFrame {
  int width = 0, height = 0;
  int stride_y = 0, stride_uv = 0;
  int crop_left = 0, crop_top = 0, crop_right = -1, crop_bottom = -1;
  int64_t time_us = 0;
  std::vector<uint8_t> data;  // Y, U, V, contiguous

  int visible_width() const { return crop_right - crop_left + 1; }
  int visible_height() const { return crop_bottom - crop_top + 1; }
  const uint8_t* y() const { return data.data(); }
  const uint8_t* u() const { return data.data() + static_cast<size_t>(stride_y) * height; }
  const uint8_t* v() const {
    return u() + static_cast<size_t>(stride_uv) * ((height + 1) / 2);
  }
  // Allocates the planes for w x h with tight strides; crop = all of it.
  void Allocate(int w, int h);
};

// The codecs (MediaFormat MIME types) and how they are decoded here.
struct VideoCodecInfo {
  std::string mime;
  std::string backend;     // "mf" (Media Foundation) or "soft"
  bool hardware = false;   // Media Foundation's decoder is a hardware MFT
  int max_width = 0, max_height = 0;
};

class VideoDecoder {
 public:
  virtual ~VideoDecoder() = default;

  // A decoder for `track` (its "mime", "width", "height", "csd-*"); null
  // with `error` when there is none on this machine.
  static std::unique_ptr<VideoDecoder> Create(const Format& track, std::string* error);
  // The video decoders this machine has, one per MIME type.
  static const std::vector<VideoCodecInfo>& Available();
  static bool Supports(const std::string& mime);

  virtual std::string Name() const = 0;
  // Decodes one access unit; frames that come out are appended to `out`.
  // False on an error the stream cannot go on from.
  virtual bool Decode(const Sample& in, std::vector<VideoFrame>* out) = 0;
  // After a seek: drop what is inside.
  virtual void Flush() = 0;
  // At the end of the stream: the frames still inside.
  virtual void Drain(std::vector<VideoFrame>* out) = 0;
};

// The codec-specific data of a track as the bytes a decoder is fed first:
// H.264 (csd-avc, the avcC box, or csd-0/csd-1) and H.265 (csd-hevc, or
// csd-0) as Annex B parameter sets; empty for the others.
std::vector<uint8_t> AnnexBParameterSets(const Format& track, const std::string& mime);
// avcC / hvcC to Annex B parameter sets (as MediaExtractor's csd-0/csd-1 are):
// for avcC, `sps` and `pps` separately.
bool AvccToAnnexB(const uint8_t* p, size_t n, std::vector<uint8_t>* sps, std::vector<uint8_t>* pps);
bool HvccToAnnexB(const uint8_t* p, size_t n, std::vector<uint8_t>* out);
// An ESDS (MPEG4Extractor's "esds": MPEG-4 part 2's) to its DecoderSpecificInfo, and
// that as the track's csd-0 where it has none (as NuMediaExtractor gives it).
bool EsdsCodecSpecificInfo(const uint8_t* p, size_t n, std::vector<uint8_t>* out);
void AddCsdFromEsds(const Format& track);

// Media Foundation (mf_video.cc).
std::unique_ptr<VideoDecoder> CreateMfVideoDecoder(const Format& track, std::string* error);
// The MF decoders present, by MIME type.
std::vector<VideoCodecInfo> MfVideoCodecs();

// AOSP's software decoders (soft_video.cc).
std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format& track, std::string* error);
std::vector<VideoCodecInfo> SoftVideoCodecs();
// libavc's H.264 decoder (soft_video_avc.cc); MPEG-4 part 2 (`mpeg4`) or H.263
// through the m4v_h263 decoder (soft_video_m4v.cc).
std::unique_ptr<VideoDecoder> CreateSoftAvcDecoder(const Format& track, std::string* error);
std::unique_ptr<VideoDecoder> CreateSoftM4vDecoder(const Format& track, bool mpeg4,
                                                   std::string* error);

// I420 to RGBA_8888 (R in the low byte), BT.601 limited range, the visible
// part of `f` scaled (nearest, or as given) to dst_w x dst_h.
void I420ToRgba(const VideoFrame& f, uint32_t* dst, int dst_stride, int dst_w, int dst_h);
// NV12 (Y plane, interleaved UV) to the frame's I420 planes.
void Nv12ToI420(const uint8_t* src, int src_stride, int src_height, VideoFrame* f);
// RGBA_8888 (R in the low byte; src_w x src_h, stride == src_w) to the frame's
// I420 planes at its size (nearest neighbour), BT.601 limited range: an input
// Surface's frames for an encoder.
void RgbaToI420(const uint32_t* src, int src_w, int src_h, VideoFrame* f);

// Draws `f` (its visible part, as RGBA_8888) into a Surface's ANativeWindow
// (render.cc): a window's, a SurfaceTexture's or a SurfaceView's.
}  // namespace gesher::media
struct ANativeWindow;
namespace gesher::media {
bool RenderFrame(ANativeWindow* window, const VideoFrame& f);

}  // namespace gesher::media
