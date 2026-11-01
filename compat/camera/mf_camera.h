/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <jni.h>

namespace gesher::camera {

enum class LensFacing {
  kFront = 0,
  kBack = 1,
  kExternal = 2
};

struct Resolution {
  int width = 0;
  int height = 0;
  int max_fps = 30;

  bool operator==(const Resolution& other) const {
    return width == other.width && height == other.height;
  }
};

enum class MediaPixelFormat {
  kNV12,
  kYUY2,
  kRGB32
};

// Information and Camera2 characteristics for a video capture device
struct CameraCharacteristics {
  std::string device_id;
  std::string friendly_name;
  LensFacing facing = LensFacing::kFront;
  std::vector<float> available_focal_lengths = {3.5f};
  std::vector<Resolution> available_resolutions;
  int orientation = 0;
  bool is_test_device = false;
};

// Enumerate video capture devices using MFEnumDeviceSources
// with MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID.
std::vector<CameraCharacteristics> EnumerateCaptureDevices(std::string* status_out = nullptr);

// Camera characteristics provider (lens facing FRONT / BACK, focal lengths, available resolutions)
class CameraCharacteristicsProvider {
 public:
  static std::vector<CameraCharacteristics> GetAllDevices();
  static bool GetCharacteristics(const std::string& device_id, CameraCharacteristics* out_chars);
};

// Active camera capture session via Media Foundation IMFSourceReader
class MFCameraCapture {
 public:
  MFCameraCapture();
  virtual ~MFCameraCapture();

  // Activates capture source and selects media type (NV12, YUY2, or RGB32 at requested resolution and fps)
  HRESULT Open(const std::string& device_id, int width = 1280, int height = 720,
               MediaPixelFormat format = MediaPixelFormat::kNV12, int fps = 30);

  // Reads a sample frame from the IMFSourceReader into a buffer
  HRESULT ReadFrame(uint8_t* out_buffer, size_t buffer_size, int64_t* out_timestamp_ns = nullptr);

  // Read next sample frame and deliver to Android Camera2 ImageReader or SurfaceTexture
  // Handles delivery directly to the Surface's native window or ImageReader queue
  HRESULT DeliverToImageReader(JNIEnv* env, jobject image_reader_surface, int64_t timestamp_ns = 0);
  HRESULT DeliverToSurfaceTexture(JNIEnv* env, jobject surface_texture_surface, int64_t timestamp_ns = 0);
  HRESULT DeliverFrame(JNIEnv* env, jobject target_surface, int64_t timestamp_ns = 0);

  void Close();

  bool IsOpen() const;
  int GetWidth() const;
  int GetHeight() const;
  MediaPixelFormat GetFormat() const;
  int GetFps() const;
  const std::string& GetDeviceId() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gesher::camera
