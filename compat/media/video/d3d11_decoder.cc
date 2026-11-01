// Gesher media: D3D11 / DXVA2 hardware video decoder hooks (video_decoder.h).

#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <d3d9types.h>
#include <dxva2api.h>
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../audio/log.h"
#include "d3d11_decoder.h"
#include "video_decoder.h"

namespace gesher::media {
namespace {

// GUIDs
typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

static HRESULT DynamicD3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
  static HMODULE h = LoadLibraryW(L"d3d11.dll");
  if (!h) return E_FAIL;
  static auto pfn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
      reinterpret_cast<void*>(GetProcAddress(h, "D3D11CreateDevice")));
  if (!pfn) return E_FAIL;
  return pfn(pAdapter, DriverType, Software, Flags, pFeatureLevels,
             FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
}

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
const GUID kMtMpegSequenceHeader = {0x3C036DE7, 0x3AD0, 0x4C9E, {0x92, 0x16, 0xEE, 0x6D, 0x6A, 0xC2, 0x1C, 0xB3}};
const GUID kSampleDiscontinuity = {0x9CDF01D9, 0xA0F0, 0x43BA, {0xB0, 0x77, 0xEA, 0xA0, 0x6C, 0xBD, 0x72, 0x8A}};
const GUID kCategoryVideoDecoder = {0xD6C02D4B, 0x6833, 0x45B4, {0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91}};

const IID kIidTransform = {0xBF94C121, 0x5B05, 0x4E6F, {0x80, 0x00, 0xBA, 0x59, 0x89, 0x61, 0x41, 0x4D}};
const IID kIidID3D11VideoDevice = {0x10EC46A6, 0x9DBA, 0x4236, {0x89, 0xAE, 0xFD, 0x86, 0xAC, 0x4D, 0x47, 0x59}};
const IID kIidID3D11VideoContext = {0x61F21C45, 0x3C0E, 0x429E, {0x9A, 0xD2, 0x6E, 0x73, 0x69, 0x3E, 0x3B, 0x97}};
const IID kIidID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
const IID kIidIDXGIResource = {0x035f3ab4, 0x482e, 0x4e50, {0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b}};
const IID kIidIMFGetService = {0xfa993888, 0x4383, 0x415a, {0xa9, 0x30, 0xdd, 0x47, 0x2a, 0x8c, 0x6f, 0x73}};
const GUID kMrVideoAccelerationService = {0xefef5110, 0x567e, 0x4d76, {0x81, 0x5e, 0x47, 0xfc, 0xd0, 0x78, 0x99, 0xb6}};

// Hardware decoder profiles
const GUID kProfileH264 = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
const GUID kProfileHevc = {0x5b11d51b, 0xdd54, 0x4749, {0xbb, 0x32, 0x86, 0x1f, 0x4d, 0x99, 0xc4, 0xff}};
const GUID kProfileVp9 = {0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}};
const GUID kProfileVp8 = {0x90b70e53, 0xbd90, 0x40d2, {0x9c, 0x87, 0x65, 0x23, 0x91, 0xbf, 0xa3, 0xaa}};

const struct {
  const char* mime;
  uint32_t fourcc;
  const GUID* profile;
} kHwCodecs[] = {
    {"video/avc", FourCc('H', '2', '6', '4'), &kProfileH264},
    {"video/hevc", FourCc('H', 'E', 'V', 'C'), &kProfileHevc},
    {"video/x-vnd.on2.vp9", FourCc('V', 'P', '9', '0'), &kProfileVp9},
    {"video/x-vnd.on2.vp8", FourCc('V', 'P', '8', '0'), &kProfileVp8},
    {"video/av01", FourCc('A', 'V', '0', '1'), nullptr},
    {"video/mp4v-es", FourCc('M', 'P', '4', 'V'), nullptr},
};

struct MfApi {
  HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
  HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
  HRESULT(WINAPI* CreateSample)(IMFSample**) = nullptr;
  HRESULT(WINAPI* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
  HRESULT(WINAPI* CreateDXGIDeviceManager)(UINT*, IMFDXGIDeviceManager**) = nullptr;
  HRESULT(WINAPI* TEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                           const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*) = nullptr;
  bool ok = false;
};

template <typename F>
void LoadSymbol(HMODULE m, const char* name, F* f) {
  *f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const MfApi& Mf() {
  static MfApi api = [] {
    MfApi a;
    HMODULE m = LoadLibraryW(L"mfplat.dll");
    if (m == nullptr) return a;
    LoadSymbol(m, "MFStartup", &a.Startup);
    LoadSymbol(m, "MFCreateMediaType", &a.CreateMediaType);
    LoadSymbol(m, "MFCreateSample", &a.CreateSample);
    LoadSymbol(m, "MFCreateMemoryBuffer", &a.CreateMemoryBuffer);
    LoadSymbol(m, "MFCreateDXGIDeviceManager", &a.CreateDXGIDeviceManager);
    LoadSymbol(m, "MFTEnumEx", &a.TEnumEx);
    a.ok = a.Startup != nullptr && a.CreateMediaType != nullptr && a.CreateSample != nullptr &&
           a.CreateMemoryBuffer != nullptr && a.TEnumEx != nullptr &&
           SUCCEEDED(a.Startup(0x20070 /* MF_VERSION */, 0 /* MFSTARTUP_FULL */));
    return a;
  }();
  return api;
}

std::string HexStr(HRESULT hr) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
  return b;
}

// Global Java VM reference for ImageReader delivery
JavaVM* gJavaVm = nullptr;

JNIEnv* GetAttachedEnv() {
  if (gJavaVm == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (gJavaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
    return env;
  }
  JavaVMAttachArgs args = {JNI_VERSION_1_6, "gesher-d3d11-decoder", nullptr};
  if (gJavaVm->AttachCurrentThread(&env, &args) == JNI_OK) {
    return env;
  }
  return nullptr;
}

}  // namespace

D3D11VideoDecoder::D3D11VideoDecoder() = default;

D3D11VideoDecoder::~D3D11VideoDecoder() {
  Shutdown();
}

bool D3D11VideoDecoder::IsHardwareSupported() {
  D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D_FEATURE_LEVEL feature_level;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = DynamicD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT, feature_levels,
                                 ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &dev,
                                 &feature_level, &ctx);
  if (FAILED(hr) || dev == nullptr) return false;

  ID3D11VideoDevice* video = nullptr;
  bool supported = SUCCEEDED(dev->QueryInterface(kIidID3D11VideoDevice, reinterpret_cast<void**>(&video))) &&
                   video != nullptr;
  if (video != nullptr) video->Release();
  if (ctx != nullptr) ctx->Release();
  if (dev != nullptr) dev->Release();
  return supported;
}

std::vector<VideoCodecInfo> D3D11VideoCodecs() {
  std::vector<VideoCodecInfo> out;
  if (!D3D11VideoDecoder::IsHardwareSupported()) return out;

  for (const auto& c : kHwCodecs) {
    VideoCodecInfo info;
    info.mime = c.mime;
    info.backend = "d3d11";
    info.hardware = true;
    info.max_width = 4096;
    info.max_height = 2304;
    out.push_back(info);
  }
  return out;
}

std::unique_ptr<D3D11VideoDecoder> D3D11VideoDecoder::Create(const Format& track, std::string* error) {
  auto d = std::make_unique<D3D11VideoDecoder>();
  if (!d->Initialize(track, error)) return nullptr;
  return d;
}

bool D3D11VideoDecoder::Initialize(const Format& track, std::string* error) {
  mime_ = track.Mime();
  track.GetInt32("width", &width_);
  track.GetInt32("height", &height_);

  if (!InitD3D11(error)) {
    return false;
  }
  if (!InitDecoder(track, error)) {
    Shutdown();
    return false;
  }
  return true;
}

bool D3D11VideoDecoder::InitD3D11(std::string* error) {
  D3D_FEATURE_LEVEL feature_levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL feature_level;
  UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

  HRESULT hr = DynamicD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 feature_levels, ARRAYSIZE(feature_levels),
                                 D3D11_SDK_VERSION, &device_, &feature_level, &context_);
  if (FAILED(hr) || device_ == nullptr) {
    // Fall back to WARP
    hr = DynamicD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                           feature_levels, ARRAYSIZE(feature_levels),
                           D3D11_SDK_VERSION, &device_, &feature_level, &context_);
    hardware_ = false;
  } else {
    hardware_ = true;
  }

  if (FAILED(hr) || device_ == nullptr) {
    *error = "D3D11: D3D11CreateDevice failed (" + HexStr(hr) + ")";
    return false;
  }

  // Obtain video device and video context
  device_->QueryInterface(kIidID3D11VideoDevice, reinterpret_cast<void**>(&video_device_));
  context_->QueryInterface(kIidID3D11VideoContext, reinterpret_cast<void**>(&video_context_));

  // Initialize staging texture for readback
  if (width_ > 0 && height_ > 0) {
    EnsureStagingTexture(width_, height_);
  }
  return true;
}

bool D3D11VideoDecoder::EnsureStagingTexture(int width, int height) {
  if (staging_texture_ != nullptr && staging_width_ == width && staging_height_ == height) {
    return true;
  }
  if (staging_texture_ != nullptr) {
    staging_texture_->Release();
    staging_texture_ = nullptr;
  }

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
  if (SUCCEEDED(hr)) {
    staging_width_ = width;
    staging_height_ = height;
    return true;
  }
  return false;
}

bool D3D11VideoDecoder::InitDecoder(const Format& track, std::string* error) {
  const MfApi& mf = Mf();
  if (!mf.ok) {
    *error = "D3D11: Media Foundation platform unavailable";
    return false;
  }

  uint32_t fourcc = 0;
  for (const auto& c : kHwCodecs) {
    if (mime_ == c.mime) {
      fourcc = c.fourcc;
      break;
    }
  }
  if (fourcc == 0) {
    *error = "D3D11: unsupported video codec " + mime_;
    return false;
  }

  // Initialize DXGI Device Manager for hardware MFT
  IMFDXGIDeviceManager* dxgi_mgr = nullptr;
  if (mf.CreateDXGIDeviceManager != nullptr) {
    HRESULT hr = mf.CreateDXGIDeviceManager(&dxgi_token_, &dxgi_mgr);
    if (SUCCEEDED(hr) && dxgi_mgr != nullptr) {
      hr = dxgi_mgr->ResetDevice(device_, dxgi_token_);
      if (SUCCEEDED(hr)) {
        dxgi_manager_ = dxgi_mgr;
      } else {
        dxgi_mgr->Release();
      }
    }
  }

  // Enumerate and activate hardware video decoder MFT
  MFT_REGISTER_TYPE_INFO in_type = {kMediaTypeVideo, FourCcSubtype(fourcc)};
  IMFActivate** acts = nullptr;
  UINT32 count = 0;
  UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
  HRESULT hr = mf.TEnumEx(kCategoryVideoDecoder, flags, &in_type, nullptr, &acts, &count);
  if (FAILED(hr) || count == 0) {
    // Try with local/sync MFT
    flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    hr = mf.TEnumEx(kCategoryVideoDecoder, flags, &in_type, nullptr, &acts, &count);
  }

  IMFTransform* transform = nullptr;
  if (SUCCEEDED(hr) && acts != nullptr) {
    for (UINT32 i = 0; i < count; i++) {
      if (transform == nullptr) {
        acts[i]->ActivateObject(kIidTransform, reinterpret_cast<void**>(&transform));
      }
      acts[i]->Release();
    }
    CoTaskMemFree(acts);
  }

  if (transform == nullptr) {
    *error = "D3D11: no decoder transform available for " + mime_;
    return false;
  }

  // Configure hardware device manager on MFT
  if (dxgi_manager_ != nullptr) {
    transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                             reinterpret_cast<ULONG_PTR>(dxgi_manager_));
  }

  // Set input media type
  IMFMediaType* in = nullptr;
  mf.CreateMediaType(&in);
  in->SetGUID(kMtMajorType, kMediaTypeVideo);
  in->SetGUID(kMtSubtype, FourCcSubtype(fourcc));
  if (width_ > 0 && height_ > 0) {
    in->SetUINT64(kMtFrameSize, (static_cast<UINT64>(width_) << 32) | static_cast<UINT32>(height_));
  }
  in->SetUINT32(kMtInterlaceMode, 2 /* Progressive */);

  std::vector<uint8_t> csd;
  if (mime_ != "video/avc" && mime_ != "video/hevc" && track.GetBuffer("csd-0", &csd) && !csd.empty()) {
    in->SetBlob(kMtMpegSequenceHeader, csd.data(), static_cast<UINT32>(csd.size()));
  }

  hr = transform->SetInputType(0, in, 0);
  in->Release();
  if (FAILED(hr)) {
    transform->Release();
    *error = "D3D11: MFT rejected input type (" + HexStr(hr) + ")";
    return false;
  }

  prefix_ = AnnexBParameterSets(track, mime_);
  if (mime_ == "video/mp4v-es") track.GetBuffer("csd-0", &prefix_);

  // Set output type (NV12)
  IMFMediaType* chosen = nullptr;
  for (DWORD i = 0;; i++) {
    IMFMediaType* type = nullptr;
    if (FAILED(transform->GetOutputAvailableType(0, i, &type))) break;
    GUID sub = {};
    type->GetGUID(kMtSubtype, &sub);
    if (IsEqualGUID(sub, kNv12)) {
      chosen = type;
      break;
    }
    type->Release();
  }

  if (chosen != nullptr) {
    hr = transform->SetOutputType(0, chosen, 0);
    if (SUCCEEDED(hr)) {
      UINT64 frame_size = 0;
      chosen->GetUINT64(kMtFrameSize, &frame_size);
      width_ = static_cast<int>(frame_size >> 32);
      height_ = static_cast<int>(frame_size & 0xffffffffu);
      crop_left_ = crop_top_ = 0;
      crop_right_ = width_ - 1;
      crop_bottom_ = height_ - 1;
      output_set_ = true;
    }
    chosen->Release();
  }

  transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  mft_ = transform;

  MFT_OUTPUT_STREAM_INFO si = {};
  transform->GetOutputStreamInfo(0, &si);
  provides_samples_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  out_size_ = std::max<DWORD>(si.cbSize, static_cast<DWORD>(width_ * height_ * 3 / 2));
  return true;
}

void D3D11VideoDecoder::Shutdown() {
  if (mft_ != nullptr) {
    auto* t = static_cast<IMFTransform*>(mft_);
    t->Release();
    mft_ = nullptr;
  }
  if (dxgi_manager_ != nullptr) {
    auto* mgr = static_cast<IMFDXGIDeviceManager*>(dxgi_manager_);
    mgr->Release();
    dxgi_manager_ = nullptr;
  }
  if (current_texture_ != nullptr) {
    current_texture_->Release();
    current_texture_ = nullptr;
  }
  if (staging_texture_ != nullptr) {
    staging_texture_->Release();
    staging_texture_ = nullptr;
  }
  if (video_context_ != nullptr) {
    video_context_->Release();
    video_context_ = nullptr;
  }
  if (video_device_ != nullptr) {
    video_device_->Release();
    video_device_ = nullptr;
  }
  if (context_ != nullptr) {
    context_->Release();
    context_ = nullptr;
  }
  if (device_ != nullptr) {
    device_->Release();
    device_ = nullptr;
  }
}

std::string D3D11VideoDecoder::Name() const {
  return "d3d11 " + mime_ + (hardware_ ? " (hardware DXVA)" : " (WARP)");
}

bool D3D11VideoDecoder::Decode(const Sample& in, std::vector<VideoFrame>* out) {
  if (mft_ == nullptr) return false;
  auto* transform = static_cast<IMFTransform*>(mft_);
  const MfApi& mf = Mf();

  std::vector<uint8_t> bytes;
  const std::vector<uint8_t>* data = &in.data;
  if (!prefix_.empty() && !prefix_sent_) {
    bytes = prefix_;
    bytes.insert(bytes.end(), in.data.begin(), in.data.end());
    data = &bytes;
    prefix_sent_ = true;
  }
  if (data->empty()) return true;

  IMFMediaBuffer* buf = nullptr;
  if (FAILED(mf.CreateMemoryBuffer(static_cast<DWORD>(data->size()), &buf))) return false;
  BYTE* p = nullptr;
  buf->Lock(&p, nullptr, nullptr);
  memcpy(p, data->data(), data->size());
  buf->Unlock();
  buf->SetCurrentLength(static_cast<DWORD>(data->size()));

  IMFSample* sample = nullptr;
  mf.CreateSample(&sample);
  sample->AddBuffer(buf);
  buf->Release();
  sample->SetSampleTime(in.time_us * 10);
  if (discontinuity_) {
    sample->SetUINT32(kSampleDiscontinuity, TRUE);
    discontinuity_ = false;
  }

  HRESULT hr = transform->ProcessInput(0, sample, 0);
  if (hr == MF_E_NOTACCEPTING) {
    Drain(out);
    hr = transform->ProcessInput(0, sample, 0);
  }
  sample->Release();

  if (FAILED(hr)) {
    MEDIA_LOGE("%s: ProcessInput %s", Name().c_str(), HexStr(hr).c_str());
    return false;
  }

  // Pull decoded output samples
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER db = {};
    IMFSample* out_sample = nullptr;
    if (!provides_samples_) {
      IMFMediaBuffer* mb = nullptr;
      mf.CreateMemoryBuffer(out_size_, &mb);
      mf.CreateSample(&out_sample);
      out_sample->AddBuffer(mb);
      mb->Release();
      db.pSample = out_sample;
    }

    DWORD status = 0;
    hr = transform->ProcessOutput(0, 1, &db, &status);
    if (db.pEvents != nullptr) db.pEvents->Release();

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (out_sample != nullptr) out_sample->Release();
      break;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (out_sample != nullptr) out_sample->Release();
      continue;
    }
    if (FAILED(hr)) {
      if (out_sample != nullptr) out_sample->Release();
      break;
    }

    if (db.pSample != nullptr) {
      LONGLONG sample_time = 0;
      db.pSample->GetSampleTime(&sample_time);

      // Check if sample encapsulates a Direct3D 11 surface via IMFGetService
      IMFMediaBuffer* media_buf = nullptr;
      if (SUCCEEDED(db.pSample->GetBufferByIndex(0, &media_buf))) {
        IMFGetService* get_service = nullptr;
        if (SUCCEEDED(media_buf->QueryInterface(kIidIMFGetService, reinterpret_cast<void**>(&get_service)))) {
          ID3D11Texture2D* tex = nullptr;
          if (SUCCEEDED(get_service->GetService(kMrVideoAccelerationService, kIidID3D11Texture2D,
                                                reinterpret_cast<void**>(&tex)))) {
            if (current_texture_ != nullptr) current_texture_->Release();
            current_texture_ = tex;
          }
          get_service->Release();
        }

        // Convert to VideoFrame for output pipeline
        VideoFrame f;
        if (current_texture_ != nullptr) {
          ExtractFrame(current_texture_, sample_time / 10, &f);
        } else {
          // Software fallback buffer read
          f.Allocate(width_, height_);
          f.time_us = sample_time / 10;
          BYTE* ptr = nullptr;
          DWORD len = 0;
          if (SUCCEEDED(media_buf->Lock(&ptr, nullptr, &len))) {
            Nv12ToI420(ptr, width_, height_, &f);
            media_buf->Unlock();
          }
        }
        media_buf->Release();
        out->push_back(std::move(f));
      }
      db.pSample->Release();
    }
  }
  return true;
}

void D3D11VideoDecoder::Flush() {
  if (mft_ != nullptr) {
    auto* t = static_cast<IMFTransform*>(mft_);
    t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  discontinuity_ = true;
  prefix_sent_ = false;
}

void D3D11VideoDecoder::Drain(std::vector<VideoFrame>* out) {
  if (mft_ == nullptr) return;
  auto* t = static_cast<IMFTransform*>(mft_);
  t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

  Sample empty;
  Decode(empty, out);
  t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

HANDLE D3D11VideoDecoder::GetSharedHandle(ID3D11Texture2D* texture) {
  if (texture == nullptr) return nullptr;
  IDXGIResource* res = nullptr;
  HRESULT hr = texture->QueryInterface(kIidIDXGIResource, reinterpret_cast<void**>(&res));
  if (FAILED(hr) || res == nullptr) return nullptr;

  HANDLE handle = nullptr;
  res->GetSharedHandle(&handle);
  res->Release();
  return handle;
}

bool D3D11VideoDecoder::CreateAngleSharedTexture(int width, int height, ID3D11Texture2D** out_texture,
                                                HANDLE* out_share_handle) {
  if (out_texture == nullptr || out_share_handle == nullptr || device_ == nullptr) return false;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  ID3D11Texture2D* tex = nullptr;
  HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &tex);
  if (FAILED(hr) || tex == nullptr) return false;

  HANDLE handle = GetSharedHandle(tex);
  if (handle == nullptr) {
    tex->Release();
    return false;
  }

  *out_texture = tex;
  *out_share_handle = handle;
  return true;
}

bool D3D11VideoDecoder::CopyToAngleTexture(ID3D11Texture2D* src_texture, ID3D11Texture2D* angle_texture) {
  if (src_texture == nullptr || angle_texture == nullptr || context_ == nullptr) return false;
  context_->CopyResource(angle_texture, src_texture);
  return true;
}

bool D3D11VideoDecoder::BindToAngleTexture(ID3D11Texture2D* texture, unsigned int gl_texture_id) {
  // ANGLE interop hook: verifies texture is shared and ready for EGL binding
  if (texture == nullptr || gl_texture_id == 0) return false;
  HANDLE handle = GetSharedHandle(texture);
  return handle != nullptr;
}

bool D3D11VideoDecoder::ExtractNv12(ID3D11Texture2D* texture, std::vector<uint8_t>* out_nv12,
                                   int* out_width, int* out_height, int* out_stride) {
  if (texture == nullptr || out_nv12 == nullptr || context_ == nullptr) return false;

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  int w = desc.Width;
  int h = desc.Height;

  if (!EnsureStagingTexture(w, h)) return false;

  context_->CopyResource(staging_texture_, texture);

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  HRESULT hr = context_->Map(staging_texture_, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr) || mapped.pData == nullptr) return false;

  size_t row_pitch = mapped.RowPitch;
  size_t nv12_size = static_cast<size_t>(w) * h * 3 / 2;
  out_nv12->resize(nv12_size);

  // Copy Y plane
  const uint8_t* src_y = static_cast<const uint8_t*>(mapped.pData);
  uint8_t* dst_y = out_nv12->data();
  for (int y = 0; y < h; y++) {
    memcpy(dst_y + static_cast<size_t>(y) * w, src_y + y * row_pitch, w);
  }

  // Copy UV plane
  const uint8_t* src_uv = src_y + h * row_pitch;
  uint8_t* dst_uv = dst_y + static_cast<size_t>(w) * h;
  for (int y = 0; y < h / 2; y++) {
    memcpy(dst_uv + static_cast<size_t>(y) * w, src_uv + y * row_pitch, w);
  }

  context_->Unmap(staging_texture_, 0);

  if (out_width) *out_width = w;
  if (out_height) *out_height = h;
  if (out_stride) *out_stride = w;
  return true;
}

bool D3D11VideoDecoder::ExtractRgba(ID3D11Texture2D* texture, std::vector<uint32_t>* out_rgba,
                                   int* out_width, int* out_height) {
  std::vector<uint8_t> nv12;
  int w = 0, h = 0, stride = 0;
  if (!ExtractNv12(texture, &nv12, &w, &h, &stride)) return false;

  out_rgba->resize(static_cast<size_t>(w) * h);
  const uint8_t* y_plane = nv12.data();
  const uint8_t* uv_plane = nv12.data() + static_cast<size_t>(w) * h;

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int y_val = y_plane[y * w + x];
      int u_val = uv_plane[(y / 2) * w + (x & ~1)];
      int v_val = uv_plane[(y / 2) * w + (x & ~1) + 1];

      int c = y_val - 16;
      int d = u_val - 128;
      int e = v_val - 128;

      int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
      int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
      int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);

      (*out_rgba)[y * w + x] = static_cast<uint32_t>(r) |
                               (static_cast<uint32_t>(g) << 8) |
                               (static_cast<uint32_t>(b) << 16) |
                               0xFF000000;
    }
  }
  if (out_width) *out_width = w;
  if (out_height) *out_height = h;
  return true;
}

bool D3D11VideoDecoder::ExtractFrame(ID3D11Texture2D* texture, int64_t timestamp_us, VideoFrame* out_frame) {
  if (texture == nullptr || out_frame == nullptr) return false;
  std::vector<uint8_t> nv12;
  int w = 0, h = 0, stride = 0;
  if (!ExtractNv12(texture, &nv12, &w, &h, &stride)) return false;

  out_frame->Allocate(w, h);
  out_frame->time_us = timestamp_us;
  Nv12ToI420(nv12.data(), w, h, out_frame);
  return true;
}

bool D3D11VideoDecoder::DeliverToImageReader(jobject surface, ID3D11Texture2D* texture, int64_t timestamp_us) {
  if (surface == nullptr || texture == nullptr) return false;
  JNIEnv* env = GetAttachedEnv();
  if (env == nullptr) return false;

  std::vector<uint8_t> nv12;
  int w = 0, h = 0, stride = 0;
  if (!ExtractNv12(texture, &nv12, &w, &h, &stride)) return false;

  jclass cls = env->FindClass("com/gesher/server/CameraFrames");
  if (cls == nullptr) {
    env->ExceptionClear();
    return false;
  }

  jmethodID m = env->GetStaticMethodID(cls, "deliver", "(Landroid/view/Surface;[BIIJ[B)I");
  if (m == nullptr) {
    env->ExceptionClear();
    return false;
  }

  env->PushLocalFrame(8);
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(nv12.size()));
  env->SetByteArrayRegion(arr, 0, static_cast<jsize>(nv12.size()), reinterpret_cast<const jbyte*>(nv12.data()));
  env->CallStaticIntMethod(cls, m, surface, arr, w, h, static_cast<jlong>(timestamp_us * 1000), nullptr);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->PopLocalFrame(nullptr);
  return true;
}

std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format& track, std::string* error) {
  return D3D11VideoDecoder::Create(track, error);
}

}  // namespace gesher::media
