/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Actual Windows consumer oracle for the native clipboard regression. This invokes Windows' BMP decoder on complete files;
 * it is not a new image decoder or color transformation in the product.
 */
#pragma once
#include <windows.h>
#include <ole2.h>
#include <wincodec.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace gesher::clipboard_wic_test {

template <typename T> class Ref {
 public:
  Ref() = default;
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  ~Ref() { if (value_) value_->Release(); }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  T** put() { return &value_; }
 private:
  T* value_ = nullptr;
};

struct ColorContext {
  HRESULT type_result = E_PENDING, data_result = E_PENDING;
  WICColorContextType type = WICColorContextUninitialized;
  UINT exif = 0;
  std::vector<uint8_t> profile;
  bool operator==(const ColorContext& other) const {
    return type_result == other.type_result && data_result == other.data_result
        && type == other.type && exif == other.exif && profile == other.profile;
  }
};
struct Picture {
  HRESULT result = E_PENDING, resolution_result = E_PENDING, color_result = E_PENDING;
  UINT width = 0, height = 0, frame_count = 0;
  GUID source_format{};
  double dpi_x = 0, dpi_y = 0;
  std::vector<uint8_t> bgra;
  std::vector<ColorContext> colors;
  bool pixels_equal(const Picture& other) const {
    return result == S_OK && other.result == S_OK && width == other.width
        && height == other.height && bgra == other.bgra;
  }
};

inline Picture Decode(const std::vector<uint8_t>& file) {
  Picture out;
  if (file.empty() || file.size() > 64u * 1024u * 1024u) {
    out.result = E_INVALIDARG;
    return out;
  }
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, file.size());
  if (!memory) { out.result = E_OUTOFMEMORY; return out; }
  void* bytes = GlobalLock(memory);
  if (!bytes) { GlobalFree(memory); out.result = E_OUTOFMEMORY; return out; }
  std::memcpy(bytes, file.data(), file.size());
  GlobalUnlock(memory);
  Ref<IStream> stream;
  out.result = CreateStreamOnHGlobal(memory, TRUE, stream.put());
  if (out.result != S_OK) { GlobalFree(memory); return out; }
  // The stream owns the memory from here. Its logical file length is exact,
  // independent of any spare HGLOBAL allocation bytes.
  ULARGE_INTEGER length{};
  length.QuadPart = file.size();
  out.result = stream->SetSize(length);
  if (out.result != S_OK) return out;
  LARGE_INTEGER zero{};
  out.result = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  if (out.result != S_OK) return out;
  Ref<IWICImagingFactory> factory;
  out.result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()));
  if (out.result != S_OK) return out;
  Ref<IWICBitmapDecoder> decoder;
  out.result = factory->CreateDecoderFromStream(stream.get(), nullptr,
      WICDecodeMetadataCacheOnLoad, decoder.put());
  if (out.result != S_OK) return out;
  GUID container{};
  out.result = decoder->GetContainerFormat(&container);
  if (out.result != S_OK || !IsEqualGUID(container, GUID_ContainerFormatBmp)) {
    if (out.result == S_OK) out.result = WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
    return out;
  }
  out.result = decoder->GetFrameCount(&out.frame_count);
  if (out.result != S_OK || out.frame_count != 1) {
    if (out.result == S_OK) out.result = WINCODEC_ERR_BADIMAGE;
    return out;
  }
  Ref<IWICBitmapFrameDecode> frame;
  out.result = decoder->GetFrame(0, frame.put());
  if (out.result != S_OK) return out;
  out.result = frame->GetSize(&out.width, &out.height);
  if (out.result != S_OK) return out;
  out.result = frame->GetPixelFormat(&out.source_format);
  if (out.result != S_OK) return out;
  out.resolution_result = frame->GetResolution(&out.dpi_x, &out.dpi_y);

  UINT color_count = 0;
  out.color_result = frame->GetColorContexts(0, nullptr, &color_count);
  if (out.color_result == S_OK && color_count > 8) {
    out.color_result = WINCODEC_ERR_VALUEOUTOFRANGE;
  } else if (out.color_result == S_OK && color_count) {
    std::vector<IWICColorContext*> contexts(color_count, nullptr);
    bool created = true;
    for (auto& context : contexts) {
      const HRESULT result = factory->CreateColorContext(&context);
      if (result != S_OK) { out.color_result = result; created = false; break; }
    }
    UINT actual = 0;
    if (created) out.color_result = frame->GetColorContexts(color_count, contexts.data(), &actual);
    if (out.color_result == S_OK && actual > color_count) out.color_result = WINCODEC_ERR_BADIMAGE;
    if (out.color_result == S_OK) {
      for (UINT index = 0; index < actual; ++index) {
        ColorContext color;
        color.type_result = contexts[index]->GetType(&color.type);
        if (color.type_result == S_OK && color.type == WICColorContextProfile) {
          UINT size = 0;
          color.data_result = contexts[index]->GetProfileBytes(0, nullptr, &size);
          if (color.data_result == S_OK && size <= 4u * 1024u * 1024u) {
            color.profile.resize(size);
            UINT copied = 0;
            color.data_result = contexts[index]->GetProfileBytes(size, color.profile.data(), &copied);
            if (color.data_result == S_OK && copied != size) color.data_result = WINCODEC_ERR_BADIMAGE;
            if (color.data_result != S_OK) color.profile.clear();
          } else if (color.data_result == S_OK) {
            color.data_result = WINCODEC_ERR_VALUEOUTOFRANGE;
          }
        } else if (color.type_result == S_OK && color.type == WICColorContextExifColorSpace) {
          color.data_result = contexts[index]->GetExifColorSpace(&color.exif);
        } else {
          color.data_result = color.type_result;
        }
        out.colors.push_back(std::move(color));
      }
    }
    for (auto* context : contexts) if (context) context->Release();
  }

  // A bounded native test output, not a production size policy. Fixtures are
  // two by two; every requested pixel and alpha byte is decoded and compared.
  const uint64_t stride = static_cast<uint64_t>(out.width) * 4u;
  const uint64_t count = stride * out.height;
  if (!out.width || !out.height || count > 16u * 1024u * 1024u) {
    out.result = WINCODEC_ERR_VALUEOUTOFRANGE;
    return out;
  }
  Ref<IWICFormatConverter> converter;
  out.result = factory->CreateFormatConverter(converter.put());
  if (out.result != S_OK) return out;
  BOOL can_convert = FALSE;
  out.result = converter->CanConvert(out.source_format, GUID_WICPixelFormat32bppBGRA, &can_convert);
  if (out.result != S_OK || !can_convert) {
    if (out.result == S_OK) out.result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    return out;
  }
  out.result = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
      WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (out.result != S_OK) return out;
  out.bgra.resize(static_cast<size_t>(count));
  out.result = converter->CopyPixels(nullptr, static_cast<UINT>(stride),
      static_cast<UINT>(count), out.bgra.data());
  if (out.result != S_OK) out.bgra.clear();
  return out;
}

inline void Print(const char* name, const char* phase, const Picture& value) {
  wchar_t format[40]{};
  StringFromGUID2(value.source_format, format, 40);
  std::printf("OLE_IMAGE_DECODE name=%s phase=%s hr=%08lx width=%u height=%u frames=%u "
      "resolution_hr=%08lx dpi_x=%.12g dpi_y=%.12g color_hr=%08lx contexts=%zu format=%ls bgra=",
      name, phase, static_cast<unsigned long>(value.result), value.width, value.height,
      value.frame_count, static_cast<unsigned long>(value.resolution_result), value.dpi_x,
      value.dpi_y, static_cast<unsigned long>(value.color_result), value.colors.size(), format);
  for (uint8_t byte : value.bgra) std::printf("%02x", static_cast<unsigned>(byte));
  std::printf("\n");
  for (size_t index = 0; index < value.colors.size(); ++index) {
    const auto& color = value.colors[index];
    std::printf("OLE_IMAGE_PROFILE name=%s phase=%s index=%zu type_hr=%08lx data_hr=%08lx "
        "type=%u exif=%u bytes=%zu profile=", name, phase, index,
        static_cast<unsigned long>(color.type_result), static_cast<unsigned long>(color.data_result),
        static_cast<unsigned>(color.type), color.exif, color.profile.size());
    for (uint8_t byte : color.profile) std::printf("%02x", static_cast<unsigned>(byte));
    std::printf("\n");
  }
}
}  // namespace gesher::clipboard_wic_test
