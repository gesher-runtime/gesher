// Windows shell presentation only: preserve the app image and identify its handler.
#pragma once

namespace gesher {
// Resolve aliases without treating an unrelated app called gesher.ico as the
// fallback. File IDs also cover hard links, junctions and short Windows paths.
inline bool SameIconFile(const wchar_t* left, const wchar_t* right) {
  if (lstrcmpiW(left, right) == 0) return true;
  constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE a = CreateFileW(left, FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
  HANDLE b = CreateFileW(right, FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, 0, nullptr);
  BY_HANDLE_FILE_INFORMATION ai = {}, bi = {};
  bool same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi) &&
      ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
      ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
  if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
  if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
  return same;
}

inline HICON LoadBadgedIcon(const wchar_t* app_path, const wchar_t* badge_path, int pixels) {
  if (pixels <= 0 || pixels > 1024) return nullptr;
  HICON app = static_cast<HICON>(LoadImageW(nullptr, app_path, IMAGE_ICON, pixels, pixels,
                                            LR_LOADFROMFILE));
  if (app == nullptr) return nullptr;
  int badge_pixels = (pixels * 3 + 7) / 8;
  if (badge_pixels < 8) badge_pixels = pixels < 8 ? pixels : 8;
  HICON badge = static_cast<HICON>(LoadImageW(nullptr, badge_path, IMAGE_ICON,
                                              badge_pixels, badge_pixels, LR_LOADFROMFILE));
  if (badge == nullptr) return app;
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = pixels;
  info.bmiHeader.biHeight = -pixels;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HBITMAP mask = nullptr;
  HICON result = nullptr;
  if (dc != nullptr && color != nullptr && bits != nullptr) {
    auto rgba_out = static_cast<DWORD*>(bits);
    ZeroMemory(bits, static_cast<SIZE_T>(pixels) * pixels * 4);
    // Read the original straight-alpha pixels before selecting the destination
    // DIB. Drawing the whole app and undoing premultiplication loses RGB bits
    // even outside the badge. Legacy icons without alpha still use GDI's mask.
    ICONINFO app_info = {};
    bool straight_alpha = false;
    if (GetIconInfo(app, &app_info)) {
      BITMAP bitmap = {};
      if (app_info.hbmColor != nullptr &&
          GetObjectW(app_info.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
          bitmap.bmBitsPixel == 32 && bitmap.bmWidth == pixels && bitmap.bmHeight == pixels &&
          GetDIBits(dc, app_info.hbmColor, 0, pixels, bits, &info, DIB_RGB_COLORS) == pixels) {
        for (int i = 0; i < pixels * pixels; ++i) {
          if ((rgba_out[i] >> 24) != 0) { straight_alpha = true; break; }
        }
      }
      if (app_info.hbmColor != nullptr) DeleteObject(app_info.hbmColor);
      if (app_info.hbmMask != nullptr) DeleteObject(app_info.hbmMask);
    }
    const int corner = pixels - badge_pixels;
    if (straight_alpha) {
      // GDI expects premultiplied destination colors only where it blends.
      for (int y = corner; y < pixels; ++y) for (int x = corner; x < pixels; ++x) {
        DWORD value = rgba_out[y * pixels + x];
        DWORD alpha = value >> 24;
        DWORD red = (((value >> 16) & 255) * alpha + 127) / 255;
        DWORD green = (((value >> 8) & 255) * alpha + 127) / 255;
        DWORD blue = ((value & 255) * alpha + 127) / 255;
        rgba_out[y * pixels + x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
      }
    } else {
      ZeroMemory(bits, static_cast<SIZE_T>(pixels) * pixels * 4);
    }
    HGDIOBJ previous = SelectObject(dc, color);
    bool drawn = (straight_alpha || DrawIconEx(dc, 0, 0, app, pixels, pixels, 0, nullptr, DI_NORMAL)) &&
        DrawIconEx(dc, pixels - badge_pixels, pixels - badge_pixels, badge,
                   badge_pixels, badge_pixels, 0, nullptr, DI_NORMAL);
    SelectObject(dc, previous);
    // DrawIconEx blends into the transparent DIB using premultiplied channels;
    // restore the ICO bitmap's straight RGB channels before CreateIconIndirect.
    for (int i = 0; i < pixels * pixels; ++i) {
      if (straight_alpha && (i / pixels < corner || i % pixels < corner)) continue;
      DWORD value = rgba_out[i];
      DWORD alpha = value >> 24;
      if (alpha != 0 && alpha != 255) {
        DWORD red = (((value >> 16) & 255) * 255 + alpha / 2) / alpha;
        DWORD green = (((value >> 8) & 255) * 255 + alpha / 2) / alpha;
        DWORD blue = ((value & 255) * 255 + alpha / 2) / alpha;
        rgba_out[i] = (alpha << 24) | ((red > 255 ? 255 : red) << 16) |
                      ((green > 255 ? 255 : green) << 8) | (blue > 255 ? 255 : blue);
      }
    }
    // Legacy monochrome fallback follows the resulting alpha, including the
    // transparent area outside rounded app/badge corners.
    SIZE_T stride = static_cast<SIZE_T>((pixels + 15) / 16) * 2;
    auto mask_bits = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stride * pixels));
    if (mask_bits != nullptr) {
      auto rgba = static_cast<const DWORD*>(bits);
      for (int y = 0; y < pixels; ++y) {
        for (int x = 0; x < pixels; ++x) {
          if ((rgba[y * pixels + x] >> 24) == 0) mask_bits[y * stride + x / 8] |= 0x80 >> (x % 8);
        }
      }
      mask = CreateBitmap(pixels, pixels, 1, 1, mask_bits);
      HeapFree(GetProcessHeap(), 0, mask_bits);
      if (drawn && mask != nullptr) {
        ICONINFO icon = {};
        icon.fIcon = TRUE;
        icon.hbmColor = color;
        icon.hbmMask = mask;
        result = CreateIconIndirect(&icon);
      }
    }
  }
  if (mask != nullptr) DeleteObject(mask);
  if (color != nullptr) DeleteObject(color);
  if (dc != nullptr) DeleteDC(dc);
  DestroyIcon(badge);
  if (result == nullptr) return app;
  DestroyIcon(app);
  return result;
}
}  // namespace gesher
