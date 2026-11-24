// Gesher media: video decoding through Media Foundation's decoders
// (video_decoder.h, ADR 0051).
//
// The synchronous decoder MFTs Windows has for a codec (H.264, H.265, VP8,
// VP9, AV1, MPEG-4 part 2, H.263: which ones depends on the edition and the
// codec extensions installed), in system memory: the input type is the
// codec, the output NV12, converted to the frames' I420. Media Foundation is
// loaded at run time (Windows "N" editions and Server without the feature
// have none; then only the software decoders remain).

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI  // mfapi.h names GDI's BITMAPINFOHEADER
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../audio/log.h"
#include "video_decoder.h"

namespace gesher::media {
namespace {

GUID FourCcSubtype(uint32_t fourcc) {
  return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}
constexpr uint32_t FourCc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

const GUID kMediaTypeVideo = FourCcSubtype(FourCc('v', 'i', 'd', 's'));
const GUID kNv12 = FourCcSubtype(FourCc('N', 'V', '1', '2'));
const GUID kMtMajorType = {0x48EBA18E, 0xF8C9, 0x4687, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
const GUID kMtSubtype = {0xF7E34C9A, 0x42E8, 0x4714, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
const GUID kMtFrameSize = {0x1652C33D, 0xD6B2, 0x4012, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
const GUID kMtInterlaceMode = {0xE2724BB8, 0xE676, 0x4806, {0xB4, 0xB2, 0xA8, 0xD6, 0xEF, 0xB4, 0x4C, 0xCD}};
const GUID kMtDefaultStride = {0x644B4E48, 0x1E02, 0x4516, {0xB0, 0xEB, 0xC0, 0x1C, 0xA9, 0xD4, 0x9A, 0xC6}};
const GUID kMtMinimumDisplayAperture = {0xD7388766, 0x18FE, 0x48C6, {0xA1, 0x77, 0xEE, 0x89, 0x48, 0x67, 0xC8, 0xC4}};
const GUID kMtMpegSequenceHeader = {0x3C036DE7, 0x3AD0, 0x4C9E, {0x92, 0x16, 0xEE, 0x6D, 0x6A, 0xC2, 0x1C, 0xB3}};
const GUID kSampleDiscontinuity = {0x9CDF01D9, 0xA0F0, 0x43BA, {0xB0, 0x77, 0xEA, 0xA0, 0x6C, 0xBD, 0x72, 0x8A}};
const GUID kCategoryVideoDecoder = {0xD6C02D4B, 0x6833, 0x45B4, {0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91}};
const IID kIidTransform = {0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}};
const IID kIid2DBuffer = {0x7DC9D5F9, 0x9ED9, 0x44EC, {0x9B, 0xBF, 0x06, 0x00, 0xBB, 0x58, 0x9F, 0xBB}};

// The codecs, by MediaFormat MIME type.
const struct {
  const char* mime;
  uint32_t fourcc;
} kCodecs[] = {
    {"video/avc", FourCc('H', '2', '6', '4')},
    {"video/hevc", FourCc('H', 'E', 'V', 'C')},
    {"video/x-vnd.on2.vp8", FourCc('V', 'P', '8', '0')},
    {"video/x-vnd.on2.vp9", FourCc('V', 'P', '9', '0')},
    {"video/av01", FourCc('A', 'V', '0', '1')},
    {"video/mp4v-es", FourCc('M', 'P', '4', 'V')},
    {"video/3gpp", FourCc('H', '2', '6', '3')},
};

struct MfApi {
  HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
  HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
  HRESULT(WINAPI* CreateSample)(IMFSample**) = nullptr;
  HRESULT(WINAPI* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
  HRESULT(WINAPI* TEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                           const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*) = nullptr;
  bool ok = false;
};

template <typename F>
void Load(HMODULE m, const char* name, F* f) {
  *f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const MfApi& Mf() {
  static MfApi api = [] {
    MfApi a;
    HMODULE m = LoadLibraryW(L"mfplat.dll");
    if (m == nullptr) return a;
    Load(m, "MFStartup", &a.Startup);
    Load(m, "MFCreateMediaType", &a.CreateMediaType);
    Load(m, "MFCreateSample", &a.CreateSample);
    Load(m, "MFCreateMemoryBuffer", &a.CreateMemoryBuffer);
    Load(m, "MFTEnumEx", &a.TEnumEx);
    a.ok = a.Startup != nullptr && a.CreateMediaType != nullptr && a.CreateSample != nullptr &&
           a.CreateMemoryBuffer != nullptr && a.TEnumEx != nullptr &&
           SUCCEEDED(a.Startup(0x20070 /* MF_VERSION */, 0 /* MFSTARTUP_FULL */));
    return a;
  }();
  return api;
}

// The decoder MFTs for `fourcc` (synchronous, in-process), activated in turn
// until one is made; null if there is none.
IMFTransform* FindDecoder(uint32_t fourcc, bool* any) {
  const MfApi& mf = Mf();
  if (any != nullptr) *any = false;
  if (!mf.ok) return nullptr;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFT_REGISTER_TYPE_INFO in = {kMediaTypeVideo, FourCcSubtype(fourcc)};
  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  if (FAILED(mf.TEnumEx(kCategoryVideoDecoder,
                        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        &in, nullptr, &acts, &count))) {
    return nullptr;
  }
  IMFTransform* t = nullptr;
  for (UINT32 i = 0; i < count; i++) {
    if (t == nullptr) acts[i]->ActivateObject(kIidTransform, reinterpret_cast<void**>(&t));
    acts[i]->Release();
  }
  CoTaskMemFree(acts);
  if (any != nullptr) *any = count > 0;
  return t;
}

std::string Hex(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
  return b;
}

struct OutputSampleCache {
  IMFSample* sample = nullptr;
  IMFMediaBuffer* buffer = nullptr;
  DWORD capacity = 0;

  OutputSampleCache() = default;
  ~OutputSampleCache() { Reset(); }
  OutputSampleCache(const OutputSampleCache&) = delete;
  OutputSampleCache& operator=(const OutputSampleCache&) = delete;

  void Reset() {
    if (sample != nullptr) {
      sample->Release();
      sample = nullptr;
    }
    if (buffer != nullptr) {
      buffer->Release();
      buffer = nullptr;
    }
    capacity = 0;
  }

  IMFSample* Acquire(const MfApi& mf, DWORD min_size) {
    if (!mf.ok) return nullptr;
    const DWORD wanted = std::max<DWORD>(min_size, 1);
    if (sample != nullptr && buffer != nullptr && capacity >= wanted) {
      DWORD count = 0;
      bool ok = sample->GetBufferCount(&count) == S_OK && count == 1;
      if (ok) {
        IMFMediaBuffer* b0 = nullptr;
        ok = sample->GetBufferByIndex(0, &b0) == S_OK && b0 == buffer;
        if (b0 != nullptr) b0->Release();
      }
      if (ok) {
        ok = SUCCEEDED(buffer->SetCurrentLength(0)) &&
             SUCCEEDED(sample->DeleteAllItems()) &&
             SUCCEEDED(sample->SetSampleTime(0)) &&
             SUCCEEDED(sample->SetSampleDuration(0));
      }
      if (ok) return sample;
    }
    Reset();
    IMFMediaBuffer* b = nullptr;
    if (FAILED(mf.CreateMemoryBuffer(wanted, &b)) || b == nullptr) {
      if (b != nullptr) b->Release();
      return nullptr;
    }
    IMFSample* s = nullptr;
    if (FAILED(mf.CreateSample(&s)) || s == nullptr) {
      if (s != nullptr) s->Release();
      b->Release();
      return nullptr;
    }
    if (FAILED(s->AddBuffer(b))) {
      s->Release();
      b->Release();
      return nullptr;
    }
    sample = s;
    buffer = b;
    capacity = wanted;
    return sample;
  }
};

class MfVideoDecoder : public VideoDecoder {
 public:
  ~MfVideoDecoder() override {
    out_cache_.Reset();
    if (t_ != nullptr) t_->Release();
  }

  std::string Name() const override { return "mf " + mime_; }

  bool Init(const Format& f, uint32_t fourcc, std::string* error) {
    mime_ = f.Mime();
    int32_t w = 0, h = 0;
    f.GetInt32("width", &w);
    f.GetInt32("height", &h);
    t_ = FindDecoder(fourcc, nullptr);
    if (t_ == nullptr) {
      *error = "Media Foundation has no decoder for " + mime_;
      return false;
    }
    const MfApi& mf = Mf();
    IMFMediaType* in = nullptr;
    HRESULT hr = mf.CreateMediaType(&in);
    if (FAILED(hr) || in == nullptr) {
      if (in != nullptr) in->Release();
      *error = mime_ + ": CreateMediaType " + Hex(FAILED(hr) ? hr : E_FAIL);
      return false;
    }
    in->SetGUID(kMtMajorType, kMediaTypeVideo);
    in->SetGUID(kMtSubtype, FourCcSubtype(fourcc));
    if (w > 0 && h > 0) in->SetUINT64(kMtFrameSize, (static_cast<UINT64>(w) << 32) | static_cast<UINT32>(h));
    in->SetUINT32(kMtInterlaceMode, 2 /* MFVideoInterlace_Progressive */);
    // MPEG-4 part 2's decoder configuration (its VOL header) and the like.
    std::vector<uint8_t> csd;
    if (mime_ != "video/avc" && mime_ != "video/hevc" && f.GetBuffer("csd-0", &csd) && !csd.empty()) {
      if (csd.size() > std::numeric_limits<UINT32>::max()) {
        in->Release();
        *error = mime_ + ": sequence header exceeds UINT32";
        return false;
      }
      in->SetBlob(kMtMpegSequenceHeader, csd.data(), static_cast<UINT32>(csd.size()));
    }
    hr = t_->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
      *error = mime_ + ": Media Foundation's decoder refuses the stream (" + Hex(hr) + ")";
      return false;
    }
    // H.264/H.265: the parameter sets go in front of the first access unit.
    prefix_ = AnnexBParameterSets(f, mime_);
    // The MPEG-4 decoder needs the VOL in the elementary stream too; the
    // media type's sequence-header attribute alone does not initialize it.
    if (mime_ == "video/mp4v-es") f.GetBuffer("csd-0", &prefix_);
    if (!SetOutput(error)) {
      // Many decoders know their output only after the first frame (a stream
      // change); until then any type may be refused.
      error->clear();
    }
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override {
    const MfApi& mf = Mf();
    if (!mf.ok || in.time_us > INT64_MAX / 10 || in.time_us < INT64_MIN / 10) return false;
    const bool use_prefix = !prefix_.empty() && !prefix_sent_;
    if (use_prefix) {
      if (prefix_.size() > std::numeric_limits<DWORD>::max() ||
          in.data.size() > std::numeric_limits<DWORD>::max() - prefix_.size()) {
        return false;
      }
    } else if (in.data.size() > std::numeric_limits<DWORD>::max()) {
      return false;
    }
    std::vector<uint8_t> bytes;
    const std::vector<uint8_t>* data = &in.data;
    if (use_prefix) {
      bytes.reserve(prefix_.size() + in.data.size());
      bytes = prefix_;
      bytes.insert(bytes.end(), in.data.begin(), in.data.end());
      data = &bytes;
    }
    if (data->empty()) return true;
    const DWORD size = static_cast<DWORD>(data->size());
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(mf.CreateMemoryBuffer(size, &buf)) || buf == nullptr) {
      if (buf != nullptr) buf->Release();
      return false;
    }
    BYTE* p = nullptr;
    DWORD cap = 0;
    HRESULT hr = buf->Lock(&p, &cap, nullptr);
    if (SUCCEEDED(hr)) {
      if (p == nullptr || cap < size) {
        hr = E_FAIL;
      } else {
        memcpy(p, data->data(), data->size());
        hr = buf->SetCurrentLength(size);
      }
      const HRESULT unlocked = buf->Unlock();
      if (SUCCEEDED(hr)) hr = unlocked;
    }
    IMFSample* sample = nullptr;
    if (SUCCEEDED(hr)) {
      hr = mf.CreateSample(&sample);
      if (SUCCEEDED(hr) && sample == nullptr) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    buf->Release();
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(in.time_us * 10);
    if (SUCCEEDED(hr) && discontinuity_) {
      hr = sample->SetUINT32(kSampleDiscontinuity, TRUE);
    }
    if (FAILED(hr)) {
      if (sample != nullptr) sample->Release();
      return false;
    }
    if (use_prefix) prefix_sent_ = true;
    if (discontinuity_) discontinuity_ = false;
    hr = t_->ProcessInput(0, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!Pull(out)) {
        sample->Release();
        return false;
      }
      hr = t_->ProcessInput(0, sample, 0);
    }
    sample->Release();
    if (FAILED(hr)) {
      MEDIA_LOGE("%s: ProcessInput %s", Name().c_str(), Hex(hr).c_str());
      return false;
    }
    return Pull(out);
  }

  void Flush() override {
    t_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    discontinuity_ = true;
    prefix_sent_ = false;
  }

  void Drain(std::vector<VideoFrame>* out) override {
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    t_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    Pull(out);
    // Ready for more input (after a flush and a new start).
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  }

 private:
  // NV12 output, with its size, stride and visible area.
  bool SetOutput(std::string* error) {
    IMFMediaType* chosen = nullptr;
    for (DWORD i = 0;; i++) {
      IMFMediaType* type = nullptr;
      if (FAILED(t_->GetOutputAvailableType(0, i, &type)) || type == nullptr) {
        if (type != nullptr) type->Release();
        break;
      }
      GUID sub = {};
      if (SUCCEEDED(type->GetGUID(kMtSubtype, &sub)) && IsEqualGUID(sub, kNv12)) {
        chosen = type;
        break;
      }
      type->Release();
    }
    if (chosen == nullptr) {
      *error = mime_ + ": the decoder offers no NV12 output";
      return false;
    }
    HRESULT hr = t_->SetOutputType(0, chosen, 0);
    if (FAILED(hr)) {
      chosen->Release();
      *error = mime_ + ": SetOutputType " + Hex(hr);
      return false;
    }
    UINT64 size = 0;
    chosen->GetUINT64(kMtFrameSize, &size);
    const int width = static_cast<int>(static_cast<uint32_t>(size >> 32));
    const int height = static_cast<int>(static_cast<uint32_t>(size & 0xffffffffu));
    UINT32 stride_attr = 0;
    const int stride = SUCCEEDED(chosen->GetUINT32(kMtDefaultStride, &stride_attr)) &&
                               static_cast<INT32>(stride_attr) > 0
                           ? static_cast<int>(static_cast<INT32>(stride_attr))
                           : width;
    MFVideoArea area = {};
    UINT32 got = 0;
    const bool has_area = SUCCEEDED(chosen->GetBlob(kMtMinimumDisplayAperture,
                                                    reinterpret_cast<UINT8*>(&area),
                                                    sizeof(area), &got)) &&
                          got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0;
    chosen->Release();
    if (width <= 0 || height <= 0 || stride < width) {
      *error = mime_ + ": invalid NV12 output geometry";
      return false;
    }
    const uint64_t nv12_bytes = static_cast<uint64_t>(stride) *
                                (static_cast<uint64_t>(height) + (static_cast<uint64_t>(height) + 1) / 2);
    if (nv12_bytes > std::numeric_limits<DWORD>::max()) {
      *error = mime_ + ": NV12 output buffer exceeds DWORD";
      return false;
    }
    MFT_OUTPUT_STREAM_INFO si = {};
    hr = t_->GetOutputStreamInfo(0, &si);
    if (FAILED(hr)) {
      *error = mime_ + ": GetOutputStreamInfo " + Hex(hr);
      return false;
    }
    width_ = width;
    height_ = height;
    stride_ = stride;
    if (has_area) {
      crop_left_ = std::max<int>(0, area.OffsetX.value);
      crop_top_ = std::max<int>(0, area.OffsetY.value);
      const int64_t right = static_cast<int64_t>(area.OffsetX.value) + area.Area.cx - 1;
      const int64_t bottom = static_cast<int64_t>(area.OffsetY.value) + area.Area.cy - 1;
      crop_right_ = static_cast<int>(std::clamp<int64_t>(right, crop_left_, width_ - 1));
      crop_bottom_ = static_cast<int>(std::clamp<int64_t>(bottom, crop_top_, height_ - 1));
    } else {
      crop_left_ = crop_top_ = 0;
      crop_right_ = width_ - 1;
      crop_bottom_ = height_ - 1;
    }
    provides_samples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    if (provides_samples_) out_cache_.Reset();
    out_size_ = std::max<DWORD>(si.cbSize, static_cast<DWORD>(nv12_bytes));
    output_set_ = true;
    MEDIA_LOGI("%s: NV12 %dx%d (stride %d), visible %d,%d-%d,%d", Name().c_str(), width_, height_,
               stride_, crop_left_, crop_top_, crop_right_, crop_bottom_);
    return true;
  }

  bool Pull(std::vector<VideoFrame>* out) {
    const MfApi& mf = Mf();
    if (!mf.ok) return false;
    for (;;) {
      if (!output_set_) {
        // Some decoders need an output type before they take input.
        std::string error;
        SetOutput(&error);
      }
      MFT_OUTPUT_DATA_BUFFER db = {};
      IMFSample* provided = nullptr;
      if (!provides_samples_ && output_set_) {
        provided = out_cache_.Acquire(mf, out_size_);
        if (provided == nullptr) {
          MEDIA_LOGE("%s: output sample allocation failed", Name().c_str());
          return false;
        }
        db.pSample = provided;
      }
      DWORD status = 0;
      HRESULT hr = t_->ProcessOutput(0, 1, &db, &status);
      if (db.pEvents != nullptr) db.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
        return true;
      }
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
        if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
        std::string error;
        if (!SetOutput(&error)) {
          out_cache_.Reset();
          MEDIA_LOGE("%s", error.c_str());
          return false;
        }
        continue;
      }
      if (FAILED(hr)) {
        if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
        out_cache_.Reset();
        MEDIA_LOGE("%s: ProcessOutput %s", Name().c_str(), Hex(hr).c_str());
        return false;
      }
      if (db.pSample != nullptr) {
        Emit(db.pSample, out);
        if (db.pSample != provided) db.pSample->Release();
      }
    }
  }

  void Emit(IMFSample* sample, std::vector<VideoFrame>* out) {
    if (width_ <= 0 || height_ <= 0 || stride_ < width_) return;
    LONGLONG t = 0;
    sample->GetSampleTime(&t);
    IMFMediaBuffer* b = nullptr;
    if (FAILED(sample->GetBufferByIndex(0, &b)) || b == nullptr) {
      if (b != nullptr) b->Release();
      return;
    }
    auto init_frame = [&](VideoFrame* f) {
      f->Allocate(width_, height_);
      f->crop_left = std::clamp(crop_left_, 0, width_ - 1);
      f->crop_top = std::clamp(crop_top_, 0, height_ - 1);
      f->crop_right = std::clamp(crop_right_, f->crop_left, width_ - 1);
      f->crop_bottom = std::clamp(crop_bottom_, f->crop_top, height_ - 1);
      f->time_us = t / 10;
    };
    VideoFrame f;
    bool converted = false;
    IMF2DBuffer* b2 = nullptr;
    if (SUCCEEDED(b->QueryInterface(kIid2DBuffer, reinterpret_cast<void**>(&b2))) && b2 != nullptr) {
      BYTE* p2 = nullptr;
      LONG pitch = 0;
      if (SUCCEEDED(b2->Lock2D(&p2, &pitch))) {
        if (p2 != nullptr && pitch >= static_cast<LONG>(width_)) {
          // A 2D buffer: its UV plane follows the Y plane's padded height, which
          // the frame size is (H.264's 16-row macroblocks).
          init_frame(&f);
          Nv12ToI420(p2, static_cast<int>(pitch), height_, &f);
          converted = true;
        }
        b2->Unlock2D();
      }
      b2->Release();
    }
    if (!converted) {
      BYTE* p = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
        const uint64_t needed = static_cast<uint64_t>(stride_) *
                                (static_cast<uint64_t>(height_) + (static_cast<uint64_t>(height_) + 1) / 2);
        if (p != nullptr && static_cast<uint64_t>(len) >= needed) {
          init_frame(&f);
          Nv12ToI420(p, stride_, height_, &f);
          converted = true;
        }
        b->Unlock();
      }
    }
    b->Release();
    if (converted) out->push_back(std::move(f));
  }

  IMFTransform* t_ = nullptr;
  OutputSampleCache out_cache_;
  std::string mime_;
  std::vector<uint8_t> prefix_;
  bool prefix_sent_ = false;
  bool discontinuity_ = false;
  bool output_set_ = false;
  bool provides_samples_ = false;
  DWORD out_size_ = 0;
  int width_ = 0, height_ = 0, stride_ = 0;
  int crop_left_ = 0, crop_top_ = 0, crop_right_ = -1, crop_bottom_ = -1;
};

}  // namespace

std::vector<VideoCodecInfo> MfVideoCodecs() {
  std::vector<VideoCodecInfo> out;
  for (const auto& c : kCodecs) {
    IMFTransform* t = FindDecoder(c.fourcc, nullptr);
    if (t == nullptr) continue;
    t->Release();
    VideoCodecInfo info;
    info.mime = c.mime;
    info.backend = "mf";
    info.hardware = false;  // system-memory decoding (no DXVA device given)
    info.max_width = 4096;
    info.max_height = 2304;
    out.push_back(info);
  }
  return out;
}

std::unique_ptr<VideoDecoder> CreateMfVideoDecoder(const Format& track, std::string* error) {
  std::string mime = track.Mime();
  for (const auto& c : kCodecs) {
    if (mime != c.mime) continue;
    auto d = std::make_unique<MfVideoDecoder>();
    if (!d->Init(track, c.fourcc, error)) return nullptr;
    return d;
  }
  *error = "Media Foundation: no codec for " + mime;
  return nullptr;
}

}  // namespace gesher::media
