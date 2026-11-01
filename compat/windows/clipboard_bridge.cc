/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "clipboard_bridge.h"

#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gesher {
namespace windows {
namespace {

#pragma pack(push, 2)
struct BmpFileHeader {
  uint16_t bfType;
  uint32_t bfSize;
  uint16_t bfReserved1;
  uint16_t bfReserved2;
  uint32_t bfOffBits;
};
#pragma pack(pop)

const wchar_t* W(const std::u16string& s) {
  return reinterpret_cast<const wchar_t*>(s.c_str());
}

std::u16string U16(const wchar_t* s, size_t n) {
  return std::u16string(reinterpret_cast<const char16_t*>(s), n);
}

std::u16string U16(const wchar_t* s) {
  return s ? U16(s, wcslen(s)) : std::u16string();
}

std::string Utf8(const std::u16string& s) {
  if (s.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, W(s), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, W(s), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

std::u16string FromUtf8(const char* s, size_t len) {
  if (len == 0) return std::u16string();
  int n = MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n);
  return U16(out.data(), out.size());
}

bool OpenClipboardRetrying() {
  for (int i = 0; i < 20; i++) {
    if (OpenClipboard(nullptr)) return true;
    Sleep(10);
  }
  return false;
}

UINT HtmlFormat() {
  static UINT f = RegisterClipboardFormatW(L"HTML Format");
  return f;
}

long HeaderNumber(const std::string& data, const char* name) {
  size_t at = data.find(name);
  if (at == std::string::npos) return -1;
  return std::strtol(data.c_str() + at + std::strlen(name), nullptr, 10);
}

// Builds CF_HTML wrapper with exact byte counts.
std::string BuildCfHtml(const std::string& fragment) {
  const char* kHeader =
      "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
      "StartFragment:%010zu\r\nEndFragment:%010zu\r\n";
  const std::string kBefore = "<html><body>\r\n<!--StartFragment-->";
  const std::string kAfter = "<!--EndFragment-->\r\n</body>\r\n</html>";
  char probe[256];
  size_t header_len = static_cast<size_t>(std::snprintf(probe, sizeof(probe), kHeader,
                                                        size_t{0}, size_t{0}, size_t{0},
                                                        size_t{0}));
  size_t start_fragment = header_len + kBefore.size();
  size_t end_fragment = start_fragment + fragment.size();
  size_t end_html = end_fragment + kAfter.size();
  char head[256];
  std::snprintf(head, sizeof(head), kHeader, header_len, end_html, start_fragment, end_fragment);
  return std::string(head) + kBefore + fragment + kAfter;
}

}  // namespace

uint32_t ClipboardSequence() {
  return GetClipboardSequenceNumber();
}

bool HasClipboardImage() {
  return IsClipboardFormatAvailable(CF_DIBV5) != 0 || IsClipboardFormatAvailable(CF_DIB) != 0;
}

bool ReadClipboardText(std::u16string* text) {
  if (text == nullptr) return false;
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) && !IsClipboardFormatAvailable(CF_TEXT)) {
    return false;
  }
  if (!OpenClipboardRetrying()) return false;

  bool ok = false;
  if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h != nullptr) {
      const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
      if (p != nullptr) {
        size_t max = GlobalSize(h) / sizeof(wchar_t);
        size_t n = 0;
        while (n < max && p[n] != L'\0') n++;
        *text = U16(p, n);
        ok = true;
        GlobalUnlock(h);
      }
    }
  } else if (IsClipboardFormatAvailable(CF_TEXT)) {
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h != nullptr) {
      const char* p = static_cast<const char*>(GlobalLock(h));
      if (p != nullptr) {
        size_t size = GlobalSize(h);
        size_t n = 0;
        while (n < size && p[n] != '\0') n++;
        int wlen = MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), nullptr, 0);
        std::wstring out(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(n), out.data(), wlen);
        *text = U16(out.data(), out.size());
        ok = true;
        GlobalUnlock(h);
      }
    }
  }
  CloseClipboard();
  return ok;
}

bool WriteClipboardText(const std::u16string& text) {
  if (!OpenClipboardRetrying()) return false;
  EmptyClipboard();

  bool ok = false;
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (g != nullptr) {
    void* p = GlobalLock(g);
    std::memcpy(p, text.c_str(), bytes);
    GlobalUnlock(g);
    if (SetClipboardData(CF_UNICODETEXT, g) != nullptr) {
      ok = true;
    } else {
      GlobalFree(g);
    }
  }
  CloseClipboard();
  return ok;
}

bool ReadClipboardHtml(std::u16string* html) {
  if (html == nullptr) return false;
  UINT format = HtmlFormat();
  if (format == 0 || !IsClipboardFormatAvailable(format) || !OpenClipboardRetrying()) {
    return false;
  }

  bool ok = false;
  HANDLE h = GetClipboardData(format);
  if (h != nullptr) {
    const char* p = static_cast<const char*>(GlobalLock(h));
    if (p != nullptr) {
      size_t size = GlobalSize(h);
      size_t n = 0;
      while (n < size && p[n] != '\0') n++;
      std::string data(p, n);
      GlobalUnlock(h);
      long start = HeaderNumber(data, "StartFragment:");
      long end = HeaderNumber(data, "EndFragment:");
      if (start >= 0 && end >= start && static_cast<size_t>(end) <= data.size()) {
        *html = FromUtf8(data.data() + start, static_cast<size_t>(end - start));
        ok = true;
      }
    }
  }
  CloseClipboard();
  return ok;
}

bool WriteClipboardHtml(const std::u16string& html) {
  UINT format = HtmlFormat();
  if (format == 0 || html.empty()) return false;
  if (!OpenClipboardRetrying()) return false;

  bool ok = false;
  std::string data = BuildCfHtml(Utf8(html));
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, data.size() + 1);
  if (g != nullptr) {
    void* p = GlobalLock(g);
    std::memcpy(p, data.c_str(), data.size() + 1);
    GlobalUnlock(g);
    if (SetClipboardData(format, g) != nullptr) {
      ok = true;
    } else {
      GlobalFree(g);
    }
  }
  CloseClipboard();
  return ok;
}

bool ReadClipboardImage(std::vector<uint8_t>* out_bmp) {
  if (out_bmp == nullptr) return false;
  UINT format = 0;
  if (IsClipboardFormatAvailable(CF_DIBV5)) {
    format = CF_DIBV5;
  } else if (IsClipboardFormatAvailable(CF_DIB)) {
    format = CF_DIB;
  } else {
    return false;
  }

  if (!OpenClipboardRetrying()) return false;

  bool ok = false;
  HANDLE h = GetClipboardData(format);
  if (h != nullptr) {
    const uint8_t* dib = static_cast<const uint8_t*>(GlobalLock(h));
    if (dib != nullptr) {
      size_t dib_size = GlobalSize(h);
      if (dib_size >= sizeof(BITMAPINFOHEADER)) {
        uint32_t header_size = *reinterpret_cast<const uint32_t*>(dib);
        uint32_t color_table_size = 0;

        const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
        uint32_t colors = bih->biClrUsed;
        if (colors == 0 && bih->biBitCount <= 8) {
          colors = 1u << bih->biBitCount;
        }
        color_table_size = colors * sizeof(RGBQUAD);

        if (bih->biCompression == BI_BITFIELDS && header_size == sizeof(BITMAPINFOHEADER)) {
          color_table_size = 3 * sizeof(DWORD);
        }

        uint32_t off_bits = sizeof(BmpFileHeader) + header_size + color_table_size;
        uint32_t file_size = sizeof(BmpFileHeader) + static_cast<uint32_t>(dib_size);

        out_bmp->resize(file_size);
        auto* bfh = reinterpret_cast<BmpFileHeader*>(out_bmp->data());
        bfh->bfType = 0x4D42;  // 'BM'
        bfh->bfSize = file_size;
        bfh->bfReserved1 = 0;
        bfh->bfReserved2 = 0;
        bfh->bfOffBits = off_bits;

        std::memcpy(out_bmp->data() + sizeof(BmpFileHeader), dib, dib_size);
        ok = true;
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return ok;
}

bool WriteClipboardImage(const uint8_t* bmp_or_dib, size_t size) {
  if (bmp_or_dib == nullptr || size == 0) return false;
  if (!OpenClipboardRetrying()) return false;

  const uint8_t* dib_ptr = bmp_or_dib;
  size_t dib_size = size;

  // If passed a complete BMP file, skip the 14-byte BITMAPFILEHEADER.
  if (size > sizeof(BmpFileHeader) && bmp_or_dib[0] == 'B' && bmp_or_dib[1] == 'M') {
    dib_ptr = bmp_or_dib + sizeof(BmpFileHeader);
    dib_size = size - sizeof(BmpFileHeader);
  }

  bool ok = false;
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, dib_size);
  if (g != nullptr) {
    void* p = GlobalLock(g);
    std::memcpy(p, dib_ptr, dib_size);
    GlobalUnlock(g);
    if (SetClipboardData(CF_DIB, g) != nullptr) {
      ok = true;
    } else {
      GlobalFree(g);
    }
  }
  CloseClipboard();
  return ok;
}

bool ReadRichClipboard(RichClipboardData* out) {
  if (out == nullptr) return false;

  out->has_text = ReadClipboardText(&out->text);
  out->has_html = ReadClipboardHtml(&out->html);
  out->has_image = ReadClipboardImage(&out->image_bytes);

  return out->has_text || out->has_html || out->has_image;
}

bool WriteRichClipboard(const std::u16string& text, const std::u16string& html,
                        const uint8_t* image_bytes, size_t image_size) {
  if (!OpenClipboardRetrying()) return false;
  EmptyClipboard();

  bool any = false;

  // CF_UNICODETEXT
  if (!text.empty()) {
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g != nullptr) {
      void* p = GlobalLock(g);
      std::memcpy(p, text.c_str(), bytes);
      GlobalUnlock(g);
      if (SetClipboardData(CF_UNICODETEXT, g) != nullptr) {
        any = true;
      } else {
        GlobalFree(g);
      }
    }
  }

  // HTML Format
  if (!html.empty() && HtmlFormat() != 0) {
    std::string data = BuildCfHtml(Utf8(html));
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, data.size() + 1);
    if (g != nullptr) {
      void* p = GlobalLock(g);
      std::memcpy(p, data.c_str(), data.size() + 1);
      GlobalUnlock(g);
      if (SetClipboardData(HtmlFormat(), g) != nullptr) {
        any = true;
      } else {
        GlobalFree(g);
      }
    }
  }

  // CF_DIB / CF_DIBV5
  if (image_bytes != nullptr && image_size > 0) {
    const uint8_t* dib_ptr = image_bytes;
    size_t dib_size = image_size;

    if (image_size > sizeof(BmpFileHeader) && image_bytes[0] == 'B' && image_bytes[1] == 'M') {
      dib_ptr = image_bytes + sizeof(BmpFileHeader);
      dib_size = image_size - sizeof(BmpFileHeader);
    }

    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, dib_size);
    if (g != nullptr) {
      void* p = GlobalLock(g);
      std::memcpy(p, dib_ptr, dib_size);
      GlobalUnlock(g);
      if (SetClipboardData(CF_DIB, g) != nullptr) {
        any = true;
      } else {
        GlobalFree(g);
      }
    }
  }

  CloseClipboard();
  return any;
}

}  // namespace windows
}  // namespace gesher

// --- JNI exports for com.gesher.server.WindowsHost ---

extern "C" {

JNIEXPORT jbyteArray JNICALL
Java_com_gesher_server_WindowsHost_nativeClipboardImage(JNIEnv* env, jclass) {
  std::vector<uint8_t> bmp;
  if (!gesher::windows::ReadClipboardImage(&bmp) || bmp.empty()) {
    return nullptr;
  }
  jbyteArray array = env->NewByteArray(static_cast<jsize>(bmp.size()));
  if (array != nullptr) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(bmp.size()),
                            reinterpret_cast<const jbyte*>(bmp.data()));
  }
  return array;
}

JNIEXPORT jboolean JNICALL
Java_com_gesher_server_WindowsHost_nativeHasClipboardImage(JNIEnv*, jclass) {
  return gesher::windows::HasClipboardImage() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_gesher_server_WindowsHost_nativeSetClipboardRich(JNIEnv* env, jclass,
                                                          jstring jtext, jstring jhtml,
                                                          jbyteArray jimage) {
  std::u16string text;
  if (jtext != nullptr) {
    const jchar* chars = env->GetStringChars(jtext, nullptr);
    jsize len = env->GetStringLength(jtext);
    text.assign(reinterpret_cast<const char16_t*>(chars), len);
    env->ReleaseStringChars(jtext, chars);
  }

  std::u16string html;
  if (jhtml != nullptr) {
    const jchar* chars = env->GetStringChars(jhtml, nullptr);
    jsize len = env->GetStringLength(jhtml);
    html.assign(reinterpret_cast<const char16_t*>(chars), len);
    env->ReleaseStringChars(jhtml, chars);
  }

  std::vector<uint8_t> image_bytes;
  if (jimage != nullptr) {
    jsize size = env->GetArrayLength(jimage);
    image_bytes.resize(static_cast<size_t>(size));
    env->GetByteArrayRegion(jimage, 0, size, reinterpret_cast<jbyte*>(image_bytes.data()));
  }

  bool ok = gesher::windows::WriteRichClipboard(
      text, html, image_bytes.empty() ? nullptr : image_bytes.data(), image_bytes.size());
  return ok ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
