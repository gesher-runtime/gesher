// Gesher media: video decoder selection, codec-specific data and pixel
// conversions. See video_decoder.h.
#include "video_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <media/NdkMediaFormat.h>

#include "../audio/log.h"

namespace gesher::media {

void VideoFrame::Allocate(int w, int h) {
  width = w;
  height = h;
  stride_y = w;
  stride_uv = (w + 1) / 2;
  crop_left = crop_top = 0;
  crop_right = w - 1;
  crop_bottom = h - 1;
  data.assign(static_cast<size_t>(stride_y) * h + 2 * static_cast<size_t>(stride_uv) * ((h + 1) / 2),
              0);
}

const std::vector<VideoCodecInfo>& VideoDecoder::Available() {
  static const std::vector<VideoCodecInfo> codecs = [] {
    // GESHER_VIDEO_DECODERS=soft: the software decoders only (as on a Windows
    // edition without Media Foundation's decoders; the tests' way to check them).
    const char* only = getenv("GESHER_VIDEO_DECODERS");
    const bool soft_only = only != nullptr && std::string(only) == "soft";
    std::vector<VideoCodecInfo> all;
    if (!soft_only) all = MfVideoCodecs();
    for (const VideoCodecInfo& s : SoftVideoCodecs()) {
      bool have = std::any_of(all.begin(), all.end(),
                              [&](const VideoCodecInfo& c) { return c.mime == s.mime; });
      if (!have) all.push_back(s);
    }
    std::string list;
    for (const VideoCodecInfo& c : all) {
      list += (list.empty() ? "" : ", ") + c.mime + " (" + c.backend + (c.hardware ? ", hardware" : "") + ")";
    }
    MEDIA_LOGI("video decoders: %s", list.empty() ? "none" : list.c_str());
    return all;
  }();
  return codecs;
}

bool VideoDecoder::Supports(const std::string& mime) {
  for (const VideoCodecInfo& c : Available()) {
    if (c.mime == mime) return true;
  }
  return false;
}

std::unique_ptr<VideoDecoder> VideoDecoder::Create(const Format& track, std::string* error) {
  std::string mime = track.Mime();
  AddCsdFromEsds(track);
  for (const VideoCodecInfo& c : Available()) {
    if (c.mime != mime) continue;
    std::unique_ptr<VideoDecoder> d = c.backend == "mf" ? CreateMfVideoDecoder(track, error)
                                                         : CreateSoftVideoDecoder(track, error);
    if (d != nullptr) return d;
    // Media Foundation refused the stream: a software decoder may still take it.
    if (c.backend == "mf") {
      std::string soft_error;
      for (const VideoCodecInfo& s : SoftVideoCodecs()) {
        if (s.mime == mime) {
          d = CreateSoftVideoDecoder(track, &soft_error);
          if (d != nullptr) return d;
        }
      }
    }
    return nullptr;
  }
  *error = "no video decoder for " + mime;
  return nullptr;
}

// --- Codec-specific data ------------------------------------------------------------------

bool EsdsCodecSpecificInfo(const uint8_t* p, size_t n, std::vector<uint8_t>* out) {
  // ES_Descriptor (3) > DecoderConfigDescriptor (4) > DecoderSpecificInfo (5).
  size_t i = 0;
  auto length = [&](size_t* len) {
    *len = 0;
    for (int k = 0; k < 4 && i < n; k++) {
      uint8_t b = p[i++];
      *len = (*len << 7) | (b & 0x7f);
      if ((b & 0x80) == 0) return true;
    }
    return i <= n;
  };
  while (i < n) {
    const uint8_t tag = p[i++];
    size_t len;
    if (!length(&len)) return false;
    if (tag == 3) {
      if (i + 3 > n) return false;
      const uint8_t flags = p[i + 2];
      i += 3;
      if (flags & 0x80) i += 2;                     // streamDependenceFlag
      if ((flags & 0x40) && i < n) i += 1 + p[i];   // URL_Flag
      if (flags & 0x20) i += 2;                     // OCRstreamFlag
    } else if (tag == 4) {
      i += 13;
    } else if (tag == 5) {
      if (i + len > n) return false;
      out->assign(p + i, p + i + len);
      return true;
    } else {
      i += len;
    }
  }
  return false;
}

void AddCsdFromEsds(const Format& track) {
  std::vector<uint8_t> have, esds, csd;
  if (track.GetBuffer("csd-0", &have) || !track.GetBuffer("esds", &esds)) return;
  if (EsdsCodecSpecificInfo(esds.data(), esds.size(), &csd) && !csd.empty()) {
    AMediaFormat_setBuffer(track.get(), "csd-0", csd.data(), csd.size());
  }
}

namespace {

void AppendNal(std::vector<uint8_t>* out, const uint8_t* p, size_t n) {
  static const uint8_t kStart[] = {0, 0, 0, 1};
  out->insert(out->end(), kStart, kStart + 4);
  out->insert(out->end(), p, p + n);
}

bool StartsWithStartCode(const std::vector<uint8_t>& b) {
  return (b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) ||
         (b.size() >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1);
}

}  // namespace

bool AvccToAnnexB(const uint8_t* p, size_t n, std::vector<uint8_t>* sps, std::vector<uint8_t>* pps) {
  // AVCDecoderConfigurationRecord: version, profile, compat, level, 6 bits +
  // lengthSizeMinusOne, 3 bits + numSPS, SPSs, numPPS, PPSs.
  if (n < 7 || p[0] != 1) return false;
  size_t off = 5;
  int count = p[off++] & 0x1f;
  for (int i = 0; i < count; i++) {
    if (off + 2 > n) return false;
    size_t len = (static_cast<size_t>(p[off]) << 8) | p[off + 1];
    off += 2;
    if (off + len > n) return false;
    AppendNal(sps, p + off, len);
    off += len;
  }
  if (off + 1 > n) return false;
  count = p[off++];
  for (int i = 0; i < count; i++) {
    if (off + 2 > n) return false;
    size_t len = (static_cast<size_t>(p[off]) << 8) | p[off + 1];
    off += 2;
    if (off + len > n) return false;
    AppendNal(pps, p + off, len);
    off += len;
  }
  return !sps->empty();
}

bool HvccToAnnexB(const uint8_t* p, size_t n, std::vector<uint8_t>* out) {
  // HEVCDecoderConfigurationRecord: 22 bytes, numOfArrays, then arrays of
  // (type, numNalus, (length, NAL)...).
  if (n < 23) return false;
  size_t off = 22;
  int arrays = p[off++];
  for (int a = 0; a < arrays; a++) {
    if (off + 3 > n) return false;
    off++;  // array_completeness, NAL type
    int nalus = (p[off] << 8) | p[off + 1];
    off += 2;
    for (int i = 0; i < nalus; i++) {
      if (off + 2 > n) return false;
      size_t len = (static_cast<size_t>(p[off]) << 8) | p[off + 1];
      off += 2;
      if (off + len > n) return false;
      AppendNal(out, p + off, len);
      off += len;
    }
  }
  return !out->empty();
}

std::vector<uint8_t> AnnexBParameterSets(const Format& track, const std::string& mime) {
  std::vector<uint8_t> out, b;
  if (mime == "video/avc") {
    if (track.GetBuffer("csd-avc", &b) && !b.empty()) {
      std::vector<uint8_t> sps, pps;
      if (AvccToAnnexB(b.data(), b.size(), &sps, &pps)) {
        out = sps;
        out.insert(out.end(), pps.begin(), pps.end());
        return out;
      }
    }
    for (const char* key : {"csd-0", "csd-1"}) {
      b.clear();
      if (!track.GetBuffer(key, &b) || b.empty()) continue;
      if (StartsWithStartCode(b)) {
        out.insert(out.end(), b.begin(), b.end());
      } else if (std::string(key) == "csd-0") {
        std::vector<uint8_t> sps, pps;
        if (AvccToAnnexB(b.data(), b.size(), &sps, &pps)) {
          out.insert(out.end(), sps.begin(), sps.end());
          out.insert(out.end(), pps.begin(), pps.end());
        }
      }
    }
  } else if (mime == "video/hevc") {
    if (track.GetBuffer("csd-hevc", &b) && HvccToAnnexB(b.data(), b.size(), &out)) return out;
    b.clear();
    if (track.GetBuffer("csd-0", &b) && !b.empty()) {
      if (StartsWithStartCode(b)) {
        out = b;
      } else {
        HvccToAnnexB(b.data(), b.size(), &out);
      }
    }
  }
  return out;
}

// --- Pixels -------------------------------------------------------------------------------

namespace {

inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

void I420ToRgba(const VideoFrame& f, uint32_t* dst, int dst_stride, int dst_w, int dst_h) {
  const int vw = std::max(f.visible_width(), 1), vh = std::max(f.visible_height(), 1);
  const uint8_t* yp = f.y();
  const uint8_t* up = f.u();
  const uint8_t* vp = f.v();
  const bool same = dst_w == vw && dst_h == vh;
  for (int y = 0; y < dst_h; y++) {
    int sy = f.crop_top + (same ? y : static_cast<int>(static_cast<int64_t>(y) * vh / dst_h));
    const uint8_t* yr = yp + static_cast<size_t>(sy) * f.stride_y;
    const uint8_t* ur = up + static_cast<size_t>(sy / 2) * f.stride_uv;
    const uint8_t* vr = vp + static_cast<size_t>(sy / 2) * f.stride_uv;
    uint32_t* d = dst + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < dst_w; x++) {
      int sx = f.crop_left + (same ? x : static_cast<int>(static_cast<int64_t>(x) * vw / dst_w));
      // BT.601, limited range, 10-bit fixed point.
      int c = (yr[sx] - 16) * 1192;
      int u = ur[sx / 2] - 128;
      int v = vr[sx / 2] - 128;
      int r = (c + 1634 * v + 512) >> 10;
      int g = (c - 833 * v - 400 * u + 512) >> 10;
      int b = (c + 2066 * u + 512) >> 10;
      d[x] = 0xff000000u | (static_cast<uint32_t>(Clamp8(b)) << 16) |
             (static_cast<uint32_t>(Clamp8(g)) << 8) | Clamp8(r);
    }
  }
}

void RgbaToI420(const uint32_t* src, int src_w, int src_h, VideoFrame* f) {
  const int w = f->width, h = f->height;
  if (src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0) return;
  uint8_t* yp = f->data.data();
  uint8_t* up = yp + static_cast<size_t>(f->stride_y) * h;
  uint8_t* vp = up + static_cast<size_t>(f->stride_uv) * ((h + 1) / 2);
  auto at = [&](int x, int y) {
    const int sx = static_cast<int>(static_cast<int64_t>(x) * src_w / w);
    const int sy = static_cast<int>(static_cast<int64_t>(y) * src_h / h);
    return src[static_cast<size_t>(sy) * src_w + sx];
  };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint32_t c = at(x, y);
      const int r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
      yp[static_cast<size_t>(y) * f->stride_y + x] = Clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    }
  }
  for (int y = 0; y < (h + 1) / 2; y++) {
    for (int x = 0; x < (w + 1) / 2; x++) {
      const uint32_t c = at(std::min(2 * x, w - 1), std::min(2 * y, h - 1));
      const int r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
      up[static_cast<size_t>(y) * f->stride_uv + x] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      vp[static_cast<size_t>(y) * f->stride_uv + x] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
}

void Nv12ToI420(const uint8_t* src, int src_stride, int src_height, VideoFrame* f) {
  for (int y = 0; y < f->height; y++) {
    memcpy(f->data.data() + static_cast<size_t>(y) * f->stride_y,
           src + static_cast<size_t>(y) * src_stride, static_cast<size_t>(f->width));
  }
  const uint8_t* uv = src + static_cast<size_t>(src_stride) * src_height;
  uint8_t* u = f->data.data() + static_cast<size_t>(f->stride_y) * f->height;
  uint8_t* v = u + static_cast<size_t>(f->stride_uv) * ((f->height + 1) / 2);
  const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
  for (int y = 0; y < ch; y++) {
    const uint8_t* s = uv + static_cast<size_t>(y) * src_stride;
    uint8_t* ur = u + static_cast<size_t>(y) * f->stride_uv;
    uint8_t* vr = v + static_cast<size_t>(y) * f->stride_uv;
    for (int x = 0; x < cw; x++) {
      ur[x] = s[2 * x];
      vr[x] = s[2 * x + 1];
    }
  }
}

}  // namespace gesher::media
