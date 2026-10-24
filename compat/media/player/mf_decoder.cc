// Gesher media: AAC through Media Foundation's decoder (decoder.h, ADR 0042).
//
// AAC (LC, HE-AAC, HE-AACv2) is decoded by the MFT Windows licenses. Media
// Foundation is loaded at run time: Windows "N" editions and Server without
// the feature lack it, and then AAC is unsupported, not a missing DLL.

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI  // mmreg.h and mfapi.h name GDI's BITMAPINFOHEADER
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>

#include <cstring>
#include <mutex>

#include "../audio/log.h"
#include "decoder.h"

namespace gesher::media {
namespace {

GUID AudioSubtype(uint32_t d1) {
  return GUID{d1, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}
const GUID kMediaTypeAudio = AudioSubtype(0x73647561);
const GUID kAudioFormatAac = AudioSubtype(0x1610);
const GUID kAudioFormatPcm = AudioSubtype(0x0001);
const GUID kAudioFormatFloat = AudioSubtype(0x0003);
const GUID kMtMajorType = {0x48EBA18E, 0xF8C9, 0x4687, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
const GUID kMtSubtype = {0xF7E34C9A, 0x42E8, 0x4714, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
const GUID kMtSamplesPerSecond = {0x5FAEEAE7, 0x0290, 0x4C31, {0x9E, 0x8A, 0xC5, 0x34, 0xF6, 0x8D, 0x9D, 0xBA}};
const GUID kMtNumChannels = {0x37E48BF5, 0x645E, 0x4C5B, {0x89, 0xDE, 0xAD, 0xA9, 0xE2, 0x9B, 0x69, 0x6A}};
const GUID kMtBitsPerSample = {0xF2DEB57F, 0x40FA, 0x4764, {0xAA, 0x33, 0xED, 0x4F, 0x2D, 0x1F, 0xF6, 0x69}};
const GUID kMtAacPayloadType = {0xBFBABE79, 0x7434, 0x4D1C, {0x94, 0xF0, 0x72, 0xA3, 0xB9, 0xE1, 0x71, 0x88}};
const GUID kMtUserData = {0xB6BC765F, 0x4C3B, 0x40A4, {0xBD, 0x51, 0x25, 0x35, 0xB6, 0x6F, 0xE0, 0x9D}};
const GUID kCategoryAudioDecoder = {0x9EA73FB4, 0xEF7A, 0x4559, {0x8D, 0x5D, 0x71, 0x9D, 0x8F, 0x04, 0x26, 0xC7}};
const IID kIidTransform = {0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}};

// mfplat.dll, at run time.
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

// The first decoder for `subtype`, activated.
IMFTransform* FindDecoder(const GUID& subtype) {
  const MfApi& mf = Mf();
  if (!mf.ok) return nullptr;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFT_REGISTER_TYPE_INFO in = {kMediaTypeAudio, subtype};
  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  if (FAILED(mf.TEnumEx(kCategoryAudioDecoder, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                                   MFT_ENUM_FLAG_SORTANDFILTER,
                        &in, nullptr, &acts, &count))) {
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

class MfAacDecoder : public AudioDecoder {
 public:
  ~MfAacDecoder() override {
    if (t_ != nullptr) t_->Release();
  }

  bool Init(const Format& f, std::string* error) {
    int32_t rate = 0, channels = 0;
    f.GetInt32("sample-rate", &rate);
    f.GetInt32("channel-count", &channels);
    std::vector<uint8_t> asc;
    f.GetBuffer("csd-0", &asc);
    t_ = FindDecoder(kAudioFormatAac);
    if (t_ == nullptr) {
      *error = "AAC needs Media Foundation's decoder, which this Windows lacks";
      return false;
    }
    const MfApi& mf = Mf();
    IMFMediaType* in = nullptr;
    mf.CreateMediaType(&in);
    in->SetGUID(kMtMajorType, kMediaTypeAudio);
    in->SetGUID(kMtSubtype, kAudioFormatAac);
    in->SetUINT32(kMtSamplesPerSecond, static_cast<UINT32>(rate));
    in->SetUINT32(kMtNumChannels, static_cast<UINT32>(channels));
    in->SetUINT32(kMtAacPayloadType, 0);  // raw access units
    // HEAACWAVEINFO after its WAVEFORMATEX: payload type, profile level, struct
    // type, reserved; then the AudioSpecificConfig.
    std::vector<uint8_t> user(12, 0);
    user[2] = 0xFE;  // wAudioProfileLevelIndication: unknown
    user.insert(user.end(), asc.begin(), asc.end());
    in->SetBlob(kMtUserData, user.data(), static_cast<UINT32>(user.size()));
    HRESULT hr = t_->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
      *error = "AAC: the decoder refuses the stream (0x" + Hex(hr) + ")";
      return false;
    }
    if (!SetOutput(error)) return false;
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  PcmInfo info() const override { return info_; }

  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    const MfApi& mf = Mf();
    IMFMediaBuffer* buf = nullptr;
    IMFSample* sample = nullptr;
    if (FAILED(mf.CreateMemoryBuffer(static_cast<DWORD>(in.data.size()), &buf))) return false;
    BYTE* p = nullptr;
    buf->Lock(&p, nullptr, nullptr);
    memcpy(p, in.data.data(), in.data.size());
    buf->Unlock();
    buf->SetCurrentLength(static_cast<DWORD>(in.data.size()));
    mf.CreateSample(&sample);
    sample->AddBuffer(buf);
    sample->SetSampleTime(in.time_us * 10);
    buf->Release();
    HRESULT hr = t_->ProcessInput(0, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
      Pull(out);
      hr = t_->ProcessInput(0, sample, 0);
    }
    sample->Release();
    if (FAILED(hr)) {
      MEDIA_LOGE("AAC: ProcessInput 0x%08lx", static_cast<unsigned long>(hr));
      return false;
    }
    return Pull(out);
  }

  void Flush(int64_t) override { t_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }

  void Drain(std::vector<uint8_t>* out) override {
    t_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    Pull(out);
  }

 private:
  static std::string Hex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "%08lx", static_cast<unsigned long>(hr));
    return b;
  }

  // 16-bit PCM if offered, else float.
  bool SetOutput(std::string* error) {
    IMFMediaType* chosen = nullptr;
    for (DWORD i = 0;; i++) {
      IMFMediaType* type = nullptr;
      if (FAILED(t_->GetOutputAvailableType(0, i, &type))) break;
      GUID sub = {};
      UINT32 bits = 0;
      type->GetGUID(kMtSubtype, &sub);
      type->GetUINT32(kMtBitsPerSample, &bits);
      bool pcm16 = IsEqualGUID(sub, kAudioFormatPcm) && bits == 16;
      bool fl = IsEqualGUID(sub, kAudioFormatFloat);
      if (pcm16 || (fl && chosen == nullptr)) {
        if (chosen != nullptr) chosen->Release();
        chosen = type;
        if (pcm16) break;
      } else {
        type->Release();
      }
    }
    if (chosen == nullptr || FAILED(t_->SetOutputType(0, chosen, 0))) {
      if (chosen != nullptr) chosen->Release();
      *error = "AAC: no PCM output type";
      return false;
    }
    UINT32 rate = 0, channels = 0, bits = 0;
    GUID sub = {};
    chosen->GetUINT32(kMtSamplesPerSecond, &rate);
    chosen->GetUINT32(kMtNumChannels, &channels);
    chosen->GetUINT32(kMtBitsPerSample, &bits);
    chosen->GetGUID(kMtSubtype, &sub);
    chosen->Release();
    info_ = {rate, channels, IsEqualGUID(sub, kAudioFormatFloat) ? PcmFormat::kFloat : PcmFormat::kS16};
    MFT_OUTPUT_STREAM_INFO si = {};
    t_->GetOutputStreamInfo(0, &si);
    provides_samples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    out_size_ = std::max<DWORD>(si.cbSize, 8192 * 8 * 4);
    return true;
  }

  bool Pull(std::vector<uint8_t>* out) {
    const MfApi& mf = Mf();
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER db = {};
      IMFSample* s = nullptr;
      if (!provides_samples_) {
        IMFMediaBuffer* b = nullptr;
        mf.CreateMemoryBuffer(out_size_, &b);
        mf.CreateSample(&s);
        s->AddBuffer(b);
        b->Release();
        db.pSample = s;
      }
      DWORD status = 0;
      HRESULT hr = t_->ProcessOutput(0, 1, &db, &status);
      if (db.pEvents != nullptr) db.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (s != nullptr) s->Release();
        return true;
      }
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (s != nullptr) s->Release();
        std::string error;
        if (!SetOutput(&error)) return false;
        continue;
      }
      if (FAILED(hr)) {
        if (s != nullptr) s->Release();
        MEDIA_LOGE("AAC: ProcessOutput 0x%08lx", static_cast<unsigned long>(hr));
        return false;
      }
      IMFMediaBuffer* b = nullptr;
      if (db.pSample != nullptr && SUCCEEDED(db.pSample->ConvertToContiguousBuffer(&b))) {
        BYTE* p = nullptr;
        DWORD len = 0;
        b->Lock(&p, nullptr, &len);
        out->insert(out->end(), p, p + len);
        b->Unlock();
        b->Release();
      }
      if (db.pSample != nullptr) db.pSample->Release();
    }
  }

  IMFTransform* t_ = nullptr;
  PcmInfo info_;
  bool provides_samples_ = false;
  DWORD out_size_ = 65536;
};

}  // namespace

bool MfAacAvailable() {
  static const bool available = [] {
    IMFTransform* t = FindDecoder(kAudioFormatAac);
    if (t == nullptr) return false;
    t->Release();
    return true;
  }();
  return available;
}

std::unique_ptr<AudioDecoder> CreateMfAacDecoder(const Format& track, std::string* error) {
  auto d = std::make_unique<MfAacDecoder>();
  if (!d->Init(track, error)) return nullptr;
  return d;
}

}  // namespace gesher::media
