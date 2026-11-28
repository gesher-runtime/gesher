// Copyright (C) 2026 The Gesher Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Native Win32 preview dialog for adjusting zoom and pan of a selected camera image
// (CameraSourceService / CameraImageSource). Decodes the image with WIC (applying EXIF
// orientation) and lets the user zoom out for black margins, zoom in, or drag to pan.

#ifdef NOGDI
#undef NOGDI
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <wincodec.h>

#include "devsvc_win32.h"
#include "../locale/ui_locale.h"
#include "../locale/ui_messages.h"
#include "../windows/host_dialog_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gesher {
namespace devsvc {
namespace {

constexpr int kMinZoomPct = 30;
constexpr int kMaxZoomPct = 250;
constexpr int kDefaultZoomPct = 100;
constexpr int kZoomButtonStep = 10;
constexpr int kZoomWheelStep = 5;
constexpr UINT kMaxPreviewEdge = 1600;

enum {
  kIdBrand = 1101,
  kIdHeading = 1102,
  kIdHint = 1103,
  kIdPreview = 1104,
  kIdZoomLabel = 1105,
  kIdZoomOut = 1106,
  kIdZoomSlider = 1107,
  kIdZoomIn = 1108,
  kIdZoomValue = 1109,
  kIdReset = 1110,
  kTimerActive = 1,
};

constexpr wchar_t kPreviewClassName[] = L"GesherCameraFramePreview";

const wchar_t* W(const std::u16string& s) {
  return reinterpret_cast<const wchar_t*>(s.c_str());
}

float ClampOffset(float value) {
  if (!std::isfinite(value)) return 0.0f;
  return std::max(-1.0f, std::min(1.0f, value));
}

int ClampZoom(int pct) {
  return std::max(kMinZoomPct, std::min(kMaxZoomPct, pct));
}

bool DesktopInteractive() {
  USEROBJECTFLAGS flags{};
  DWORD bytes = 0;
  HWINSTA station = GetProcessWindowStation();
  return station != nullptr
      && GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &bytes)
      && (flags.dwFlags & WSF_VISIBLE) != 0;
}

struct DecodedImage {
  UINT width = 0;
  UINT height = 0;
  std::vector<BYTE> bgra;  // top-down 32bpp BGR (stride = width * 4)
};

WICBitmapTransformOptions ExifToTransform(IWICBitmapFrameDecode* frame) {
  IWICMetadataQueryReader* reader = nullptr;
  if (FAILED(frame->GetMetadataQueryReader(&reader)) || !reader) {
    return WICBitmapTransformRotate0;
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  HRESULT hr = reader->GetMetadataByName(L"System.Photo.Orientation", &value);
  if (FAILED(hr)) {
    hr = reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value);
  }
  USHORT orientation = 1;
  if (SUCCEEDED(hr) && value.vt == VT_UI2) {
    orientation = value.uiVal;
  }
  PropVariantClear(&value);
  reader->Release();

  switch (orientation) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(
        WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(
        WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
  }
}

bool DecodePreviewImage(const wchar_t* path, DecodedImage* out) {
  if (!path || !path[0] || !out) return false;
  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&factory));
  if (FAILED(hr) || !factory) return false;

  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICBitmapFlipRotator* rotator = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  IWICFormatConverter* converter = nullptr;
  IWICBitmapSource* source = nullptr;
  bool ok = false;

  hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnLoad, &decoder);
  if (SUCCEEDED(hr) && decoder) {
    hr = decoder->GetFrame(0, &frame);
  }
  if (SUCCEEDED(hr) && frame) {
    source = frame;
    WICBitmapTransformOptions xform = ExifToTransform(frame);
    if (xform != WICBitmapTransformRotate0) {
      if (SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) && rotator
          && SUCCEEDED(rotator->Initialize(source, xform))) {
        source = rotator;
      }
    }
    UINT sw = 0, sh = 0;
    if (SUCCEEDED(source->GetSize(&sw, &sh)) && sw > 0 && sh > 0) {
      if (sw > kMaxPreviewEdge || sh > kMaxPreviewEdge) {
        double ratio = static_cast<double>(kMaxPreviewEdge) / std::max(sw, sh);
        UINT tw = std::max<UINT>(1, static_cast<UINT>(std::lround(sw * ratio)));
        UINT th = std::max<UINT>(1, static_cast<UINT>(std::lround(sh * ratio)));
        if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler
            && SUCCEEDED(scaler->Initialize(source, tw, th, WICBitmapInterpolationModeFant))) {
          source = scaler;
          sw = tw;
          sh = th;
        }
      }
      if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter
          && SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppBGR,
              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT stride = sw * 4;
        size_t total = static_cast<size_t>(stride) * sh;
        if (sw <= kMaxPreviewEdge && sh <= kMaxPreviewEdge && total <= 16u * 1024u * 1024u) {
          out->bgra.resize(total);
          if (SUCCEEDED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(total),
                  out->bgra.data()))) {
            out->width = sw;
            out->height = sh;
            ok = true;
          }
        }
      }
    }
  }

  if (converter) converter->Release();
  if (scaler) scaler->Release();
  if (rotator) rotator->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  factory->Release();
  return ok;
}

struct FramingDialogState {
  GesherUiLanguage language = GESHER_UI_EN;
  GesherHostTheme theme{};
  HFONT heading_font = nullptr;
  HFONT brand_font = nullptr;
  const wchar_t* title = nullptr;
  DecodedImage image;
  bool (*active)(void*) = nullptr;
  void* context = nullptr;

  int zoom_pct = kDefaultZoomPct;
  float offset_x = 0.0f;
  float offset_y = 0.0f;

  bool dragging = false;
  int drag_start_x = 0;
  int drag_start_y = 0;
  float drag_orig_ox = 0.0f;
  float drag_orig_oy = 0.0f;

  bool ended = false;
  bool confirmed = false;
};

void UpdateZoomUi(HWND dialog, FramingDialogState* state) {
  HWND slider = GetDlgItem(dialog, kIdZoomSlider);
  if (slider) {
    SendMessageW(slider, TBM_SETPOS, TRUE, state->zoom_pct);
  }
  wchar_t pct[16];
  std::swprintf(pct, sizeof(pct) / sizeof(pct[0]), L"%d%%", state->zoom_pct);
  SetDlgItemTextW(dialog, kIdZoomValue, pct);
  HWND preview = GetDlgItem(dialog, kIdPreview);
  if (preview) {
    InvalidateRect(preview, nullptr, FALSE);
  }
}

void AdjustZoom(HWND dialog, FramingDialogState* state, int delta) {
  int next = ClampZoom(state->zoom_pct + delta);
  if (next != state->zoom_pct) {
    state->zoom_pct = next;
    UpdateZoomUi(dialog, state);
  }
}

void DrawCornerGuides(HDC hdc, int cw, int ch) {
  // Draw subtle white corner brackets at 8% inset so the user can align checks/documents
  // comfortably inside an app's viewfinder overlay.
  int mx = std::max(8, cw * 8 / 100);
  int my = std::max(8, ch * 8 / 100);
  int arm = std::max(14, std::min(cw, ch) / 10);
  int l = mx, t = my, r = cw - mx, b = ch - my;
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(215, 220, 228));
  HGDIOBJ old_pen = SelectObject(hdc, pen);

  // Top-left
  MoveToEx(hdc, l, t + arm, nullptr); LineTo(hdc, l, t); LineTo(hdc, l + arm, t);
  // Top-right
  MoveToEx(hdc, r - arm, t, nullptr); LineTo(hdc, r, t); LineTo(hdc, r, t + arm);
  // Bottom-left
  MoveToEx(hdc, l, b - arm, nullptr); LineTo(hdc, l, b); LineTo(hdc, l + arm, b);
  // Bottom-right
  MoveToEx(hdc, r - arm, b, nullptr); LineTo(hdc, r, b); LineTo(hdc, r, b - arm);

  SelectObject(hdc, old_pen);
  DeleteObject(pen);
}

void PaintPreview(HWND hwnd, FramingDialogState* state) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc{};
  GetClientRect(hwnd, &rc);
  int cw = rc.right - rc.left;
  int ch = rc.bottom - rc.top;
  if (cw <= 0 || ch <= 0) {
    EndPaint(hwnd, &ps);
    return;
  }

  HDC memdc = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
  HGDIOBJ old_bmp = SelectObject(memdc, bmp);

  // Black camera frame background (matches CameraImageSource.toNv12).
  HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  FillRect(memdc, &rc, black);

  if (state && state->image.width > 0 && state->image.height > 0 && !state->image.bgra.empty()) {
    float sw = static_cast<float>(state->image.width);
    float sh = static_cast<float>(state->image.height);
    float base_scale = std::min(static_cast<float>(cw) / sw, static_cast<float>(ch) / sh);
    float scale = base_scale * (static_cast<float>(state->zoom_pct) / 100.0f);
    float dw = sw * scale;
    float dh = sh * scale;
    float left = (static_cast<float>(cw) - dw) * 0.5f + state->offset_x * static_cast<float>(cw);
    float top = (static_cast<float>(ch) - dh) * 0.5f + state->offset_y * static_cast<float>(ch);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(state->image.width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(state->image.height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(memdc, HALFTONE);
    SetBrushOrgEx(memdc, 0, 0, nullptr);
    StretchDIBits(memdc,
        static_cast<int>(std::lround(left)), static_cast<int>(std::lround(top)),
        std::max(1, static_cast<int>(std::lround(dw))),
        std::max(1, static_cast<int>(std::lround(dh))),
        0, 0, static_cast<int>(state->image.width), static_cast<int>(state->image.height),
        state->image.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
  }

  DrawCornerGuides(memdc, cw, ch);
  BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
  SelectObject(memdc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(memdc);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<FramingDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintPreview(hwnd, state);
      return 0;
    case WM_LBUTTONDOWN:
      if (state) {
        SetCapture(hwnd);
        state->dragging = true;
        state->drag_start_x = GET_X_LPARAM(lparam);
        state->drag_start_y = GET_Y_LPARAM(lparam);
        state->drag_orig_ox = state->offset_x;
        state->drag_orig_oy = state->offset_y;
      }
      return 0;
    case WM_MOUSEMOVE:
      if (state && state->dragging) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int cw = std::max<int>(1, rc.right - rc.left);
        int ch = std::max<int>(1, rc.bottom - rc.top);
        int dx = GET_X_LPARAM(lparam) - state->drag_start_x;
        int dy = GET_Y_LPARAM(lparam) - state->drag_start_y;
        state->offset_x = ClampOffset(state->drag_orig_ox + static_cast<float>(dx) / cw);
        state->offset_y = ClampOffset(state->drag_orig_oy + static_cast<float>(dy) / ch);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (state && state->dragging) {
        state->dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (state) {
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        AdjustZoom(GetParent(hwnd), state, notches * kZoomWheelStep);
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

bool EnsurePreviewClassRegistered() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = PreviewProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32646));  // IDC_SIZEALL
  wc.lpszClassName = kPreviewClassName;
  if (RegisterClassExW(&wc) != 0) return true;
  return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateDialogControl(HWND dialog, DWORD extra, const wchar_t* klass, const wchar_t* text,
    DWORD style, int id, int x, int y, int w, int h) {
  RECT r{x, y, x + w, y + h};
  if (!MapDialogRect(dialog, &r)) return nullptr;
  HWND control = CreateWindowExW(extra, klass, text, WS_CHILD | WS_VISIBLE | style,
      r.left, r.top, r.right - r.left, r.bottom - r.top,
      dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (control) {
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
  }
  return control;
}

void FinishDialog(HWND dialog, FramingDialogState* state, bool confirmed) {
  if (state->ended) return;
  state->ended = true;
  state->confirmed = confirmed;
  KillTimer(dialog, kTimerActive);
  EndDialog(dialog, confirmed ? IDOK : IDCANCEL);
}

BOOL CreateDialogControls(HWND dialog, FramingDialogState* state) {
  GesherHostThemeInit(&state->theme, dialog);
  BOOL rtl = GesherUiRtl(state->language);
  DWORD reading = rtl ? WS_EX_RTLREADING : 0;
  DWORD align = rtl ? SS_RIGHT : SS_LEFT;

  HWND brand = CreateDialogControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "brand"), align, kIdBrand, 14, 8, 332, 12);
  HWND heading = CreateDialogControl(dialog, reading, L"STATIC",
      state->title && state->title[0] ? state->title
                                      : GesherUiText(state->language, "camera_frame_title"),
      align | SS_NOPREFIX, kIdHeading, 14, 22, 332, 16);
  HWND hint = CreateDialogControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "camera_frame_hint"),
      align | SS_NOPREFIX, kIdHint, 14, 40, 332, 14);
  HWND preview = CreateDialogControl(dialog, WS_EX_CLIENTEDGE, kPreviewClassName, L"",
      WS_TABSTOP, kIdPreview, 14, 58, 332, 198);
  if (preview) {
    SetWindowLongPtrW(preview, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }

  HWND zoom_label = CreateDialogControl(dialog, reading, L"STATIC",
      GesherUiText(state->language, "camera_frame_zoom"),
      align | SS_CENTERIMAGE, kIdZoomLabel, 14, 264, 34, 18);
  HWND zoom_out = CreateDialogControl(dialog, 0, L"BUTTON", L"-",
      WS_TABSTOP | BS_OWNERDRAW, kIdZoomOut, 52, 264, 20, 18);
  HWND slider = CreateDialogControl(dialog, 0, TRACKBAR_CLASSW, L"",
      WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS, kIdZoomSlider, 76, 264, 166, 18);
  HWND zoom_in = CreateDialogControl(dialog, 0, L"BUTTON", L"+",
      WS_TABSTOP | BS_OWNERDRAW, kIdZoomIn, 246, 264, 20, 18);
  HWND zoom_val = CreateDialogControl(dialog, 0, L"STATIC", L"100%",
      SS_CENTER | SS_CENTERIMAGE, kIdZoomValue, 270, 264, 30, 18);
  HWND reset = CreateDialogControl(dialog, reading, L"BUTTON",
      GesherUiText(state->language, "camera_frame_reset"),
      WS_TABSTOP | BS_OWNERDRAW, kIdReset, 304, 264, 42, 18);

  HWND ok = CreateDialogControl(dialog, reading, L"BUTTON",
      GesherUiText(state->language, "camera_frame_use"),
      WS_TABSTOP | BS_OWNERDRAW, IDOK, 184, 290, 78, 22);
  HWND cancel = CreateDialogControl(dialog, reading, L"BUTTON",
      GesherUiText(state->language, "camera_frame_cancel"),
      WS_TABSTOP | BS_OWNERDRAW, IDCANCEL, 268, 290, 78, 22);

  if (!brand || !heading || !hint || !preview || !zoom_label || !zoom_out
      || !slider || !zoom_in || !zoom_val || !reset || !ok || !cancel) {
    return FALSE;
  }

  SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(kMinZoomPct, kMaxZoomPct));
  SendMessageW(slider, TBM_SETTICFREQ, 25, 0);
  SendMessageW(slider, TBM_SETPOS, TRUE, state->zoom_pct);
  GesherHostSetFont(heading, &state->heading_font, 130, FW_SEMIBOLD);
  GesherHostSetFont(brand, &state->brand_font, 100, FW_BOLD);
  return TRUE;
}

INT_PTR CALLBACK FramingDialogProc(HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<FramingDialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (msg == WM_INITDIALOG) {
    state = reinterpret_cast<FramingDialogState*>(lparam);
    SetLastError(0);
    LONG_PTR prev = SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
    if ((prev == 0 && GetLastError() != 0)
        || (state->active && !state->active(state->context))) {
      FinishDialog(dialog, state, false);
      return FALSE;
    }
    if (!CreateDialogControls(dialog, state)) {
      FinishDialog(dialog, state, false);
      return FALSE;
    }
    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
    if (SetTimer(dialog, kTimerActive, 250, nullptr) == 0) {
      FinishDialog(dialog, state, false);
      return FALSE;
    }
    SetFocus(GetDlgItem(dialog, IDOK));
    return FALSE;
  }
  if (!state) return FALSE;

  if (msg == WM_TIMER && wparam == kTimerActive) {
    if (state->active && !state->active(state->context)) {
      FinishDialog(dialog, state, false);
    }
    return TRUE;
  }
  if (msg == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, kIdHeading), &state->heading_font, 130, FW_SEMIBOLD);
    GesherHostSetFont(GetDlgItem(dialog, kIdBrand), &state->brand_font, 100, FW_BOLD);
    return TRUE;
  }
  if (msg == WM_HSCROLL && reinterpret_cast<HWND>(lparam) == GetDlgItem(dialog, kIdZoomSlider)) {
    int pos = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lparam), TBM_GETPOS, 0, 0));
    state->zoom_pct = ClampZoom(pos);
    UpdateZoomUi(dialog, state);
    return TRUE;
  }
  if (msg == WM_MOUSEWHEEL) {
    int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
    AdjustZoom(dialog, state, notches * kZoomWheelStep);
    return TRUE;
  }
  if (msg == WM_CLOSE) {
    FinishDialog(dialog, state, false);
    return TRUE;
  }
  if (msg == WM_DESTROY) {
    KillTimer(dialog, kTimerActive);
    if (state->heading_font) DeleteObject(state->heading_font);
    if (state->brand_font) DeleteObject(state->brand_font);
    GesherHostThemeDispose(&state->theme);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  if (msg == WM_COMMAND) {
    int cmd = LOWORD(wparam);
    if (cmd == IDOK) {
      if (state->active && !state->active(state->context)) {
        FinishDialog(dialog, state, false);
      } else {
        FinishDialog(dialog, state, true);
      }
      return TRUE;
    }
    if (cmd == IDCANCEL) {
      FinishDialog(dialog, state, false);
      return TRUE;
    }
    if (cmd == kIdZoomOut) {
      AdjustZoom(dialog, state, -kZoomButtonStep);
      return TRUE;
    }
    if (cmd == kIdZoomIn) {
      AdjustZoom(dialog, state, kZoomButtonStep);
      return TRUE;
    }
    if (cmd == kIdReset) {
      state->zoom_pct = kDefaultZoomPct;
      state->offset_x = 0.0f;
      state->offset_y = 0.0f;
      UpdateZoomUi(dialog, state);
      return TRUE;
    }
    return FALSE;
  }
  return GesherHostThemeEvent(dialog, &state->theme, msg, wparam, lparam, kIdBrand);
}

}  // namespace

CameraImageFraming FrameCameraImage(const std::u16string& title, const std::u16string& path,
    uint32_t owner_pid, bool (*active)(void*), void* context) {
  (void)owner_pid;
  CameraImageFraming result;
  if (!active || !active(context)) {
    result.available = true;
    result.confirmed = false;
    return result;
  }
  if (!DesktopInteractive() || !EnsurePreviewClassRegistered()) {
    return result;  // available = false -> caller keeps default framing
  }

  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool com_uninit = SUCCEEDED(com_hr);

  FramingDialogState state;
  state.language = GesherUiCurrentLanguage();
  state.title = title.empty() ? GesherUiText(state.language, "camera_frame_title") : W(title);
  state.active = active;
  state.context = context;

  bool decoded = DecodePreviewImage(W(path), &state.image);
  if (com_uninit) CoUninitialize();
  if (!decoded) {
    return result;  // available = false -> let Android decode with default framing
  }

  result.available = true;
  union {
    DWORD alignment;
    WORD words[160];
  } buffer{};
  auto* dlg = reinterpret_cast<DLGTEMPLATE*>(buffer.words);
  dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dlg->dwExtendedStyle = GesherUiRtl(state.language) ? WS_EX_RTLREADING : 0;
  dlg->cx = 360;
  dlg->cy = 320;
  WORD* at = reinterpret_cast<WORD*>(dlg + 1);
  *at++ = 0;  // no menu
  *at++ = 0;  // predefined dialog class
  const wchar_t* caption = state.title;
  size_t title_chars = wcslen(caption) + 1;
  const wchar_t font[] = L"Segoe UI";
  if (static_cast<size_t>(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) {
    return result;
  }
  for (size_t i = 0; i < title_chars; ++i) *at++ = static_cast<WORD>(caption[i]);
  *at++ = 9;
  for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = static_cast<WORD>(font[i]);

  DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dlg, nullptr, FramingDialogProc,
      reinterpret_cast<LPARAM>(&state));
  if (state.confirmed) {
    result.confirmed = true;
    result.scale = static_cast<float>(state.zoom_pct) / 100.0f;
    result.offset_x = ClampOffset(state.offset_x);
    result.offset_y = ClampOffset(state.offset_y);
  }
  return result;
}

}  // namespace devsvc
}  // namespace gesher
