// Real production Extract path, independent of package parsing and registration.
#include "../../compat/shell/gesher_shellext.cc"
#include <cstdio>
#include <vector>

std::vector<DWORD> Pixels(HICON icon, int size) {
  ICONINFO ii = {};
  if (!GetIconInfo(icon, &ii)) return {};
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size;
  info.bmiHeader.biHeight = -size;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  std::vector<DWORD> pixels(static_cast<size_t>(size) * size);
  HDC dc = CreateCompatibleDC(nullptr);
  int rows = GetDIBits(dc, ii.hbmColor, 0, size, pixels.data(), &info, DIB_RGB_COLORS);
  DeleteDC(dc);
  DeleteObject(ii.hbmColor);
  DeleteObject(ii.hbmMask);
  if (rows != size) return {};
  return pixels;
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;  // real application's ICO
  g_module = GetModuleHandleW(nullptr);  // gesher.ico next to probe
  IconHandler handler;
  for (int size : {16, 32, 48, 256}) {
    HICON raw = static_cast<HICON>(LoadImageW(nullptr, argv[1], IMAGE_ICON, size, size, LR_LOADFROMFILE));
    HICON result = nullptr;
    HICON small = nullptr;
    HRESULT hr = handler.Extract(argv[1], 0, &result, &small, MAKELONG(size, 16));
    if (hr != S_OK || raw == nullptr || result == nullptr || small == nullptr) return 3;
    auto before = Pixels(raw, size);
    auto after = Pixels(result, size);
    if (before.empty() || after.size() != before.size() || Pixels(small, 16).size() != 256) return 4;
    int extent = (size * 3 + 7) / 8;
    if (extent < 8) extent = 8;
    int changed = 0;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
      bool different = before[y * size + x] != after[y * size + x];
      if (x < size - extent || y < size - extent) {
        if (different) { std::printf("FAIL: app pixel %d,%d changed %08lx -> %08lx\n", x,y,static_cast<unsigned long>(before[y*size+x]),static_cast<unsigned long>(after[y*size+x])); return 5; }
      } else if (different) ++changed;
    }
    if (changed == 0) return 6;
    DestroyIcon(raw); DestroyIcon(result); DestroyIcon(small);
    HICON optional = nullptr;
    if (handler.Extract(argv[1], 0, nullptr, &optional, MAKELONG(size, 16)) != S_OK || optional == nullptr) return 7;
    DestroyIcon(optional);
    std::printf("PASS: %dpx app preserved outside badge; %d corner pixels changed; 16px small icon returned\n", size, changed);
  }
  wchar_t fallback[kPathChars];
  if (!DefaultIcon(fallback, kPathChars)) return 8;
  HICON result = nullptr;
  if (handler.Extract(fallback, 0, &result, nullptr, 32) != S_OK) return 9;
  HICON raw = static_cast<HICON>(LoadImageW(nullptr, fallback, IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
  bool same = Pixels(raw, 32) == Pixels(result, 32);
  DestroyIcon(raw); DestroyIcon(result);
  if (!same) return 10;
  // The producer can return the installed path while the handler is loaded
  // through a junction or hard link. String equality alone double-badges it.
  wchar_t alias[kPathChars];
  lstrcpyW(alias, fallback);
  lstrcatW(alias, L".alias.ico");
  if (!CreateHardLinkW(alias, fallback, nullptr)) return 15;
  result = nullptr;
  HRESULT alias_hr = handler.Extract(alias, 0, &result, nullptr, 32);
  raw = static_cast<HICON>(LoadImageW(nullptr, fallback, IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
  same = alias_hr == S_OK && raw != nullptr && result != nullptr &&
         Pixels(raw, 32) == Pixels(result, 32);
  if (raw != nullptr) DestroyIcon(raw);
  if (result != nullptr) DestroyIcon(result);
  DeleteFileW(alias);
  if (!same) { std::puts("FAIL: aliased Gesher fallback was double-badged"); return 16; }
  std::puts("PASS: aliased Gesher fallback is not double-badged");
  if (handler.Extract(nullptr, 0, &result, nullptr, 32) != E_INVALIDARG) return 11;
  HICON unbadged = gesher::LoadBadgedIcon(argv[1], L"missing-gesher-badge.ico", 32);
  raw = static_cast<HICON>(LoadImageW(nullptr, argv[1], IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
  same = Pixels(raw, 32) == Pixels(unbadged, 32);
  DestroyIcon(raw); DestroyIcon(unbadged);
  if (!same) return 12;
  DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  for (int i = 0; i < 64; ++i) {
    result = nullptr;
    if (handler.Extract(argv[1], 0, &result, nullptr, 48) != S_OK) return 13;
    DestroyIcon(result);
  }
  if (GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) > resources + 2) return 14;
  std::puts("PASS: Gesher fallback is not double-badged; null input rejected");
  std::puts("PASS: missing badge preserves app icon; repeated extraction does not leak GDI objects");
  return 0;
}
