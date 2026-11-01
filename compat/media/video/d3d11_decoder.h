// Gesher media: D3D11 / DXVA2 hardware video decoding hooks (video_decoder.h).
#pragma once

#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "video_decoder.h"

namespace gesher::media {

struct D3D11DecoderConfig {
  int width = 0;
  int height = 0;
  std::string mime;
  DXGI_FORMAT format = DXGI_FORMAT_NV12;
  bool use_hardware = true;
};

// D3D11 / DXVA2 hardware video decoder interfacing with ANGLE and ImageReader.
class D3D11VideoDecoder : public VideoDecoder {
 public:
  D3D11VideoDecoder();
  ~D3D11VideoDecoder() override;

  static bool IsHardwareSupported();
  static std::vector<VideoCodecInfo> SupportedCodecs();
  static std::unique_ptr<D3D11VideoDecoder> Create(const Format& track, std::string* error);

  bool Initialize(const Format& track, std::string* error);
  void Shutdown();

  // VideoDecoder interface implementation
  std::string Name() const override;
  bool Decode(const Sample& in, std::vector<VideoFrame>* out) override;
  void Flush() override;
  void Drain(std::vector<VideoFrame>* out) override;

  // D3D11 device and context handles
  ID3D11Device* device() const { return device_; }
  ID3D11DeviceContext* context() const { return context_; }
  ID3D11VideoDevice* video_device() const { return video_device_; }
  ID3D11VideoContext* video_context() const { return video_context_; }
  ID3D11Texture2D* current_texture() const { return current_texture_; }

  // --- ANGLE Interfacing Hooks ---
  // Obtains a shared DXGI resource handle for binding into ANGLE's EGL display
  HANDLE GetSharedHandle(ID3D11Texture2D* texture);
  // Creates an ANGLE-compatible shared D3D11 2D texture (EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE)
  bool CreateAngleSharedTexture(int width, int height, ID3D11Texture2D** out_texture, HANDLE* out_share_handle);
  // Copies/blits decoded frame to an ANGLE client texture or render target
  bool CopyToAngleTexture(ID3D11Texture2D* src_texture, ID3D11Texture2D* angle_texture);
  // Hooks an ANGLE OpenGL ES texture object to the decoded frame
  bool BindToAngleTexture(ID3D11Texture2D* texture, unsigned int gl_texture_id);

  // --- ImageReader Interfacing Hooks ---
  // Delivers decoded NV12 frame directly to an Android ImageReader's Surface (CameraFrames.deliver)
  bool DeliverToImageReader(jobject surface, ID3D11Texture2D* texture, int64_t timestamp_us);
  // Extracts raw NV12 planes from a decoded D3D11 texture using staging memory
  bool ExtractNv12(ID3D11Texture2D* texture, std::vector<uint8_t>* out_nv12,
                   int* out_width, int* out_height, int* out_stride);
  // Extracts RGBA32 pixels from texture
  bool ExtractRgba(ID3D11Texture2D* texture, std::vector<uint32_t>* out_rgba,
                   int* out_width, int* out_height);
  // Converts texture to VideoFrame (I420)
  bool ExtractFrame(ID3D11Texture2D* texture, int64_t timestamp_us, VideoFrame* out_frame);

 private:
  bool InitD3D11(std::string* error);
  bool InitDecoder(const Format& track, std::string* error);
  bool EnsureStagingTexture(int width, int height);

  std::string mime_;
  int width_ = 0;
  int height_ = 0;
  int stride_ = 0;
  int crop_left_ = 0;
  int crop_top_ = 0;
  int crop_right_ = -1;
  int crop_bottom_ = -1;
  bool hardware_ = false;

  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  ID3D11VideoDevice* video_device_ = nullptr;
  ID3D11VideoContext* video_context_ = nullptr;
  ID3D11Texture2D* current_texture_ = nullptr;
  ID3D11Texture2D* staging_texture_ = nullptr;
  int staging_width_ = 0;
  int staging_height_ = 0;

  // Media Foundation DXVA2 / D3D11 MFT integration
  void* mft_ = nullptr;              // IMFTransform*
  void* dxgi_manager_ = nullptr;     // IMFDXGIDeviceManager*
  UINT dxgi_token_ = 0;
  std::vector<uint8_t> prefix_;
  bool prefix_sent_ = false;
  bool discontinuity_ = false;
  bool output_set_ = false;
  bool provides_samples_ = false;
  DWORD out_size_ = 0;
};

// Global decoder hooks
std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format& track, std::string* error);
std::vector<VideoCodecInfo> D3D11VideoCodecs();

}  // namespace gesher::media
