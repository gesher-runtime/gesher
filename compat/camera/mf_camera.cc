/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include "mf_camera.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <shlwapi.h>

#include <jni.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

// libandroid_runtime's ANativeWindow definitions for SurfaceTexture / SurfaceView
struct ANativeWindow;
struct ARect {
  int32_t left, top, right, bottom;
};
struct ANativeWindow_Buffer {
  int32_t width, height, stride, format;
  void* bits;
  uint32_t reserved[6];
};
extern "C" {
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface);
void ANativeWindow_release(ANativeWindow* window);
int32_t ANativeWindow_getWidth(ANativeWindow* window);
int32_t ANativeWindow_getHeight(ANativeWindow* window);
int32_t ANativeWindow_getFormat(ANativeWindow* window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t w, int32_t h,
                                         int32_t format);
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* buffer, ARect* bounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window);
}

namespace gesher::camera {

namespace {

// Media Foundation GUIDs
constexpr GUID kDevSourceType = {0xc60ac5fe, 0x252a, 0x478f,
                                 {0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3}};
constexpr GUID kDevSourceVidcap = {0x8ac3587a, 0x4ae7, 0x42d8,
                                   {0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f}};
constexpr GUID kDevSourceFriendlyName = {0x60d0e559, 0x52f8, 0x4fa2,
                                         {0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01}};
constexpr GUID kDevSourceSymbolicLink = {0x58f0aad8, 0x22bf, 0x4f8a,
                                         {0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f}};
constexpr GUID kReaderVideoProcessing = {0x0f81da2c, 0xb537, 0x4672,
                                         {0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3}};
constexpr GUID kMtMajorType = {0x48eba18e, 0xf8c9, 0x4687,
                               {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
constexpr GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010,
                                  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kMtSubtype = {0xf7e34c9a, 0x42e8, 0x4714,
                             {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
constexpr GUID kVideoFormatNv12 = {0x3231564e, 0x0000, 0x0010,
                                   {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kVideoFormatYuy2 = {0x32595559, 0x0000, 0x0010,
                                   {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kVideoFormatRgb32 = {0x00000016, 0x0000, 0x0010,
                                    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kMtFrameSize = {0x1652c33d, 0xd6b2, 0x4012,
                               {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
constexpr GUID kMtFrameRate = {0xc450743b, 0x7968, 0x4e42,
                               {0x80, 0x88, 0xec, 0xa8, 0xbe, 0x81, 0xe4, 0x78}};

constexpr DWORD kFirstVideoStream = 0xFFFFFFFC;  // MF_SOURCE_READER_FIRST_VIDEO_STREAM
constexpr int kHalRgba8888 = 1;

void EnsureMF() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
  });
}

template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

std::string Utf8FromWide(const WCHAR* wstr) {
  if (!wstr) return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return "";
  std::string s(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr, -1, s.data(), len, nullptr, nullptr);
  return s;
}

inline uint8_t Clamp(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// Convert NV12 to RGBA (for SurfaceTexture / ANativeWindow)
void Nv12ToRgba(const uint8_t* nv12, int w, int h, uint8_t* rgba, int stride, int dw, int dh) {
  const uint8_t* yPlane = nv12;
  const uint8_t* uvPlane = nv12 + (w * h);
  for (int y = 0; y < dh; ++y) {
    int sy = (y * h) / dh;
    uint8_t* row = rgba + y * stride * 4;
    for (int x = 0; x < dw; ++x) {
      int sx = (x * w) / dw;
      int yVal = static_cast<int>(yPlane[sy * w + sx]) - 16;
      int uvIdx = (sy / 2) * w + (sx / 2) * 2;
      int uVal = static_cast<int>(uvPlane[uvIdx]) - 128;
      int vVal = static_cast<int>(uvPlane[uvIdx + 1]) - 128;

      row[x * 4 + 0] = Clamp((298 * yVal + 409 * vVal + 128) >> 8);
      row[x * 4 + 1] = Clamp((298 * yVal - 100 * uVal - 208 * vVal + 128) >> 8);
      row[x * 4 + 2] = Clamp((298 * yVal + 516 * uVal + 128) >> 8);
      row[x * 4 + 3] = 255;
    }
  }
}

// Convert YUY2 to RGBA (for SurfaceTexture / ANativeWindow)
void Yuy2ToRgba(const uint8_t* yuy2, int w, int h, uint8_t* rgba, int stride, int dw, int dh) {
  for (int y = 0; y < dh; ++y) {
    int sy = (y * h) / dh;
    const uint8_t* srcRow = yuy2 + sy * w * 2;
    uint8_t* dstRow = rgba + y * stride * 4;
    for (int x = 0; x < dw; ++x) {
      int sx = (x * w) / dw;
      int pairIdx = (sx / 2) * 4;
      int yVal = static_cast<int>(srcRow[pairIdx + (sx % 2 == 0 ? 0 : 2)]) - 16;
      int uVal = static_cast<int>(srcRow[pairIdx + 1]) - 128;
      int vVal = static_cast<int>(srcRow[pairIdx + 3]) - 128;

      dstRow[x * 4 + 0] = Clamp((298 * yVal + 409 * vVal + 128) >> 8);
      dstRow[x * 4 + 1] = Clamp((298 * yVal - 100 * uVal - 208 * vVal + 128) >> 8);
      dstRow[x * 4 + 2] = Clamp((298 * yVal + 516 * uVal + 128) >> 8);
      dstRow[x * 4 + 3] = 255;
    }
  }
}

// Convert RGB32 (BGRA) to RGBA
void Rgb32ToRgba(const uint8_t* rgb32, int w, int h, uint8_t* rgba, int stride, int dw, int dh) {
  for (int y = 0; y < dh; ++y) {
    int sy = (y * h) / dh;
    const uint8_t* srcRow = rgb32 + sy * w * 4;
    uint8_t* dstRow = rgba + y * stride * 4;
    for (int x = 0; x < dw; ++x) {
      int sx = (x * w) / dw;
      dstRow[x * 4 + 0] = srcRow[sx * 4 + 2];  // R
      dstRow[x * 4 + 1] = srcRow[sx * 4 + 1];  // G
      dstRow[x * 4 + 2] = srcRow[sx * 4 + 0];  // B
      dstRow[x * 4 + 3] = 255;
    }
  }
}

// Convert YUY2 to NV12
void Yuy2ToNv12(const uint8_t* yuy2, int w, int h, uint8_t* nv12) {
  uint8_t* yPlane = nv12;
  uint8_t* uvPlane = nv12 + (w * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* srcRow = yuy2 + y * w * 2;
    uint8_t* yRow = yPlane + y * w;
    for (int x = 0; x < w; ++x) {
      yRow[x] = srcRow[x * 2];
    }
    if (y % 2 == 0) {
      uint8_t* uvRow = uvPlane + (y / 2) * w;
      for (int x = 0; x < w / 2; ++x) {
        uvRow[x * 2 + 0] = srcRow[x * 4 + 1];  // U
        uvRow[x * 2 + 1] = srcRow[x * 4 + 3];  // V
      }
    }
  }
}

// Convert RGB32 (BGRA) to NV12
void Rgb32ToNv12(const uint8_t* rgb32, int w, int h, uint8_t* nv12) {
  uint8_t* yPlane = nv12;
  uint8_t* uvPlane = nv12 + (w * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int idx = (y * w + x) * 4;
      int b = rgb32[idx + 0];
      int g = rgb32[idx + 1];
      int r = rgb32[idx + 2];
      int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
      yPlane[y * w + x] = Clamp(yVal);
      if (y % 2 == 0 && x % 2 == 0) {
        int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        int uvIdx = (y / 2) * w + x;
        uvPlane[uvIdx + 0] = Clamp(uVal);
        uvPlane[uvIdx + 1] = Clamp(vVal);
      }
    }
  }
}

LensFacing DetectLensFacing(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if (lower.find("back") != std::string::npos || lower.find("rear") != std::string::npos ||
      lower.find("world") != std::string::npos) {
    return LensFacing::kBack;
  }
  if (lower.find("usb") != std::string::npos || lower.find("external") != std::string::npos) {
    return LensFacing::kExternal;
  }
  return LensFacing::kFront;
}

std::vector<Resolution> GetNativeResolutions(IMFSourceReader* reader) {
  std::vector<Resolution> resolutions;
  if (!reader) return resolutions;
  for (DWORD i = 0;; ++i) {
    IMFMediaType* mt = nullptr;
    HRESULT hr = reader->GetNativeMediaType(kFirstVideoStream, i, &mt);
    if (FAILED(hr)) break;
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(MFGetAttributeSize(mt, kMtFrameSize, &w, &h))) {
      UINT32 num = 30, den = 1;
      MFGetAttributeRatio(mt, kMtFrameRate, &num, &den);
      int fps = den ? static_cast<int>(num / den) : 30;
      Resolution res{static_cast<int>(w), static_cast<int>(h), fps};
      if (std::find(resolutions.begin(), resolutions.end(), res) == resolutions.end()) {
        resolutions.push_back(res);
      }
    }
    SafeRelease(mt);
  }
  // Sort descending by area
  std::sort(resolutions.begin(), resolutions.end(), [](const Resolution& a, const Resolution& b) {
    return (a.width * a.height) > (b.width * b.height);
  });
  return resolutions;
}

}  // namespace

// Enumerate video capture devices using MFEnumDeviceSources with MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
std::vector<CameraCharacteristics> EnumerateCaptureDevices(std::string* status_out) {
  EnsureMF();
  std::vector<CameraCharacteristics> result;

  IMFAttributes* attributes = nullptr;
  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(hr)) {
    hr = attributes->SetGUID(kDevSourceType, kDevSourceVidcap);
  }

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (SUCCEEDED(hr)) {
    hr = MFEnumDeviceSources(attributes, &devices, &count);
  }
  SafeRelease(attributes);

  if (FAILED(hr)) {
    if (status_out) {
      *status_out = "MFEnumDeviceSources failed: hr=0x" + std::to_string(hr);
    }
  } else {
    if (status_out) {
      *status_out = "Media Foundation: " + std::to_string(count) + " video capture device(s)";
    }
    for (UINT32 i = 0; i < count; ++i) {
      CameraCharacteristics chars;
      WCHAR* str = nullptr;
      UINT32 len = 0;
      if (SUCCEEDED(devices[i]->GetAllocatedString(kDevSourceSymbolicLink, &str, &len))) {
        chars.device_id = Utf8FromWide(str);
        CoTaskMemFree(str);
      }
      str = nullptr;
      if (SUCCEEDED(devices[i]->GetAllocatedString(kDevSourceFriendlyName, &str, &len))) {
        chars.friendly_name = Utf8FromWide(str);
        CoTaskMemFree(str);
      }
      chars.facing = DetectLensFacing(chars.friendly_name);
      chars.available_focal_lengths = {3.5f};
      chars.orientation = 0;

      // Inspect supported media types/resolutions
      IMFMediaSource* source = nullptr;
      if (SUCCEEDED(devices[i]->ActivateObject(IID_PPV_ARGS(&source)))) {
        IMFAttributes* readerAttr = nullptr;
        MFCreateAttributes(&readerAttr, 1);
        if (readerAttr) {
          readerAttr->SetUINT32(kReaderVideoProcessing, TRUE);
        }
        IMFSourceReader* reader = nullptr;
        if (SUCCEEDED(MFCreateSourceReaderFromMediaSource(source, readerAttr, &reader))) {
          chars.available_resolutions = GetNativeResolutions(reader);
          SafeRelease(reader);
        }
        SafeRelease(readerAttr);
        source->Shutdown();
        SafeRelease(source);
      }
      devices[i]->ShutdownObject();
      SafeRelease(devices[i]);

      if (chars.available_resolutions.empty()) {
        chars.available_resolutions = {{1920, 1080, 30}, {1280, 720, 30}, {640, 480, 30}};
      }
      if (!chars.device_id.empty()) {
        result.push_back(chars);
      }
    }
    CoTaskMemFree(devices);
  }

  // Support GESHER_CAMERA_TEST test camera
  char testEnv[32] = {0};
  if (GetEnvironmentVariableA("GESHER_CAMERA_TEST", testEnv, sizeof(testEnv)) > 0 &&
      testEnv[0] == '1') {
    CameraCharacteristics testCamera;
    testCamera.device_id = "gesher-test";
    testCamera.friendly_name = "Gesher Test Camera";
    testCamera.facing = LensFacing::kBack;
    testCamera.available_focal_lengths = {3.5f};
    testCamera.available_resolutions = {{640, 480, 30}, {320, 240, 30}};
    testCamera.orientation = 0;
    testCamera.is_test_device = true;
    result.push_back(testCamera);
  }

  return result;
}

// Camera characteristics provider
std::vector<CameraCharacteristics> CameraCharacteristicsProvider::GetAllDevices() {
  return EnumerateCaptureDevices();
}

bool CameraCharacteristicsProvider::GetCharacteristics(const std::string& device_id,
                                                       CameraCharacteristics* out_chars) {
  if (!out_chars) return false;
  auto devices = GetAllDevices();
  for (const auto& dev : devices) {
    if (dev.device_id == device_id) {
      *out_chars = dev;
      return true;
    }
  }
  // Try matching numeric index (e.g. "0", "1")
  try {
    size_t idx = std::stoul(device_id);
    if (idx < devices.size()) {
      *out_chars = devices[idx];
      return true;
    }
  } catch (...) {
  }
  return false;
}

// Implementation of MFCameraCapture
class MFCameraCapture::Impl {
 public:
  Impl() = default;
  ~Impl() { Close(); }

  HRESULT Open(const std::string& device_id, int width, int height, MediaPixelFormat format,
               int fps) {
    EnsureMF();
    Close();

    device_id_ = device_id;
    width_ = width;
    height_ = height;
    format_ = format;
    fps_ = fps;

    IMFAttributes* attributes = nullptr;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) {
      hr = attributes->SetGUID(kDevSourceType, kDevSourceVidcap);
    }
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(hr)) {
      hr = MFEnumDeviceSources(attributes, &devices, &count);
    }
    SafeRelease(attributes);
    if (FAILED(hr)) return hr;

    IMFActivate* matchedActivate = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
      WCHAR* str = nullptr;
      UINT32 len = 0;
      if (SUCCEEDED(devices[i]->GetAllocatedString(kDevSourceSymbolicLink, &str, &len))) {
        std::string symlink = Utf8FromWide(str);
        CoTaskMemFree(str);
        if (symlink == device_id || (device_id == std::to_string(i))) {
          matchedActivate = devices[i];
          break;
        }
      }
    }
    if (!matchedActivate && count > 0) {
      matchedActivate = devices[0];
    }
    if (!matchedActivate) {
      CoTaskMemFree(devices);
      return E_FAIL;
    }

    hr = matchedActivate->ActivateObject(IID_PPV_ARGS(&media_source_));
    for (UINT32 i = 0; i < count; ++i) {
      SafeRelease(devices[i]);
    }
    CoTaskMemFree(devices);
    if (FAILED(hr)) return hr;

    IMFAttributes* readerAttr = nullptr;
    MFCreateAttributes(&readerAttr, 2);
    if (readerAttr) {
      readerAttr->SetUINT32(kReaderVideoProcessing, TRUE);
    }
    hr = MFCreateSourceReaderFromMediaSource(media_source_, readerAttr, &source_reader_);
    SafeRelease(readerAttr);
    if (FAILED(hr)) return hr;

    // Configure media type: NV12, YUY2, or RGB32 at requested width x height and fps
    IMFMediaType* targetType = nullptr;
    hr = MFCreateMediaType(&targetType);
    if (SUCCEEDED(hr)) {
      targetType->SetGUID(kMtMajorType, kMediaTypeVideo);
      GUID sub = kVideoFormatNv12;
      if (format == MediaPixelFormat::kYUY2) sub = kVideoFormatYuy2;
      else if (format == MediaPixelFormat::kRGB32) sub = kVideoFormatRgb32;
      targetType->SetGUID(kMtSubtype, sub);
      MFSetAttributeSize(targetType, kMtFrameSize, width, height);
      MFSetAttributeRatio(targetType, kMtFrameRate, fps, 1);
      hr = source_reader_->SetCurrentMediaType(kFirstVideoStream, nullptr, targetType);
      SafeRelease(targetType);
    }

    // Allocate internal frame buffers
    size_t frame_bytes = (format == MediaPixelFormat::kRGB32)
                             ? (width * height * 4)
                             : (width * height * 3 / 2);
    frame_buffer_.resize(frame_bytes);
    nv12_cache_.resize(width * height * 3 / 2);
    is_open_ = true;
    return S_OK;
  }

  HRESULT ReadFrame(uint8_t* out_buffer, size_t buffer_size, int64_t* out_timestamp_ns) {
    if (!is_open_ || !source_reader_) return E_FAIL;

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG sampleTs = 0;
    IMFSample* sample = nullptr;

    HRESULT hr = source_reader_->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags,
                                            &sampleTs, &sample);
    if (FAILED(hr) || !sample) return hr;

    IMFMediaBuffer* mediaBuffer = nullptr;
    hr = sample->ConvertToContiguousBuffer(&mediaBuffer);
    if (SUCCEEDED(hr)) {
      BYTE* pData = nullptr;
      DWORD maxLen = 0, currLen = 0;
      hr = mediaBuffer->Lock(&pData, &maxLen, &currLen);
      if (SUCCEEDED(hr)) {
        size_t copyLen = std::min(buffer_size, static_cast<size_t>(currLen));
        if (out_buffer && copyLen > 0) {
          memcpy(out_buffer, pData, copyLen);
        }
        if (copyLen > 0) {
          frame_buffer_.assign(pData, pData + copyLen);
        }
        int64_t ts = (sampleTs > 0) ? (sampleTs * 100) : (GetTickCount64() * 1000000ULL);
        last_timestamp_ns_ = ts;
        if (out_timestamp_ns) {
          *out_timestamp_ns = ts;
        }

        // Cache as NV12 for Android ImageReader delivery
        if (format_ == MediaPixelFormat::kNV12) {
          nv12_cache_.assign(pData, pData + std::min(nv12_cache_.size(), static_cast<size_t>(currLen)));
        } else if (format_ == MediaPixelFormat::kYUY2) {
          Yuy2ToNv12(pData, width_, height_, nv12_cache_.data());
        } else if (format_ == MediaPixelFormat::kRGB32) {
          Rgb32ToNv12(pData, width_, height_, nv12_cache_.data());
        }

        mediaBuffer->Unlock();
      }
      SafeRelease(mediaBuffer);
    }
    SafeRelease(sample);
    return hr;
  }

  HRESULT DeliverToSurfaceTexture(JNIEnv* env, jobject surface_texture_surface,
                                 int64_t timestamp_ns) {
    if (!env || !surface_texture_surface) return E_INVALIDARG;
    if (frame_buffer_.empty()) {
      HRESULT hr = ReadFrame(nullptr, 0, &timestamp_ns);
      if (FAILED(hr)) return hr;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface_texture_surface);
    if (!window) return E_FAIL;

    int ww = ANativeWindow_getWidth(window);
    int wh = ANativeWindow_getHeight(window);
    if (ww <= 0 || wh <= 0) {
      ww = width_;
      wh = height_;
    }
    ANativeWindow_setBuffersGeometry(window, ww, wh, kHalRgba8888);
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, nullptr) == 0) {
      uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
      if (format_ == MediaPixelFormat::kNV12) {
        Nv12ToRgba(frame_buffer_.data(), width_, height_, dst, buffer.stride, ww, wh);
      } else if (format_ == MediaPixelFormat::kYUY2) {
        Yuy2ToRgba(frame_buffer_.data(), width_, height_, dst, buffer.stride, ww, wh);
      } else if (format_ == MediaPixelFormat::kRGB32) {
        Rgb32ToRgba(frame_buffer_.data(), width_, height_, dst, buffer.stride, ww, wh);
      }
      ANativeWindow_unlockAndPost(window);
    }
    ANativeWindow_release(window);
    return S_OK;
  }

  HRESULT DeliverToImageReader(JNIEnv* env, jobject image_reader_surface, int64_t timestamp_ns) {
    if (!env || !image_reader_surface) return E_INVALIDARG;
    if (nv12_cache_.empty()) {
      HRESULT hr = ReadFrame(nullptr, 0, &timestamp_ns);
      if (FAILED(hr)) return hr;
    }
    if (timestamp_ns <= 0) timestamp_ns = last_timestamp_ns_;

    jclass cameraFramesClass = env->FindClass("com/gesher/server/CameraFrames");
    if (!cameraFramesClass) return E_FAIL;

    jmethodID deliverMid = env->GetStaticMethodID(
        cameraFramesClass, "deliver", "(Landroid/view/Surface;[BIIJ[B)I");
    if (!deliverMid) {
      env->DeleteLocalRef(cameraFramesClass);
      return E_FAIL;
    }

    jbyteArray jnv12 = env->NewByteArray(static_cast<jsize>(nv12_cache_.size()));
    if (!jnv12) {
      env->DeleteLocalRef(cameraFramesClass);
      return E_OUTOFMEMORY;
    }
    env->SetByteArrayRegion(jnv12, 0, static_cast<jsize>(nv12_cache_.size()),
                            reinterpret_cast<const jbyte*>(nv12_cache_.data()));

    jint result = env->CallStaticIntMethod(cameraFramesClass, deliverMid, image_reader_surface,
                                          jnv12, static_cast<jint>(width_),
                                          static_cast<jint>(height_),
                                          static_cast<jlong>(timestamp_ns), nullptr);
    env->DeleteLocalRef(jnv12);
    env->DeleteLocalRef(cameraFramesClass);

    return (result == 0) ? S_OK : E_FAIL;
  }

  HRESULT DeliverFrame(JNIEnv* env, jobject target_surface, int64_t timestamp_ns) {
    if (!env || !target_surface) return E_INVALIDARG;
    // Check if target has an ANativeWindow (SurfaceTexture or SurfaceView)
    ANativeWindow* window = ANativeWindow_fromSurface(env, target_surface);
    if (window) {
      ANativeWindow_release(window);
      return DeliverToSurfaceTexture(env, target_surface, timestamp_ns);
    }
    // Otherwise deliver to ImageReader queue
    return DeliverToImageReader(env, target_surface, timestamp_ns);
  }

  void Close() {
    if (source_reader_) {
      SafeRelease(source_reader_);
    }
    if (media_source_) {
      media_source_->Shutdown();
      SafeRelease(media_source_);
    }
    is_open_ = false;
    frame_buffer_.clear();
    nv12_cache_.clear();
  }

  bool is_open_ = false;
  std::string device_id_;
  int width_ = 1280;
  int height_ = 720;
  MediaPixelFormat format_ = MediaPixelFormat::kNV12;
  int fps_ = 30;
  int64_t last_timestamp_ns_ = 0;

  IMFMediaSource* media_source_ = nullptr;
  IMFSourceReader* source_reader_ = nullptr;
  std::vector<uint8_t> frame_buffer_;
  std::vector<uint8_t> nv12_cache_;
};

MFCameraCapture::MFCameraCapture() : impl_(std::make_unique<Impl>()) {}
MFCameraCapture::~MFCameraCapture() = default;

HRESULT MFCameraCapture::Open(const std::string& device_id, int width, int height,
                              MediaPixelFormat format, int fps) {
  return impl_->Open(device_id, width, height, format, fps);
}

HRESULT MFCameraCapture::ReadFrame(uint8_t* out_buffer, size_t buffer_size,
                                   int64_t* out_timestamp_ns) {
  return impl_->ReadFrame(out_buffer, buffer_size, out_timestamp_ns);
}

HRESULT MFCameraCapture::DeliverToImageReader(JNIEnv* env, jobject image_reader_surface,
                                            int64_t timestamp_ns) {
  return impl_->DeliverToImageReader(env, image_reader_surface, timestamp_ns);
}

HRESULT MFCameraCapture::DeliverToSurfaceTexture(JNIEnv* env, jobject surface_texture_surface,
                                               int64_t timestamp_ns) {
  return impl_->DeliverToSurfaceTexture(env, surface_texture_surface, timestamp_ns);
}

HRESULT MFCameraCapture::DeliverFrame(JNIEnv* env, jobject target_surface, int64_t timestamp_ns) {
  return impl_->DeliverFrame(env, target_surface, timestamp_ns);
}

void MFCameraCapture::Close() {
  impl_->Close();
}

bool MFCameraCapture::IsOpen() const {
  return impl_->is_open_;
}

int MFCameraCapture::GetWidth() const {
  return impl_->width_;
}

int MFCameraCapture::GetHeight() const {
  return impl_->height_;
}

MediaPixelFormat MFCameraCapture::GetFormat() const {
  return impl_->format_;
}

int MFCameraCapture::GetFps() const {
  return impl_->fps_;
}

const std::string& MFCameraCapture::GetDeviceId() const {
  return impl_->device_id_;
}

}  // namespace gesher::camera
