// Gesher media: Media Foundation's encoders (encoder.h; ADR 0059): the H.264
// encoder MFT (in system memory: I420 in, Annex B out; low latency, so each
// frame comes out as it goes in) and the AAC encoder MFT (16-bit PCM in, raw
// AAC access units out). Loaded at run time, as the decoders are
// (compat/media/video/mf_video.cc).

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../audio/log.h"
#include "encoder.h"

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
const GUID kMediaTypeAudio = FourCcSubtype(FourCc('a', 'u', 'd', 's'));
const GUID kH264 = FourCcSubtype(FourCc('H', '2', '6', '4'));
const GUID kI420 = FourCcSubtype(FourCc('I', '4', '2', '0'));
const GUID kAac = FourCcSubtype(0x1610);
const GUID kPcm = FourCcSubtype(0x0001);
const GUID kMtMajorType = {0x48EBA18E, 0xF8C9, 0x4687, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
const GUID kMtSubtype = {0xF7E34C9A, 0x42E8, 0x4714, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
const GUID kMtFrameSize = {0x1652C33D, 0xD6B2, 0x4012, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
const GUID kMtFrameRate = {0xC459A2E8, 0x3D2C, 0x4E44, {0xB1, 0x32, 0xFE, 0xE5, 0x15, 0x6C, 0x7B, 0xB0}};
const GUID kMtPixelAspect = {0xC6376A1E, 0x8D0A, 0x4027, {0xBE, 0x45, 0x6D, 0x9A, 0x0A, 0xD3, 0x9B, 0xB6}};
const GUID kMtInterlaceMode = {0xE2724BB8, 0xE676, 0x4806, {0xB4, 0xB2, 0xA8, 0xD6, 0xEF, 0xB4, 0x4C, 0xCD}};
const GUID kMtAvgBitrate = {0x20332624, 0xFB0D, 0x4D9E, {0xBD, 0x0D, 0xCB, 0xF6, 0x78, 0x6C, 0x10, 0x2E}};
const GUID kMtMpeg2Profile = {0xAD76A80B, 0x2D5C, 0x4E0B, {0xB3, 0x75, 0x64, 0xE5, 0x20, 0x13, 0x70, 0x36}};
const GUID kMtDefaultStride = {0x644B4E48, 0x1E02, 0x4516, {0xB0, 0xEB, 0xC0, 0x1C, 0xA9, 0xD4, 0x9A, 0xC6}};
const GUID kMtAudioChannels = {0x37E48BF5, 0x645E, 0x4C5B, {0x89, 0xDE, 0xAD, 0xA9, 0xE2, 0x9B, 0x69, 0x6A}};
const GUID kMtAudioRate = {0x5FAEEAE7, 0x0290, 0x4C31, {0x9E, 0x8A, 0xC5, 0x34, 0xF6, 0x8D, 0x9D, 0xBA}};
const GUID kMtAudioBits = {0xF2DEB57F, 0x40FA, 0x4764, {0xAA, 0x33, 0xED, 0x4F, 0x2D, 0x1F, 0xF6, 0x69}};
const GUID kMtAudioBytesPerSecond = {0x1AAB75C8, 0xCFEF, 0x451C, {0xAB, 0x95, 0xAC, 0x03, 0x4B, 0x8E, 0x17, 0x31}};
const GUID kMtAudioBlockAlign = {0x322DE230, 0x9EEB, 0x43BD, {0xAB, 0x7A, 0xFF, 0x41, 0x22, 0x51, 0x54, 0x1D}};
const GUID kMtAacPayloadType = {0xBFBABE79, 0x7434, 0x4D1C, {0x94, 0xF0, 0x72, 0xA3, 0xB9, 0xE1, 0x71, 0x88}};
const GUID kSampleCleanPoint = {0x9CDF01D8, 0xA0F0, 0x43BA, {0xB0, 0x77, 0xEA, 0xA0, 0x6C, 0xBD, 0x72, 0x8A}};
const GUID kCategoryVideoEncoder = {0xF79EAC7D, 0xE545, 0x4387, {0xBD, 0xEE, 0xD6, 0x47, 0xD7, 0xBD, 0xE4, 0x2A}};
const GUID kCategoryAudioEncoder = {0x91C64BD0, 0xF91E, 0x4D8C, {0x92, 0x76, 0xDB, 0x24, 0x82, 0x79, 0xD9, 0x75}};
const IID kIidTransform = {0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}};
const IID kIidCodecApi = {0x901DB4C7, 0x31CE, 0x41A2, {0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA}};
const GUID kApiGopSize = {0x95F31B26, 0x95A4, 0x41AA, {0x93, 0x03, 0x24, 0x6A, 0x7F, 0xC6, 0xEE, 0xF1}};
const GUID kApiForceKeyFrame = {0x398C1B98, 0x8353, 0x475A, {0x9E, 0xF2, 0x8F, 0x26, 0x5D, 0x26, 0x03, 0x45}};
const GUID kApiLowLatency = {0x9C27891A, 0xED7A, 0x40E1, {0x88, 0xE8, 0xB2, 0x27, 0x27, 0xA0, 0x24, 0xEE}};
const GUID kApiMeanBitrate = {0xF7222374, 0x2144, 0x4815, {0xB5, 0x50, 0xA3, 0x7F, 0x8E, 0x12, 0xEE, 0x52}};

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
           SUCCEEDED(a.Startup(0x20070 /* MF_VERSION */, 0));
    return a;
  }();
  return api;
}

std::string Hex(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
  return b;
}

// The software encoder MFTs producing `subtype`, activated in turn until one is made.
IMFTransform* FindEncoder(const GUID& category, const GUID& major, const GUID& subtype,
                          bool initialize_com = true) {
  const MfApi& mf = Mf();
  if (!mf.ok) return nullptr;
  if (initialize_com) CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFT_REGISTER_TYPE_INFO outInfo = {major, subtype};
  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  if (FAILED(mf.TEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                        nullptr, &outInfo, &acts, &count))) {
    return nullptr;
  }
  IMFTransform* t = nullptr;
  for (UINT32 i = 0; i < count; i++) {
    if (t == nullptr) acts[i]->ActivateObject(kIidTransform, reinterpret_cast<void**>(&t));
    acts[i]->Release();
  }
  CoTaskMemFree(acts);
  return t;
}

bool Has(const GUID& category, const GUID& major, const GUID& subtype) {
  IMFTransform* t = FindEncoder(category, major, subtype);
  if (t == nullptr) return false;
  t->Release();
  return true;
}

// Fill the sample's own memory before publishing it to the transform. The MFT
// may retain an input sample after ProcessInput, so every input owns its buffer.
template <typename Fill>
IMFSample* NewSample(size_t size, int64_t time_us, int64_t duration_us, Fill fill) {
  const MfApi& mf = Mf();
  if (!mf.ok || size > std::numeric_limits<DWORD>::max() ||
      time_us > INT64_MAX / 10 || time_us < INT64_MIN / 10 ||
      duration_us > INT64_MAX / 10 || duration_us < INT64_MIN / 10) return nullptr;
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(mf.CreateMemoryBuffer(static_cast<DWORD>(std::max<size_t>(size, 1)), &buf))) return nullptr;
  if (buf == nullptr) return nullptr;
  BYTE* p = nullptr;
  DWORD capacity = 0;
  HRESULT hr = buf->Lock(&p, &capacity, nullptr);
  if (SUCCEEDED(hr)) {
    if (p == nullptr || capacity < size) {
      hr = E_FAIL;
    } else {
      fill(p);
      hr = buf->SetCurrentLength(static_cast<DWORD>(size));
    }
    // Even failed filling/length checks must balance the successful Lock.
    const HRESULT unlocked = buf->Unlock();
    if (SUCCEEDED(hr)) hr = unlocked;
  }
  IMFSample* s = nullptr;
  if (SUCCEEDED(hr)) hr = mf.CreateSample(&s);
  if (SUCCEEDED(hr)) hr = s != nullptr ? s->AddBuffer(buf) : E_FAIL;
  buf->Release();
  if (SUCCEEDED(hr)) hr = s->SetSampleTime(time_us * 10);
  if (SUCCEEDED(hr)) hr = s->SetSampleDuration(duration_us * 10);
  if (FAILED(hr)) {
    if (s != nullptr) s->Release();
    return nullptr;
  }
  return s;
}

IMFSample* NewSample(const uint8_t* data, size_t size, int64_t time_us, int64_t duration_us) {
  return NewSample(size, time_us, duration_us, [&](uint8_t* dst) {
    if (size != 0) memcpy(dst, data, size);
  });
}

// The input sample and the existing output-size estimate both use DWORD byte
// counts. Widen before multiplication and reject an unrepresentable layout.
bool I420SampleSize(int w, int h, size_t* size) {
  if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;
  const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
  if (pixels > (std::numeric_limits<DWORD>::max() - 4096u) / 2) return false;
  *size = static_cast<size_t>(pixels + pixels / 2);
  return true;
}

bool I420FrameFits(const VideoFrame& f, int w, int h) {
  if (f.width <= 0 || f.height <= 0 || f.height == INT32_MAX ||
      f.stride_y < f.width || f.stride_uv < (static_cast<int64_t>(f.width) + 1) / 2 ||
      f.crop_left < 0 || f.crop_top < 0 || f.crop_left > f.width || f.crop_top > f.height ||
      w > f.width - f.crop_left || h > f.height - f.crop_top) return false;
  const uint64_t y = static_cast<uint64_t>(f.stride_y) * f.height;
  const uint64_t uv = static_cast<uint64_t>(f.stride_uv) * ((static_cast<uint64_t>(f.height) + 1) / 2);
  return y + 2 * uv <= f.data.size();
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

// Pulls every output the MFT has now; `emit` takes each sample's bytes, time and clean point.
template <typename Emit>
bool PullAll(IMFTransform* t, DWORD out_size, OutputSampleCache* cache, const char* name, Emit emit) {
  const MfApi& mf = Mf();
  if (!mf.ok) return false;
  MFT_OUTPUT_STREAM_INFO si = {};
  HRESULT info_hr = t->GetOutputStreamInfo(0, &si);
  if (FAILED(info_hr)) {
    MEDIA_LOGE("%s: GetOutputStreamInfo %s", name, Hex(info_hr).c_str());
    return false;
  }
  bool provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  DWORD size = std::max<DWORD>(si.cbSize, out_size);
  if (provides) cache->Reset();
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER db = {};
    IMFSample* provided = nullptr;
    if (!provides) {
      provided = cache->Acquire(mf, size);
      if (provided == nullptr) {
        MEDIA_LOGE("%s: output sample allocation failed", name);
        return false;
      }
      db.pSample = provided;
    }
    DWORD status = 0;
    HRESULT hr = t->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents != nullptr) db.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
      // GetOutputStreamInfo only describes buffer requirements; it does not
      // acknowledge a type change. Keep the configured encoding format, and
      // report failure if the transform can no longer accept it.
      IMFMediaType* current = nullptr;
      HRESULT type_hr = t->GetOutputCurrentType(0, &current);
      if (SUCCEEDED(type_hr)) {
        type_hr = current != nullptr ? t->SetOutputType(0, current, 0) : E_FAIL;
      }
      if (current != nullptr) current->Release();
      if (FAILED(type_hr)) {
        cache->Reset();
        MEDIA_LOGE("%s: output type change %s", name, Hex(type_hr).c_str());
        return false;
      }
      si = {};
      info_hr = t->GetOutputStreamInfo(0, &si);
      if (FAILED(info_hr)) {
        cache->Reset();
        MEDIA_LOGE("%s: GetOutputStreamInfo %s", name, Hex(info_hr).c_str());
        return false;
      }
      provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      size = std::max<DWORD>(si.cbSize, out_size);
      if (provides) cache->Reset();
      continue;
    }
    if (FAILED(hr)) {
      if (db.pSample != nullptr && db.pSample != provided) db.pSample->Release();
      cache->Reset();
      MEDIA_LOGE("%s: ProcessOutput %s", name, Hex(hr).c_str());
      return false;
    }
    if (db.pSample != nullptr) {
      LONGLONG time = 0;
      db.pSample->GetSampleTime(&time);
      UINT32 clean = 0;
      db.pSample->GetUINT32(kSampleCleanPoint, &clean);
      IMFMediaBuffer* b = nullptr;
      if (SUCCEEDED(db.pSample->ConvertToContiguousBuffer(&b)) && b != nullptr) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
          if (p != nullptr || len == 0) {
            emit(p, static_cast<size_t>(len), static_cast<int64_t>(time / 10), clean != 0);
          }
          b->Unlock();
        }
        b->Release();
      } else if (b != nullptr) {
        b->Release();
      }
      if (db.pSample != provided) db.pSample->Release();
    }
  }
}

// ICodecAPI's first methods (codecapi.h), as far as SetValue.
struct CodecApi : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) = 0;
  virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) = 0;
};

bool SetCodecApi(IMFTransform* t, const GUID& api, uint32_t value, bool require_modifiable = false) {
  CodecApi* c = nullptr;
  if (FAILED(t->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(&c))) || c == nullptr) return false;
  VARIANT v;
  memset(&v, 0, sizeof(v));
  v.vt = VT_UI4;
  v.ulVal = value;
  const bool applied = (!require_modifiable || c->IsModifiable(&api) == S_OK) &&
                       c->SetValue(&api, &v) == S_OK;
  c->Release();
  return applied;
}

// --- H.264 --------------------------------------------------------------------------

class MfH264Encoder : public VideoEncoder {
 public:
  ~MfH264Encoder() override {
    out_cache_.Reset();
    if (t_ != nullptr) t_->Release();
  }

  std::string Name() const override { return "mf avc encoder"; }

  bool Init(const VideoEncoderParams& p, std::string* error, bool probe = false) {
    size_t size_bytes;
    if (!I420SampleSize(p.width, p.height, &size_bytes)) {
      *error = "H.264 encoder: unsupported buffer size";
      return false;
    }
    p_ = p;
    t_ = FindEncoder(kCategoryVideoEncoder, kMediaTypeVideo, kH264, !probe);
    if (t_ == nullptr) {
      *error = "Media Foundation has no H.264 encoder";
      return false;
    }
    const MfApi& mf = Mf();
    const UINT64 size = (static_cast<UINT64>(p.width) << 32) | static_cast<UINT32>(p.height);
    const UINT64 rate = (static_cast<UINT64>(std::max(p.frame_rate, 1)) << 32) | 1;
    IMFMediaType* out = nullptr;
    HRESULT hr = mf.CreateMediaType(&out);
    if (FAILED(hr) || out == nullptr) {
      if (out != nullptr) out->Release();
      *error = "H.264 encoder: CreateMediaType " + Hex(FAILED(hr) ? hr : E_FAIL);
      return false;
    }
    out->SetGUID(kMtMajorType, kMediaTypeVideo);
    out->SetGUID(kMtSubtype, kH264);
    out->SetUINT32(kMtAvgBitrate, static_cast<UINT32>(std::max(p.bitrate, 64000)));
    out->SetUINT64(kMtFrameSize, size);
    out->SetUINT64(kMtFrameRate, rate);
    out->SetUINT64(kMtPixelAspect, (static_cast<UINT64>(1) << 32) | 1);
    out->SetUINT32(kMtInterlaceMode, 2 /* progressive */);
    const UINT32 profile = p.profile == kAvcProfileHigh ? 100 :
                           p.profile == kAvcProfileMain ? 77 : 66;
    out->SetUINT32(kMtMpeg2Profile, profile);
    hr = t_->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr)) {
      *error = "H.264 encoder: SetOutputType " + Hex(hr);
      return false;
    }
    IMFMediaType* in = nullptr;
    hr = mf.CreateMediaType(&in);
    if (FAILED(hr) || in == nullptr) {
      if (in != nullptr) in->Release();
      *error = "H.264 encoder: CreateMediaType " + Hex(FAILED(hr) ? hr : E_FAIL);
      return false;
    }
    in->SetGUID(kMtMajorType, kMediaTypeVideo);
    in->SetGUID(kMtSubtype, kI420);
    in->SetUINT64(kMtFrameSize, size);
    in->SetUINT64(kMtFrameRate, rate);
    in->SetUINT64(kMtPixelAspect, (static_cast<UINT64>(1) << 32) | 1);
    in->SetUINT32(kMtInterlaceMode, 2);
    in->SetUINT32(kMtDefaultStride, static_cast<UINT32>(p.width));
    hr = t_->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
      *error = "H.264 encoder: SetInputType (I420) " + Hex(hr);
      return false;
    }
    if (probe) return true;  // Both media types negotiated. Do not start streaming.
    SetCodecApi(t_, kApiLowLatency, 1);
    if (p.i_frame_interval_s > 0) {
      SetCodecApi(t_, kApiGopSize, static_cast<uint32_t>(std::max(1.0f, p.i_frame_interval_s * p.frame_rate)));
    } else {
      SetCodecApi(t_, kApiGopSize, 1);
    }
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool SetBitrate(int bitrate) override {
    return bitrate > 0 && SetCodecApi(t_, kApiMeanBitrate, static_cast<uint32_t>(bitrate), true);
  }

  bool Encode(const VideoFrame& f, bool key, std::vector<EncodedPacket>* out) override {
    // Copy the visible I420 rows directly into the independently owned sample.
    const int w = p_.width, h = p_.height;
    size_t size;
    if (!I420SampleSize(w, h, &size) || !I420FrameFits(f, w, h)) return false;
    IMFSample* s = NewSample(size, f.time_us, 1000000 / std::max(p_.frame_rate, 1),
                            [&](uint8_t* dst) { CopyI420(f, w, h, dst); });
    if (s == nullptr) return false;
    if (key) SetCodecApi(t_, kApiForceKeyFrame, 1);
    HRESULT hr = t_->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!Pull(out)) {
        s->Release();
        return false;
      }
      hr = t_->ProcessInput(0, s, 0);
    }
    s->Release();
    if (FAILED(hr)) {
      MEDIA_LOGE("%s: ProcessInput %s", Name().c_str(), Hex(hr).c_str());
      return false;
    }
    return Pull(out);
  }

  bool Drain(std::vector<EncodedPacket>* out) override {
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    t_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return Pull(out);
  }

  static void CopyI420(const VideoFrame& f, int w, int h, uint8_t* dst) {
    const int cw = w / 2, ch = h / 2;
    for (int y = 0; y < h; y++) {
      memcpy(dst + static_cast<size_t>(y) * w,
             f.y() + static_cast<size_t>(f.crop_top + y) * f.stride_y + f.crop_left, static_cast<size_t>(w));
    }
    uint8_t* u = dst + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(cw) * ch;
    for (int y = 0; y < ch; y++) {
      const size_t row = static_cast<size_t>(f.crop_top / 2 + y) * f.stride_uv + f.crop_left / 2;
      memcpy(u + static_cast<size_t>(y) * cw, f.u() + row, static_cast<size_t>(cw));
      memcpy(v + static_cast<size_t>(y) * cw, f.v() + row, static_cast<size_t>(cw));
    }
  }

 private:
  bool Pull(std::vector<EncodedPacket>* out) {
    const DWORD size = static_cast<DWORD>(static_cast<uint64_t>(p_.width) * p_.height * 2 + 4096);
    return PullAll(t_, size, &out_cache_, "mf avc encoder", [&](const uint8_t* p, size_t n, int64_t time_us, bool clean) {
      // The parameter sets go first, as the codec-config buffer; the frames without them.
      std::vector<uint8_t> config, frame;
      bool idr = false;
      for (auto [nal, len] : SplitAnnexB(p, n)) {
        if (len == 0) continue;
        const int type = nal[0] & 0x1f;
        std::vector<uint8_t>* to = (type == 7 || type == 8) ? &config : &frame;
        if (type == 9) continue;  // access unit delimiters
        if (type == 5) idr = true;
        static const uint8_t kStart[] = {0, 0, 0, 1};
        to->insert(to->end(), kStart, kStart + 4);
        to->insert(to->end(), nal, nal + len);
      }
      if (!config.empty() && !config_sent_) {
        EncodedPacket c;
        c.data = std::move(config);
        c.config = true;
        c.time_us = time_us;
        out->push_back(std::move(c));
        config_sent_ = true;
      }
      if (!frame.empty()) {
        EncodedPacket pkt;
        pkt.data = std::move(frame);
        pkt.time_us = time_us;
        pkt.key = clean || idr;
        out->push_back(std::move(pkt));
      }
    });
  }

  VideoEncoderParams p_;
  IMFTransform* t_ = nullptr;
  OutputSampleCache out_cache_;
  bool config_sent_ = false;
};

// --- AAC ----------------------------------------------------------------------------

class MfAacEncoder : public AudioEncoder {
 public:
  ~MfAacEncoder() override {
    out_cache_.Reset();
    if (t_ != nullptr) t_->Release();
  }

  std::string Name() const override { return "mf aac encoder"; }
  int ConfiguredBitrate() const override { return p_.bitrate; }

  bool Init(const AudioEncoderParams& p, std::string* error) {
    if (p.aac_profile != 2) {
      *error = "Media Foundation AAC encoder implements LC only";
      return false;
    }
    p_ = p;
    if (p.sample_rate != 44100 && p.sample_rate != 48000) {
      *error = "Media Foundation's AAC encoder takes 44100 or 48000 Hz, not " + std::to_string(p.sample_rate);
      return false;
    }
    if (p.channels == 0 ||
        static_cast<uint64_t>(p.channels) * 2 * p.sample_rate > std::numeric_limits<UINT32>::max()) {
      *error = "Media Foundation AAC encoder: invalid channel count";
      return false;
    }
    t_ = FindEncoder(kCategoryAudioEncoder, kMediaTypeAudio, kAac);
    if (t_ == nullptr) {
      *error = "Media Foundation has no AAC encoder";
      return false;
    }
    const MfApi& mf = Mf();
    IMFMediaType* in = nullptr;
    HRESULT hr = mf.CreateMediaType(&in);
    if (FAILED(hr) || in == nullptr) {
      if (in != nullptr) in->Release();
      *error = "AAC encoder: CreateMediaType " + Hex(FAILED(hr) ? hr : E_FAIL);
      return false;
    }
    in->SetGUID(kMtMajorType, kMediaTypeAudio);
    in->SetGUID(kMtSubtype, kPcm);
    in->SetUINT32(kMtAudioBits, 16);
    in->SetUINT32(kMtAudioRate, p.sample_rate);
    in->SetUINT32(kMtAudioChannels, p.channels);
    in->SetUINT32(kMtAudioBlockAlign, 2 * p.channels);
    in->SetUINT32(kMtAudioBytesPerSecond, 2 * p.channels * p.sample_rate);
    hr = t_->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
      *error = "AAC encoder: SetInputType " + Hex(hr);
      return false;
    }
    // Its bitrates: 96, 128, 160 or 192 kb/s.
    static const UINT32 kBytes[] = {12000, 16000, 20000, 24000};
    UINT32 bytes = kBytes[0];
    for (UINT32 b : kBytes) {
      if (static_cast<int>(b * 8) <= std::max(p.bitrate, 96000)) bytes = b;
    }
    IMFMediaType* out = nullptr;
    hr = mf.CreateMediaType(&out);
    if (FAILED(hr) || out == nullptr) {
      if (out != nullptr) out->Release();
      *error = "AAC encoder: CreateMediaType " + Hex(FAILED(hr) ? hr : E_FAIL);
      return false;
    }
    out->SetGUID(kMtMajorType, kMediaTypeAudio);
    out->SetGUID(kMtSubtype, kAac);
    out->SetUINT32(kMtAudioBits, 16);
    out->SetUINT32(kMtAudioRate, p.sample_rate);
    out->SetUINT32(kMtAudioChannels, p.channels);
    out->SetUINT32(kMtAudioBytesPerSecond, bytes);
    out->SetUINT32(kMtAacPayloadType, 0);  // raw access units
    hr = t_->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr)) {
      *error = "AAC encoder: SetOutputType " + Hex(hr);
      return false;
    }
    p_.bitrate = static_cast<int>(bytes * 8);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool Encode(const int16_t* pcm, size_t frames, int64_t time_us, std::vector<EncodedPacket>* out) override {
    if (p_.channels == 0 || p_.channels > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        p_.sample_rate == 0 || p_.sample_rate > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    const uint64_t bytes_per_frame = static_cast<uint64_t>(p_.channels) * 2;
    if (frames > 0 &&
        (pcm == nullptr ||
         frames > std::numeric_limits<DWORD>::max() / bytes_per_frame ||
         frames > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / 1000000ULL)) {
      return false;
    }
    if (!config_sent_) {
      EncodedPacket c;
      c.data = AacAudioSpecificConfig(2, p_.sample_rate, p_.channels);
      c.config = true;
      c.time_us = time_us;
      out->push_back(std::move(c));
      config_sent_ = true;
    }
    if (frames == 0) return true;
    const size_t byte_size = static_cast<size_t>(frames) * static_cast<size_t>(bytes_per_frame);
    const int64_t duration = static_cast<int64_t>((static_cast<uint64_t>(frames) * 1000000ULL) / p_.sample_rate);
    IMFSample* s = NewSample(reinterpret_cast<const uint8_t*>(pcm), byte_size, time_us, duration);
    if (s == nullptr) return false;
    HRESULT hr = t_->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (!Pull(out)) {
        s->Release();
        return false;
      }
      hr = t_->ProcessInput(0, s, 0);
    }
    s->Release();
    if (FAILED(hr)) {
      MEDIA_LOGE("%s: ProcessInput %s", Name().c_str(), Hex(hr).c_str());
      return false;
    }
    return Pull(out);
  }

  bool Drain(std::vector<EncodedPacket>* out) override {
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    t_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    return Pull(out);
  }

  size_t frame_size() const override { return 1024; }

 private:
  bool Pull(std::vector<EncodedPacket>* out) {
    return PullAll(t_, 8192, &out_cache_, "mf aac encoder", [&](const uint8_t* p, size_t n, int64_t time_us, bool) {
      EncodedPacket pkt;
      pkt.data.assign(p, p + n);
      pkt.time_us = time_us;
      pkt.key = true;
      out->push_back(std::move(pkt));
    });
  }

  AudioEncoderParams p_;
  IMFTransform* t_ = nullptr;
  OutputSampleCache out_cache_;
  bool config_sent_ = false;
};

}  // namespace

int MfH264EncoderProfiles() {
  static const int profiles = [] {
    // Activating a transform does not show that it accepts our I420 input.
    // Test the same type negotiation as configure() once per process, at 1080p30.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return 0;
    VideoEncoderParams params;
    params.width = 1920;
    params.height = 1080;
    int negotiated = 0;
    for (int profile : {kAvcProfileBaseline, kAvcProfileMain, kAvcProfileHigh}) {
      params.profile = profile;
      MfH264Encoder probe;
      std::string error;
      if (probe.Init(params, &error, true)) negotiated |= profile;
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return negotiated;
  }();
  return profiles;
}

bool MfAacEncoderAvailable() {
  static const bool has = Has(kCategoryAudioEncoder, kMediaTypeAudio, kAac);
  return has;
}

std::unique_ptr<VideoEncoder> CreateMfH264Encoder(const VideoEncoderParams& p, std::string* error) {
  auto e = std::make_unique<MfH264Encoder>();
  if (!e->Init(p, error)) return nullptr;
  return e;
}

std::unique_ptr<AudioEncoder> CreateMfAacEncoder(const AudioEncoderParams& p, std::string* error) {
  auto e = std::make_unique<MfAacEncoder>();
  if (!e->Init(p, error)) return nullptr;
  return e;
}

}  // namespace gesher::media
