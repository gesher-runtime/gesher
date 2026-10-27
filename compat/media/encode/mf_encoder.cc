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
IMFTransform* FindEncoder(const GUID& category, const GUID& major, const GUID& subtype) {
  const MfApi& mf = Mf();
  if (!mf.ok) return nullptr;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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

IMFSample* NewSample(const uint8_t* data, size_t size, int64_t time_us, int64_t duration_us) {
  const MfApi& mf = Mf();
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(mf.CreateMemoryBuffer(static_cast<DWORD>(std::max<size_t>(size, 1)), &buf))) return nullptr;
  BYTE* p = nullptr;
  buf->Lock(&p, nullptr, nullptr);
  if (size > 0) memcpy(p, data, size);
  buf->Unlock();
  buf->SetCurrentLength(static_cast<DWORD>(size));
  IMFSample* s = nullptr;
  mf.CreateSample(&s);
  s->AddBuffer(buf);
  buf->Release();
  s->SetSampleTime(time_us * 10);
  s->SetSampleDuration(duration_us * 10);
  return s;
}

// Pulls every output the MFT has now; `emit` takes each sample's bytes, time and clean point.
template <typename Emit>
bool PullAll(IMFTransform* t, DWORD out_size, const char* name, Emit emit) {
  const MfApi& mf = Mf();
  MFT_OUTPUT_STREAM_INFO si = {};
  t->GetOutputStreamInfo(0, &si);
  const bool provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  const DWORD size = std::max<DWORD>(si.cbSize, out_size);
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER db = {};
    IMFSample* s = nullptr;
    if (!provides) {
      IMFMediaBuffer* b = nullptr;
      mf.CreateMemoryBuffer(size, &b);
      mf.CreateSample(&s);
      s->AddBuffer(b);
      b->Release();
      db.pSample = s;
    }
    DWORD status = 0;
    HRESULT hr = t->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents != nullptr) db.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (s != nullptr) s->Release();
      return true;
    }
    if (FAILED(hr)) {
      if (s != nullptr) s->Release();
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
      MEDIA_LOGE("%s: ProcessOutput %s", name, Hex(hr).c_str());
      return false;
    }
    if (db.pSample != nullptr) {
      LONGLONG time = 0;
      db.pSample->GetSampleTime(&time);
      UINT32 clean = 0;
      db.pSample->GetUINT32(kSampleCleanPoint, &clean);
      IMFMediaBuffer* b = nullptr;
      if (SUCCEEDED(db.pSample->ConvertToContiguousBuffer(&b))) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
          emit(p, static_cast<size_t>(len), static_cast<int64_t>(time / 10), clean != 0);
          b->Unlock();
        }
        b->Release();
      }
      db.pSample->Release();
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

void SetCodecApi(IMFTransform* t, const GUID& api, uint32_t value) {
  CodecApi* c = nullptr;
  if (FAILED(t->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(&c))) || c == nullptr) return;
  VARIANT v;
  memset(&v, 0, sizeof(v));
  v.vt = VT_UI4;
  v.ulVal = value;
  c->SetValue(&api, &v);
  c->Release();
}

// --- H.264 --------------------------------------------------------------------------

class MfH264Encoder : public VideoEncoder {
 public:
  ~MfH264Encoder() override {
    if (t_ != nullptr) t_->Release();
  }

  std::string Name() const override { return "mf avc encoder"; }

  bool Init(const VideoEncoderParams& p, std::string* error) {
    p_ = p;
    t_ = FindEncoder(kCategoryVideoEncoder, kMediaTypeVideo, kH264);
    if (t_ == nullptr) {
      *error = "Media Foundation has no H.264 encoder";
      return false;
    }
    const MfApi& mf = Mf();
    const UINT64 size = (static_cast<UINT64>(p.width) << 32) | static_cast<UINT32>(p.height);
    const UINT64 rate = (static_cast<UINT64>(std::max(p.frame_rate, 1)) << 32) | 1;
    IMFMediaType* out = nullptr;
    mf.CreateMediaType(&out);
    out->SetGUID(kMtMajorType, kMediaTypeVideo);
    out->SetGUID(kMtSubtype, kH264);
    out->SetUINT32(kMtAvgBitrate, static_cast<UINT32>(std::max(p.bitrate, 64000)));
    out->SetUINT64(kMtFrameSize, size);
    out->SetUINT64(kMtFrameRate, rate);
    out->SetUINT64(kMtPixelAspect, (static_cast<UINT64>(1) << 32) | 1);
    out->SetUINT32(kMtInterlaceMode, 2 /* progressive */);
    out->SetUINT32(kMtMpeg2Profile, p.profile == 8 ? 100 : p.profile == 2 ? 77 : 66);
    HRESULT hr = t_->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr)) {
      *error = "H.264 encoder: SetOutputType " + Hex(hr);
      return false;
    }
    IMFMediaType* in = nullptr;
    mf.CreateMediaType(&in);
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

  bool Encode(const VideoFrame& f, bool key, std::vector<EncodedPacket>* out) override {
    // The visible part, tightly packed I420.
    const int w = p_.width, h = p_.height, cw = w / 2, ch = h / 2;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3 / 2);
    CopyI420(f, w, h, buf.data());
    if (key) SetCodecApi(t_, kApiForceKeyFrame, 1);
    IMFSample* s = NewSample(buf.data(), buf.size(), f.time_us, 1000000 / std::max(p_.frame_rate, 1));
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
    (void)cw;
    (void)ch;
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
    const DWORD size = static_cast<DWORD>(p_.width * p_.height * 2 + 4096);
    return PullAll(t_, size, "mf avc encoder", [&](const uint8_t* p, size_t n, int64_t time_us, bool clean) {
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
  bool config_sent_ = false;
};

// --- AAC ----------------------------------------------------------------------------

class MfAacEncoder : public AudioEncoder {
 public:
  ~MfAacEncoder() override {
    if (t_ != nullptr) t_->Release();
  }

  std::string Name() const override { return "mf aac encoder"; }

  bool Init(const AudioEncoderParams& p, std::string* error) {
    p_ = p;
    if (p.sample_rate != 44100 && p.sample_rate != 48000) {
      *error = "Media Foundation's AAC encoder takes 44100 or 48000 Hz, not " + std::to_string(p.sample_rate);
      return false;
    }
    t_ = FindEncoder(kCategoryAudioEncoder, kMediaTypeAudio, kAac);
    if (t_ == nullptr) {
      *error = "Media Foundation has no AAC encoder";
      return false;
    }
    const MfApi& mf = Mf();
    IMFMediaType* in = nullptr;
    mf.CreateMediaType(&in);
    in->SetGUID(kMtMajorType, kMediaTypeAudio);
    in->SetGUID(kMtSubtype, kPcm);
    in->SetUINT32(kMtAudioBits, 16);
    in->SetUINT32(kMtAudioRate, p.sample_rate);
    in->SetUINT32(kMtAudioChannels, p.channels);
    in->SetUINT32(kMtAudioBlockAlign, 2 * p.channels);
    in->SetUINT32(kMtAudioBytesPerSecond, 2 * p.channels * p.sample_rate);
    HRESULT hr = t_->SetInputType(0, in, 0);
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
    mf.CreateMediaType(&out);
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
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool Encode(const int16_t* pcm, size_t frames, int64_t time_us, std::vector<EncodedPacket>* out) override {
    if (!config_sent_) {
      EncodedPacket c;
      c.data = AacAudioSpecificConfig(2, p_.sample_rate, p_.channels);
      c.config = true;
      c.time_us = time_us;
      out->push_back(std::move(c));
      config_sent_ = true;
    }
    if (frames == 0) return true;
    const int64_t duration = static_cast<int64_t>(frames) * 1000000 / p_.sample_rate;
    IMFSample* s = NewSample(reinterpret_cast<const uint8_t*>(pcm), frames * 2 * p_.channels, time_us, duration);
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
    return PullAll(t_, 8192, "mf aac encoder", [&](const uint8_t* p, size_t n, int64_t time_us, bool) {
      EncodedPacket pkt;
      pkt.data.assign(p, p + n);
      pkt.time_us = time_us;
      pkt.key = true;
      out->push_back(std::move(pkt));
    });
  }

  AudioEncoderParams p_;
  IMFTransform* t_ = nullptr;
  bool config_sent_ = false;
};

}  // namespace

bool MfH264EncoderAvailable() {
  static const bool has = Has(kCategoryVideoEncoder, kMediaTypeVideo, kH264);
  return has;
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
