/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gesher {
namespace windows {

// Rich clipboard data representation for bidirectional Windows <-> Android sync.
struct RichClipboardData {
  bool has_text = false;
  std::u16string text;

  bool has_html = false;
  std::u16string html;

  bool has_image = false;
  std::vector<uint8_t> image_bytes;  // Full BMP image (with BITMAPFILEHEADER)
};

// Reads the rich clipboard contents from the Windows clipboard.
// Supports CF_UNICODETEXT, HTML Format, and CF_DIB / CF_DIBV5.
bool ReadRichClipboard(RichClipboardData* out);

// Writes rich content to the Windows clipboard.
// Any non-empty component will be placed on the clipboard with the corresponding format:
// - text -> CF_UNICODETEXT
// - html -> RegisterClipboardFormat("HTML Format")
// - image_bytes -> CF_DIB / CF_DIBV5
bool WriteRichClipboard(const std::u16string& text, const std::u16string& html,
                        const uint8_t* image_bytes, size_t image_size);

// Checks if the Windows clipboard currently has an image in CF_DIB or CF_DIBV5.
bool HasClipboardImage();

// Reads CF_DIB / CF_DIBV5 from the Windows clipboard and converts it to complete
// BMP file bytes (including BITMAPFILEHEADER). Returns false if none is available.
bool ReadClipboardImage(std::vector<uint8_t>* out_bmp);

// Writes a BMP or DIB image buffer to the Windows clipboard as CF_DIB / CF_DIBV5.
bool WriteClipboardImage(const uint8_t* bmp_or_dib, size_t size);

// Plain text (CF_UNICODETEXT).
bool ReadClipboardText(std::u16string* text);
bool WriteClipboardText(const std::u16string& text);

// HTML Format (CF_HTML).
bool ReadClipboardHtml(std::u16string* html);
bool WriteClipboardHtml(const std::u16string& html);

// Clipboard sequence number.
uint32_t ClipboardSequence();

}  // namespace windows
}  // namespace gesher
