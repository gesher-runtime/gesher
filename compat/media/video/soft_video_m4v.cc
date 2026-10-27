// Gesher media: MPEG-4 part 2 and H.263 through AOSP's m4v_h263 decoder
// (frameworks/av media/module/codecs/m4v_h263/dec; video_decoder.h; media3,
// ADR 0059), where Media Foundation has no decoder for them. As
// C2SoftMpeg4Dec drives it: PVInitVideoDecoder with the VOL header (csd-0;
// none for H.263, whose short header carries the size), then a VOP header and
// body per sample into one of two frame buffers (the other is the reference).
// No B-frames: pictures come out in decode order, which is display order.

#include <algorithm>
#include <cstring>

#include "mp4dec_api.h"

#include "../audio/log.h"
#include "video_decoder.h"

namespace gesher::media {
namespace {

constexpr int Align16(int v) { return (v + 15) & ~15; }

class M4vDecoder : public VideoDecoder {
 public:
  ~M4vDecoder() override {
    if (initialized_) PVCleanUpVideoDecoder(&ctl_);
  }

  std::string Name() const override {
    return mpeg4_ ? "soft mpeg4 (m4v_h263)" : "soft h263 (m4v_h263)";
  }

  bool Init(const Format& f, bool mpeg4, std::string* error) {
    mpeg4_ = mpeg4;
    int32_t w = 0, h = 0;
    f.GetInt32("width", &w);
    f.GetInt32("height", &h);
    // The largest picture the buffers take (C2SoftMpeg4Dec's max size).
    max_w_ = std::max(w > 0 ? w : 352, 352);
    max_h_ = std::max(h > 0 ? h : 288, 288);
    f.GetBuffer("csd-0", &vol_);
    memset(&ctl_, 0, sizeof(ctl_));
    if (!InitDecoder(vol_)) {
      *error = Name() + ": PVInitVideoDecoder failed (unsupported content?)";
      return false;
    }
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    if (in.data.empty()) return true;
    std::vector<uint8_t> data = in.data;
    // A VOL header in the stream starts the decoder over with it.
    if (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 && data[3] == 0xB0) {
      PVCleanUpVideoDecoder(&ctl_);
      initialized_ = false;
      vol_ = data;
      return InitDecoder(vol_);
    }
    if (!initialized_ && !InitDecoder(mpeg4_ ? vol_ : std::vector<uint8_t>{})) return false;
    const size_t frame = static_cast<size_t>(Align16(max_w_)) * Align16(max_h_) * 3 / 2;
    for (auto& b : frames_) {
      if (b.size() < frame) b.assign(frame, 0);
    }
    if (!refs_set_) {
      PVSetReferenceYUV(&ctl_, frames_[1].data());
      refs_set_ = true;
    }
    uint8_t* bitstream = data.data();
    int32_t size = static_cast<int32_t>(data.size());
    uint32_t timestamp = static_cast<uint32_t>(count_);
    uint32_t use_ext = 1;
    VopHeaderInfo header = {};
    uint8_t* cur = frames_[count_ & 1].data();
    if (PVDecodeVopHeader(&ctl_, &bitstream, &timestamp, &size, &header, &use_ext, cur) != PV_TRUE) {
      MEDIA_LOGE("%s: the VOP header did not decode", Name().c_str());
      return false;
    }
    if (SizeChanged()) {
      // H.263's size is in each picture's short header: start over at the new size.
      PVCleanUpVideoDecoder(&ctl_);
      initialized_ = false;
      if (!InitDecoder(mpeg4_ ? vol_ : std::vector<uint8_t>{})) return false;
      return Decode(in, out);
    }
    if (PVDecodeVopBody(&ctl_, &size) != PV_TRUE) {
      MEDIA_LOGE("%s: the VOP body did not decode", Name().c_str());
      return false;
    }
    // The picture: width x height inside buffers of 16-aligned strides.
    const int stride = Align16(width_), vstride = Align16(height_);
    VideoFrame f;
    f.Allocate(width_, height_);
    f.time_us = in.time_us;
    const uint8_t* sy = cur;
    const uint8_t* su = cur + static_cast<size_t>(vstride) * stride;
    const uint8_t* sv = su + static_cast<size_t>(vstride / 2) * (stride / 2);
    for (int r = 0; r < height_; r++) {
      memcpy(f.data.data() + static_cast<size_t>(r) * f.stride_y, sy + static_cast<size_t>(r) * stride,
             static_cast<size_t>(width_));
    }
    uint8_t* du = f.data.data() + static_cast<size_t>(f.stride_y) * f.height;
    uint8_t* dv = du + static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
    const int cw = (width_ + 1) / 2, ch = (height_ + 1) / 2;
    for (int r = 0; r < ch; r++) {
      memcpy(du + static_cast<size_t>(r) * f.stride_uv, su + static_cast<size_t>(r) * (stride / 2),
             static_cast<size_t>(cw));
      memcpy(dv + static_cast<size_t>(r) * f.stride_uv, sv + static_cast<size_t>(r) * (stride / 2),
             static_cast<size_t>(cw));
    }
    out->push_back(std::move(f));
    count_++;
    return true;
  }

  void Flush() override {
    // After a seek the next sample is a key frame: the references start over.
    if (initialized_) PVResetVideoDecoder(&ctl_);
    refs_set_ = false;
    count_ = 0;
  }

  void Drain(std::vector<VideoFrame>*) override {}

 private:
  bool InitDecoder(const std::vector<uint8_t>& vol) {
    std::vector<uint8_t> copy = vol;
    uint8_t* vol_data[1] = {copy.empty() ? nullptr : copy.data()};
    int32_t vol_size = static_cast<int32_t>(copy.size());
    if (!PVInitVideoDecoder(&ctl_, vol_data, &vol_size, 1, max_w_, max_h_,
                            mpeg4_ ? MPEG4_MODE : H263_MODE)) {
      return false;
    }
    if (PVGetDecBitstreamMode(&ctl_) != (mpeg4_ ? MPEG4_MODE : H263_MODE)) {
      MEDIA_LOGE("%s: the stream is not %s", Name().c_str(), mpeg4_ ? "MPEG-4" : "H.263");
      PVCleanUpVideoDecoder(&ctl_);
      return false;
    }
    initialized_ = true;
    PVSetPostProcType(&ctl_, 0);
    refs_set_ = false;
    SizeChanged();
    return true;
  }

  // The display size now; true when it changed since the last call (frames sized anew).
  bool SizeChanged() {
    int32_t w = 0, h = 0;
    PVGetVideoDimensions(&ctl_, &w, &h);
    if (w <= 0 || h <= 0 || (w == width_ && h == height_)) return false;
    const bool had = width_ > 0;
    // Buffers sized for a smaller picture, or a new size mid-stream: start over.
    const bool grow = w > max_w_ || h > max_h_;
    width_ = w;
    height_ = h;
    max_w_ = std::max(max_w_, w);
    max_h_ = std::max(max_h_, h);
    if (!had && !grow) return false;
    for (auto& b : frames_) b.clear();
    refs_set_ = false;
    return true;
  }

  VideoDecControls ctl_;
  bool mpeg4_ = true;
  bool initialized_ = false, refs_set_ = false;
  int max_w_ = 352, max_h_ = 288;
  int width_ = 0, height_ = 0;
  std::vector<uint8_t> vol_;
  std::vector<uint8_t> frames_[2];
  int64_t count_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoder> CreateSoftM4vDecoder(const Format& track, bool mpeg4,
                                                   std::string* error) {
  auto d = std::make_unique<M4vDecoder>();
  if (!d->Init(track, mpeg4, error)) return nullptr;
  return d;
}

}  // namespace gesher::media
