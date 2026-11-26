// Gesher (print): Windows printers, Windows' print dialog and PDF printing.
// See print_win32.h.

#include "print_win32.h"

#include <objbase.h>  // PrintDlgEx declarations require COM's STDMETHOD macros
#include <commdlg.h>
#include <cderr.h>
#include <wincodec.h>
#include <winspool.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

#include "winrt.h"  // compat/devices: combase's WinRT entry points, found at run time

namespace gesher::print {

namespace winrt = gesher::devices::winrt;

namespace {

// ---- winspool -------------------------------------------------------------

class PrinterHandle {
 public:
  explicit PrinterHandle(const std::wstring& name) {
    if (!OpenPrinterW(const_cast<LPWSTR>(name.c_str()), &handle_, nullptr)) handle_ = nullptr;
  }
  ~PrinterHandle() {
    if (handle_) ClosePrinter(handle_);
  }
  PrinterHandle(const PrinterHandle&) = delete;
  PrinterHandle& operator=(const PrinterHandle&) = delete;
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

HRESULT LastError() {
  DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

// PRINTER_INFO_2 for one printer: its port, driver and status.
bool PrinterInfo2(const std::wstring& name, Printer* printer) {
  PrinterHandle handle(name);
  if (!handle.get()) return false;
  DWORD needed = 0;
  GetPrinterW(handle.get(), 2, nullptr, 0, &needed);
  if (needed == 0) return false;
  std::vector<BYTE> buffer(needed);
  if (!GetPrinterW(handle.get(), 2, buffer.data(), needed, &needed)) return false;
  auto* info = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
  printer->status = info->Status;
  printer->attributes = info->Attributes;
  if (info->pPortName) printer->port = info->pPortName;
  if (info->pDriverName) printer->driver = info->pDriverName;
  return true;
}

std::wstring PortOf(const std::wstring& name) {
  Printer printer;
  return PrinterInfo2(name, &printer) ? printer.port : std::wstring();
}

// DeviceCapabilitiesW's count of fixed-size records, or -1.
int CapabilityCount(const std::wstring& name, const std::wstring& port, WORD capability,
                    const DEVMODEW* devmode) {
  return DeviceCapabilitiesW(name.c_str(), port.empty() ? nullptr : port.c_str(), capability,
                             nullptr, devmode);
}

bool PrinterDc(const std::wstring& name, const std::vector<BYTE>& devmode, HDC* dc) {
  *dc = CreateDCW(L"WINSPOOL", name.c_str(), nullptr,
                  devmode.empty() ? nullptr : reinterpret_cast<const DEVMODEW*>(devmode.data()));
  return *dc != nullptr;
}

std::vector<Paper> Papers(const std::wstring& name, const std::wstring& port) {
  std::vector<Paper> papers;
  int count = CapabilityCount(name, port, DC_PAPERS, nullptr);
  if (count <= 0 || count > 4096) return papers;
  std::vector<WORD> ids(count);
  std::vector<wchar_t> names(static_cast<size_t>(count) * 64);
  std::vector<POINT> sizes(count);
  const wchar_t* p = port.empty() ? nullptr : port.c_str();
  if (DeviceCapabilitiesW(name.c_str(), p, DC_PAPERS, reinterpret_cast<LPWSTR>(ids.data()), nullptr) != count ||
      DeviceCapabilitiesW(name.c_str(), p, DC_PAPERSIZE, reinterpret_cast<LPWSTR>(sizes.data()), nullptr) != count) {
    return papers;
  }
  bool named = DeviceCapabilitiesW(name.c_str(), p, DC_PAPERNAMES, names.data(), nullptr) == count;
  for (int i = 0; i < count; ++i) {
    if (sizes[i].x <= 0 || sizes[i].y <= 0) continue;
    Paper paper;
    paper.id = ids[i];
    if (named) {
      const wchar_t* label = &names[static_cast<size_t>(i) * 64];
      paper.name.assign(label, wcsnlen(label, 64));
    }
    paper.width = std::min(sizes[i].x, sizes[i].y);
    paper.height = std::max(sizes[i].x, sizes[i].y);
    papers.push_back(paper);
  }
  return papers;
}

}  // namespace

HRESULT ListPrinters(std::vector<Printer>* out) {
  out->clear();
  const DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
  DWORD needed = 0, returned = 0;
  if (!EnumPrintersW(flags, nullptr, 4, nullptr, 0, &needed, &returned)) {
    DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  if (needed == 0) return S_OK;
  std::vector<BYTE> buffer(needed);
  if (!EnumPrintersW(flags, nullptr, 4, buffer.data(), needed, &needed, &returned)) return LastError();
  std::wstring fallback;
  DefaultPrinter(&fallback);
  auto* infos = reinterpret_cast<PRINTER_INFO_4W*>(buffer.data());
  for (DWORD i = 0; i < returned; ++i) {
    if (!infos[i].pPrinterName) continue;
    Printer printer;
    printer.name = infos[i].pPrinterName;
    printer.attributes = infos[i].Attributes;
    printer.is_default = !fallback.empty() && _wcsicmp(fallback.c_str(), printer.name.c_str()) == 0;
    PrinterInfo2(printer.name, &printer);
    out->push_back(std::move(printer));
  }
  return S_OK;
}

HRESULT DefaultPrinter(std::wstring* name) {
  name->clear();
  DWORD size = 0;
  if (!GetDefaultPrinterW(nullptr, &size)) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return S_FALSE;
    if (error != ERROR_INSUFFICIENT_BUFFER) return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  if (size == 0) return S_FALSE;
  std::vector<wchar_t> buffer(size);
  if (!GetDefaultPrinterW(buffer.data(), &size)) {
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ? S_FALSE : (error ? HRESULT_FROM_WIN32(error) : E_FAIL);
  }
  name->assign(buffer.data());
  return name->empty() ? S_FALSE : S_OK;
}

HRESULT DefaultSettings(const std::wstring& printer, std::vector<BYTE>* devmode) {
  devmode->clear();
  PrinterHandle handle(printer);
  if (!handle.get()) return LastError();
  LPWSTR device = const_cast<LPWSTR>(printer.c_str());
  LONG size = DocumentPropertiesW(nullptr, handle.get(), device, nullptr, nullptr, 0);
  if (size < static_cast<LONG>(sizeof(DEVMODEW) - sizeof(DWORD) * 8) || size > (1 << 20)) {
    return size < 0 ? LastError() : E_UNEXPECTED;
  }
  devmode->resize(size);
  LONG result = DocumentPropertiesW(nullptr, handle.get(), device,
                                    reinterpret_cast<DEVMODEW*>(devmode->data()), nullptr, DM_OUT_BUFFER);
  if (result != IDOK) {
    devmode->clear();
    return result < 0 ? LastError() : E_FAIL;
  }
  return S_OK;
}

HRESULT QueryCapabilities(const std::wstring& printer, Capabilities* out) {
  *out = Capabilities();
  HRESULT hr = DefaultSettings(printer, &out->default_devmode);
  if (FAILED(hr)) return hr;
  std::wstring port = PortOf(printer);
  out->papers = Papers(printer, port);
  int resolutions = CapabilityCount(printer, port, DC_ENUMRESOLUTIONS, nullptr);
  if (resolutions > 0 && resolutions < 1024) {
    std::vector<LONG> values(static_cast<size_t>(resolutions) * 2);
    if (DeviceCapabilitiesW(printer.c_str(), port.empty() ? nullptr : port.c_str(), DC_ENUMRESOLUTIONS,
                            reinterpret_cast<LPWSTR>(values.data()), nullptr) == resolutions) {
      for (int i = 0; i < resolutions; ++i) {
        if (values[2 * i] > 0 && values[2 * i + 1] > 0) {
          out->resolutions.push_back(POINT{values[2 * i], values[2 * i + 1]});
        }
      }
    }
  }
  out->color = CapabilityCount(printer, port, DC_COLORDEVICE, nullptr) == 1;
  out->duplex = CapabilityCount(printer, port, DC_DUPLEX, nullptr) == 1;
  out->landscape = CapabilityCount(printer, port, DC_ORIENTATION, nullptr) > 0;
  int copies = CapabilityCount(printer, port, DC_COPIES, nullptr);
  out->max_copies = copies > 0 ? static_cast<DWORD>(copies) : 1;
  return S_OK;
}

HRESULT ApplyRequest(const std::wstring& printer, const Request& request, std::vector<BYTE>* devmode) {
  if (devmode->empty()) {
    HRESULT hr = DefaultSettings(printer, devmode);
    if (FAILED(hr)) return hr;
  }
  std::vector<BYTE> defaults;
  HRESULT hr = DefaultSettings(printer, &defaults);
  if (FAILED(hr)) return hr;
  const DWORD supported = reinterpret_cast<const DEVMODEW*>(defaults.data())->dmFields;
  auto* dm = reinterpret_cast<DEVMODEW*>(devmode->data());
  std::wstring port = PortOf(printer);
  if (request.width > 0 && request.height > 0 && (supported & DM_PAPERSIZE)) {
    // A paper the driver lists, within a millimeter of the requested size.
    LONG w = std::min(request.width, request.height), h = std::max(request.width, request.height);
    for (const Paper& paper : Papers(printer, port)) {
      if (std::labs(paper.width - w) <= 10 && std::labs(paper.height - h) <= 10) {
        dm->dmPaperSize = static_cast<short>(paper.id);
        dm->dmFields = (dm->dmFields | DM_PAPERSIZE) & ~(DM_PAPERLENGTH | DM_PAPERWIDTH);
        break;
      }
    }
  }
  if (request.landscape >= 0 && (supported & DM_ORIENTATION) &&
      (request.landscape == 0 || CapabilityCount(printer, port, DC_ORIENTATION, nullptr) > 0)) {
    dm->dmOrientation = static_cast<short>(request.landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT);
    dm->dmFields |= DM_ORIENTATION;
  }
  if (request.color >= 0 && (supported & DM_COLOR) &&
      (request.color == DMCOLOR_MONOCHROME || CapabilityCount(printer, port, DC_COLORDEVICE, nullptr) == 1)) {
    dm->dmColor = static_cast<short>(request.color);
    dm->dmFields |= DM_COLOR;
  }
  if (request.duplex >= 0 && (supported & DM_DUPLEX) &&
      (request.duplex == DMDUP_SIMPLEX || CapabilityCount(printer, port, DC_DUPLEX, nullptr) == 1)) {
    dm->dmDuplex = static_cast<short>(request.duplex);
    dm->dmFields |= DM_DUPLEX;
  }
  // The driver merges and validates the fields it was given.
  PrinterHandle handle(printer);
  if (!handle.get()) return LastError();
  std::vector<BYTE> merged(devmode->size() > defaults.size() ? devmode->size() : defaults.size());
  LONG result = DocumentPropertiesW(nullptr, handle.get(), const_cast<LPWSTR>(printer.c_str()),
                                    reinterpret_cast<DEVMODEW*>(merged.data()), dm,
                                    DM_IN_BUFFER | DM_OUT_BUFFER);
  if (result != IDOK) return result < 0 ? LastError() : E_FAIL;
  merged.resize(defaults.size());
  *devmode = std::move(merged);
  return S_OK;
}

HRESULT Describe(const std::wstring& printer, const std::vector<BYTE>& devmode, Description* out) {
  *out = Description();
  if (devmode.size() < offsetof(DEVMODEW, dmFields) + sizeof(DWORD)) return E_INVALIDARG;
  const auto* dm = reinterpret_cast<const DEVMODEW*>(devmode.data());
  std::wstring port = PortOf(printer);
  HDC dc = nullptr;
  if (!PrinterDc(printer, devmode, &dc)) return LastError();
  LONG dpi_x = GetDeviceCaps(dc, LOGPIXELSX), dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
  LONG width = GetDeviceCaps(dc, PHYSICALWIDTH), height = GetDeviceCaps(dc, PHYSICALHEIGHT);
  LONG offset_x = GetDeviceCaps(dc, PHYSICALOFFSETX), offset_y = GetDeviceCaps(dc, PHYSICALOFFSETY);
  LONG printable_x = GetDeviceCaps(dc, HORZRES), printable_y = GetDeviceCaps(dc, VERTRES);
  DeleteDC(dc);
  if (dpi_x <= 0 || dpi_y <= 0 || width <= 0 || height <= 0) return E_UNEXPECTED;
  auto mils = [](LONG pixels, LONG dpi) { return static_cast<LONG>((static_cast<int64_t>(pixels) * 1000 + dpi / 2) / dpi); };
  out->dpi_x = dpi_x;
  out->dpi_y = dpi_y;
  out->width_mils = mils(width, dpi_x);
  out->height_mils = mils(height, dpi_y);
  out->margins_mils[0] = mils(std::max<LONG>(0, offset_x), dpi_x);
  out->margins_mils[1] = mils(std::max<LONG>(0, offset_y), dpi_y);
  out->margins_mils[2] = mils(std::max<LONG>(0, width - offset_x - printable_x), dpi_x);
  out->margins_mils[3] = mils(std::max<LONG>(0, height - offset_y - printable_y), dpi_y);
  out->landscape = (dm->dmFields & DM_ORIENTATION) ? dm->dmOrientation == DMORIENT_LANDSCAPE : width > height;
  out->color_capable = CapabilityCount(printer, port, DC_COLORDEVICE, nullptr) == 1;
  out->color = out->color_capable && (!(dm->dmFields & DM_COLOR) || dm->dmColor == DMCOLOR_COLOR);
  out->duplex_capable = CapabilityCount(printer, port, DC_DUPLEX, nullptr) == 1;
  out->duplex = (dm->dmFields & DM_DUPLEX) && out->duplex_capable ? dm->dmDuplex : DMDUP_SIMPLEX;
  out->copies = (dm->dmFields & DM_COPIES) && dm->dmCopies > 0 ? dm->dmCopies : 1;
  if (dm->dmFields & DM_PAPERSIZE) {
    out->paper = static_cast<WORD>(dm->dmPaperSize);
    for (const Paper& paper : Papers(printer, port)) {
      if (paper.id == out->paper) {
        out->paper_name = paper.name;
        break;
      }
    }
  }
  return S_OK;
}

// ---- the print dialog -----------------------------------------------------

namespace {

struct DialogOwner {
  DWORD pid;
  HWND window;
};

BOOL CALLBACK FindOwner(HWND window, LPARAM parameter) {
  auto* owner = reinterpret_cast<DialogOwner*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == owner->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
    owner->window = window;
    return FALSE;
  }
  return TRUE;
}

struct DialogLifetime {
  bool (*active)(void*);
  void* context;
  HWND helper;
  bool closed;
};

thread_local DialogLifetime* dialog_lifetime = nullptr;

BOOL CALLBACK CancelThreadWindow(HWND window, LPARAM parameter) {
  if (window != reinterpret_cast<HWND>(parameter) && IsWindowVisible(window)) {
    PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
  }
  return TRUE;
}

// A thread timer, dispatched by the dialog's modal loop on this thread.
void CALLBACK DialogTimer(HWND, UINT, UINT_PTR, DWORD) {
  DialogLifetime* lifetime = dialog_lifetime;
  if (!lifetime || lifetime->closed || lifetime->active(lifetime->context)) return;
  lifetime->closed = true;
  EnumThreadWindows(GetCurrentThreadId(), CancelThreadWindow, reinterpret_cast<LPARAM>(lifetime->helper));
}

HGLOBAL GlobalCopy(const void* data, size_t size) {
  HGLOBAL global = GlobalAlloc(GHND, size);
  if (!global) return nullptr;
  void* target = GlobalLock(global);
  if (!target) {
    GlobalFree(global);
    return nullptr;
  }
  memcpy(target, data, size);
  GlobalUnlock(global);
  return global;
}

HGLOBAL DevNames(const std::wstring& printer) {
  // DEVNAMES offsets count WCHARs from its start: an empty driver, the device, an empty port.
  const size_t header = sizeof(DEVNAMES) / sizeof(wchar_t);
  std::vector<wchar_t> data(header + 1 + printer.size() + 1 + 1, L'\0');
  auto* names = reinterpret_cast<DEVNAMES*>(data.data());
  names->wDriverOffset = static_cast<WORD>(header);
  names->wDeviceOffset = static_cast<WORD>(header + 1);
  names->wOutputOffset = static_cast<WORD>(header + 1 + printer.size() + 1);
  names->wDefault = 0;
  memcpy(&data[header + 1], printer.c_str(), printer.size() * sizeof(wchar_t));
  return GlobalCopy(data.data(), data.size() * sizeof(wchar_t));
}

}  // namespace

HRESULT ChoosePrinter(const std::wstring& printer, const std::vector<BYTE>& devmode, int page_count,
                      DWORD owner_pid, bool (*active)(void*), void* context, Choice* out) {
  *out = Choice();
  if (!active || !active(context)) return S_FALSE;
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  DialogOwner owner{owner_pid, nullptr};
  if (owner_pid) EnumWindows(FindOwner, reinterpret_cast<LPARAM>(&owner));
  // PrintDlgEx needs an owner window. Without a visible app window (a headless
  // device), an invisible top-level window of this thread owns it.
  HWND helper = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  HRESULT hr = S_FALSE;
  if (!owner.window && !helper) hr = LastError();
  PRINTPAGERANGE ranges[32] = {};
  PRINTDLGEXW pd = {};
  pd.lStructSize = sizeof(pd);
  pd.hwndOwner = owner.window ? owner.window : helper;
  if (!devmode.empty()) pd.hDevMode = GlobalCopy(devmode.data(), devmode.size());
  if (!printer.empty()) pd.hDevNames = DevNames(printer);
  pd.Flags = PD_ALLPAGES | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION | PD_NOCURRENTPAGE |
             (page_count > 0 ? 0 : PD_NOPAGENUMS);
  pd.nMinPage = 1;
  pd.nMaxPage = page_count > 0 ? static_cast<DWORD>(page_count) : 1;
  pd.nMaxPageRanges = page_count > 0 ? 32 : 0;
  pd.lpPageRanges = page_count > 0 ? ranges : nullptr;
  pd.nCopies = 1;
  pd.nStartPage = START_PAGE_GENERAL;
  DialogLifetime lifetime{active, context, helper, false};
  if (pd.hwndOwner) {
    dialog_lifetime = &lifetime;
    UINT_PTR timer = SetTimer(nullptr, 0, 100, DialogTimer);
    hr = PrintDlgExW(&pd);
    if (timer) KillTimer(nullptr, timer);
    dialog_lifetime = nullptr;
    if (FAILED(hr)) {
      // No printer installed: Windows told the user so; the dialog was not used.
      if (CommDlgExtendedError() == PDERR_NODEFAULTPRN) hr = S_FALSE;
    } else if (pd.dwResultAction != PD_RESULT_PRINT || lifetime.closed || !active(context)) {
      hr = S_FALSE;
    } else {
      hr = S_OK;
      if (pd.hDevNames) {
        auto* names = static_cast<DEVNAMES*>(GlobalLock(pd.hDevNames));
        if (names) {
          out->printer = reinterpret_cast<const wchar_t*>(names) + names->wDeviceOffset;
          GlobalUnlock(pd.hDevNames);
        }
      }
      if (pd.hDevMode) {
        auto* dm = static_cast<DEVMODEW*>(GlobalLock(pd.hDevMode));
        if (dm) {
          size_t size = static_cast<size_t>(dm->dmSize) + dm->dmDriverExtra;
          if (size <= GlobalSize(pd.hDevMode)) {
            out->devmode.assign(reinterpret_cast<BYTE*>(dm), reinterpret_cast<BYTE*>(dm) + size);
          }
          GlobalUnlock(pd.hDevMode);
        }
      }
      if (out->printer.empty() || out->devmode.empty()) hr = E_UNEXPECTED;
      if ((pd.Flags & PD_PAGENUMS) && page_count > 0) {
        for (DWORD i = 0; i < pd.nPageRanges; ++i) {
          DWORD from = std::min(ranges[i].nFromPage, ranges[i].nToPage);
          DWORD to = std::max(ranges[i].nFromPage, ranges[i].nToPage);
          if (from < 1 || from > static_cast<DWORD>(page_count)) continue;
          to = std::min<DWORD>(to, static_cast<DWORD>(page_count));
          out->pages.push_back(static_cast<int>(from - 1));
          out->pages.push_back(static_cast<int>(to - 1));
        }
      }
    }
  }
  if (pd.hDevMode) GlobalFree(pd.hDevMode);
  if (pd.hDevNames) GlobalFree(pd.hDevNames);
  if (pd.hDC) DeleteDC(pd.hDC);
  if (helper) DestroyWindow(helper);
  if (SUCCEEDED(com)) CoUninitialize();
  if (hr != S_OK) *out = Choice();
  return hr;
}

// ---- PDF rendering (Windows.Data.Pdf) and GDI printing ---------------------

namespace {

// The ABI of the Windows.Data.Pdf interfaces used here, as the Windows SDK's
// windows.data.pdf.idl declares them (mingw-w64 has no header for them).
struct Size {
  float Width;
  float Height;
};
struct Rect {
  float X, Y, Width, Height;
};

// Windows.Foundation.IAsyncOperation<PdfDocument>: GetResults is its 9th slot.
struct IPdfDocumentOperation : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(IUnknown** results) = 0;
};

// Windows.Data.Pdf.IPdfDocumentStatics {433A0B5F-C007-4788-90F2-08143D922599}
struct IPdfDocumentStatics : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown* file, IUnknown** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown* file, void* password,
                                                                  IUnknown** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* stream,
                                                        IPdfDocumentOperation** operation) = 0;
  virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown* stream, void* password,
                                                                    IUnknown** operation) = 0;
};

// Windows.Data.Pdf.IPdfDocument {AC7EBEDD-80FA-4089-846E-81B77FF5A86C}
struct IPdfDocument : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IUnknown** page) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* count) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean* value) = 0;
};

// Windows.Data.Pdf.IPdfPage {9DB4B0C8-5320-4CFC-AD76-493FDAD0E594}
struct IPdfPage : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* stream, IUnknown** action) = 0;
  virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* stream, IUnknown* options,
                                                                   IUnknown** action) = 0;
  virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IUnknown** action) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32* index) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(Size* size) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Dimensions(IUnknown** dimensions) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Rotation(int* rotation) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float* zoom) = 0;
};

// Windows.Data.Pdf.IPdfPageRenderOptions {3C98056F-B7CF-4C29-9A04-52D90267F425}
struct IPdfPageRenderOptions : winrt::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_SourceRect(void* rect) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_SourceRect(Rect rect) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(void* color) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(UINT32 color) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(boolean* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(boolean value) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID value) = 0;
};

const IID IID_IPdfDocumentStatics = {0x433A0B5F, 0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
const IID IID_IPdfDocument = {0xAC7EBEDD, 0x80FA, 0x4089, {0x84, 0x6E, 0x81, 0xB7, 0x7F, 0xF5, 0xA8, 0x6C}};
const IID IID_IPdfPage = {0x9DB4B0C8, 0x5320, 0x4CFC, {0xAD, 0x76, 0x49, 0x3F, 0xDA, 0xD0, 0xE5, 0x94}};
const IID IID_IPdfPageRenderOptions = {0x3C98056F, 0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
// Windows.Storage.Streams.IRandomAccessStream {905A0FE1-BC53-11DF-8C49-001E4FC686DA}
const IID IID_IRandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
// CLSID_WICBmpEncoder (BitmapEncoder.BmpEncoderId) and WIC's factory and 32bpp BGR format.
const GUID kBmpEncoder = {0x69BE8BB4, 0xD66D, 0x47C8, {0x86, 0x5A, 0xED, 0x15, 0x89, 0x43, 0x37, 0x82}};
const CLSID kWicFactory = {0xCACAF262, 0x9370, 0x4615, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
const GUID kBgr32 = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0E}};

template <typename T>
class Ref {
 public:
  Ref() = default;
  ~Ref() { reset(); }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  T** put() {
    reset();
    return &p_;
  }
  void** put_void() { return reinterpret_cast<void**>(put()); }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

// shcore's stream bridges, found at run time as combase's WinRT functions are.
using OnFileFn = HRESULT(WINAPI*)(LPCWSTR, int, REFIID, void**);
using OverRandomAccessFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
struct Shcore {
  OnFileFn on_file = nullptr;
  OverRandomAccessFn over_random_access = nullptr;
};
const Shcore& GetShcore() {
  static Shcore api = [] {
    Shcore result;
    HMODULE module = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) {
      result.on_file = reinterpret_cast<OnFileFn>(
          reinterpret_cast<void*>(GetProcAddress(module, "CreateRandomAccessStreamOnFile")));
      result.over_random_access = reinterpret_cast<OverRandomAccessFn>(
          reinterpret_cast<void*>(GetProcAddress(module, "CreateStreamOverRandomAccessStream")));
    }
    return result;
  }();
  return api;
}

constexpr int64_t kAsyncTimeoutMs = 120000;
constexpr LONG kMaxRenderDpi = 300;
constexpr uint64_t kMaxRenderPixels = 64ull * 1024 * 1024;

HRESULT LoadDocument(const std::wstring& path, Ref<IPdfDocument>* document, UINT32* count) {
  const Shcore& shcore = GetShcore();
  if (!shcore.on_file || !shcore.over_random_access) return E_NOTIMPL;
  Ref<IUnknown> file;
  HRESULT hr = shcore.on_file(path.c_str(), 0 /* FileAccessMode::Read */, IID_IRandomAccessStream, file.put_void());
  if (FAILED(hr)) return hr;
  Ref<IPdfDocumentStatics> statics;
  hr = winrt::GetActivationFactory(L"Windows.Data.Pdf.PdfDocument", IID_IPdfDocumentStatics, statics.put_void());
  if (FAILED(hr)) return hr;
  Ref<IPdfDocumentOperation> operation;
  hr = statics->LoadFromStreamAsync(file.get(), operation.put());
  if (FAILED(hr)) return hr;
  hr = winrt::Wait(operation.get(), kAsyncTimeoutMs);
  if (FAILED(hr)) return hr;
  Ref<IUnknown> result;
  hr = operation->GetResults(result.put());
  if (FAILED(hr)) return hr;
  if (!result) return E_UNEXPECTED;
  hr = result->QueryInterface(IID_IPdfDocument, document->put_void());
  if (FAILED(hr)) return hr;
  return (*document)->get_PageCount(count);
}

struct Bitmap {
  UINT width = 0;
  UINT height = 0;
  std::vector<BYTE> pixels;  // 32bpp BGR, top-down
};

HRESULT RenderPage(IPdfDocument* document, UINT32 index, UINT width, UINT height, Bitmap* bitmap) {
  Ref<IUnknown> unknown_page;
  HRESULT hr = document->GetPage(index, unknown_page.put());
  if (FAILED(hr)) return hr;
  Ref<IPdfPage> page;
  hr = unknown_page->QueryInterface(IID_IPdfPage, page.put_void());
  if (FAILED(hr)) return hr;
  Ref<winrt::IInspectable> options_object;
  hr = winrt::ActivateInstance(L"Windows.Data.Pdf.PdfPageRenderOptions", options_object.put());
  if (FAILED(hr)) return hr;
  Ref<IPdfPageRenderOptions> options;
  hr = options_object->QueryInterface(IID_IPdfPageRenderOptions, options.put_void());
  if (SUCCEEDED(hr)) hr = options->put_DestinationWidth(width);
  if (SUCCEEDED(hr)) hr = options->put_DestinationHeight(height);
  if (SUCCEEDED(hr)) hr = options->put_BitmapEncoderId(kBmpEncoder);
  if (FAILED(hr)) return hr;
  Ref<winrt::IInspectable> memory_object;
  hr = winrt::ActivateInstance(L"Windows.Storage.Streams.InMemoryRandomAccessStream", memory_object.put());
  if (FAILED(hr)) return hr;
  Ref<IUnknown> memory;
  hr = memory_object->QueryInterface(IID_IRandomAccessStream, memory.put_void());
  if (FAILED(hr)) return hr;
  Ref<IUnknown> action;
  hr = page->RenderWithOptionsToStreamAsync(memory.get(), options.get(), action.put());
  if (FAILED(hr)) return hr;
  hr = winrt::Wait(action.get(), kAsyncTimeoutMs);
  if (FAILED(hr)) return hr;
  {
    Ref<winrt::IClosable> closable;
    if (SUCCEEDED(page->QueryInterface(winrt::IID_IClosable, closable.put_void()))) closable->Close();
  }
  Ref<IStream> stream;
  hr = GetShcore().over_random_access(memory.get(), __uuidof(IStream), stream.put_void());
  if (FAILED(hr)) return hr;
  LARGE_INTEGER zero = {};
  hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
  if (FAILED(hr)) return hr;
  Ref<IWICImagingFactory> factory;
  hr = CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                        factory.put_void());
  if (FAILED(hr)) return hr;
  Ref<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put());
  if (FAILED(hr)) return hr;
  Ref<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, frame.put());
  if (FAILED(hr)) return hr;
  Ref<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(converter.put());
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(frame.get(), kBgr32, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
  }
  if (SUCCEEDED(hr)) hr = converter->GetSize(&bitmap->width, &bitmap->height);
  if (FAILED(hr)) return hr;
  if (bitmap->width == 0 || bitmap->height == 0 ||
      static_cast<uint64_t>(bitmap->width) * bitmap->height > kMaxRenderPixels) {
    return E_UNEXPECTED;
  }
  const UINT stride = bitmap->width * 4;
  bitmap->pixels.resize(static_cast<size_t>(stride) * bitmap->height);
  return converter->CopyPixels(nullptr, stride, static_cast<UINT>(bitmap->pixels.size()), bitmap->pixels.data());
}

// Draws the bitmap into dest in bands, as some drivers reject one huge transfer.
HRESULT DrawBitmap(HDC dc, const Bitmap& bitmap, LONG x, LONG y, LONG width, LONG height) {
  const UINT band = 256;
  for (UINT row = 0; row < bitmap.height; row += band) {
    UINT rows = std::min(band, bitmap.height - row);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(bitmap.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(rows);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    LONG top = y + static_cast<LONG>(static_cast<int64_t>(height) * row / bitmap.height);
    LONG bottom = y + static_cast<LONG>(static_cast<int64_t>(height) * (row + rows) / bitmap.height);
    if (bottom <= top) continue;
    const BYTE* bits = bitmap.pixels.data() + static_cast<size_t>(row) * bitmap.width * 4;
    if (StretchDIBits(dc, x, top, width, bottom - top, 0, 0, static_cast<int>(bitmap.width),
                      static_cast<int>(rows), bits, &info, DIB_RGB_COLORS, SRCCOPY) == 0) {
      return LastError();
    }
  }
  return S_OK;
}

}  // namespace

HRESULT PrintPdf(const std::wstring& printer, const std::vector<BYTE>& devmode, const std::wstring& pdf,
                 const std::vector<int>& pages, const std::wstring& document_name,
                 const std::wstring& output, bool (*active)(void*), void* context, DWORD* job_id,
                 int* printed_pages) {
  if (job_id) *job_id = 0;
  if (printed_pages) *printed_pages = 0;
  if (pages.size() % 2 != 0) return E_INVALIDARG;
  if (active && !active(context)) return E_ABORT;
  HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HRESULT hr = winrt::EnsureApartment();
  Ref<IPdfDocument> document;
  UINT32 count = 0;
  if (SUCCEEDED(hr)) hr = LoadDocument(pdf, &document, &count);
  std::vector<UINT32> order;
  if (SUCCEEDED(hr)) {
    if (pages.empty()) {
      for (UINT32 i = 0; i < count; ++i) order.push_back(i);
    } else {
      for (size_t i = 0; i < pages.size(); i += 2) {
        if (pages[i] < 0 || pages[i + 1] < pages[i]) {
          hr = E_INVALIDARG;
          break;
        }
        for (int64_t page = pages[i]; page <= pages[i + 1] && page < count; ++page) {
          order.push_back(static_cast<UINT32>(page));
        }
      }
    }
    if (SUCCEEDED(hr) && order.empty()) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  HDC dc = nullptr;
  if (SUCCEEDED(hr) && !PrinterDc(printer, devmode, &dc)) hr = LastError();
  bool started = false;
  if (SUCCEEDED(hr)) {
    DOCINFOW info = {};
    info.cbSize = sizeof(info);
    info.lpszDocName = document_name.c_str();
    info.lpszOutput = output.empty() ? nullptr : output.c_str();
    int job = StartDocW(dc, &info);
    if (job <= 0) {
      hr = LastError();
    } else {
      started = true;
      if (job_id) *job_id = static_cast<DWORD>(job);
    }
  }
  if (SUCCEEDED(hr)) {
    const LONG dpi_x = GetDeviceCaps(dc, LOGPIXELSX), dpi_y = GetDeviceCaps(dc, LOGPIXELSY);
    const LONG paper_w = GetDeviceCaps(dc, PHYSICALWIDTH), paper_h = GetDeviceCaps(dc, PHYSICALHEIGHT);
    const LONG offset_x = GetDeviceCaps(dc, PHYSICALOFFSETX), offset_y = GetDeviceCaps(dc, PHYSICALOFFSETY);
    if (dpi_x <= 0 || dpi_y <= 0 || paper_w <= 0 || paper_h <= 0) hr = E_UNEXPECTED;
    for (size_t i = 0; SUCCEEDED(hr) && i < order.size(); ++i) {
      if (active && !active(context)) {
        hr = E_ABORT;
        break;
      }
      // The app laid its PDF page out for the chosen media, so the page maps onto the
      // whole sheet (device origin is the printable corner); a page of another shape
      // is fitted to it, centered, keeping its proportions.
      Ref<IUnknown> unknown_page;
      Ref<IPdfPage> page;
      Size size = {};
      hr = document->GetPage(order[i], unknown_page.put());
      if (SUCCEEDED(hr)) hr = unknown_page->QueryInterface(IID_IPdfPage, page.put_void());
      if (SUCCEEDED(hr)) hr = page->get_Size(&size);
      if (SUCCEEDED(hr) && (size.Width <= 0 || size.Height <= 0)) hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      page.reset();
      unknown_page.reset();
      if (FAILED(hr)) break;
      double sheet_w = static_cast<double>(paper_w) / dpi_x, sheet_h = static_cast<double>(paper_h) / dpi_y;
      double page_w = size.Width / 96.0, page_h = size.Height / 96.0;  // DIPs to inches
      double scale = std::min(sheet_w / page_w, sheet_h / page_h);
      double draw_w = page_w * scale, draw_h = page_h * scale;  // inches
      LONG dest_w = static_cast<LONG>(draw_w * dpi_x + 0.5), dest_h = static_cast<LONG>(draw_h * dpi_y + 0.5);
      LONG dest_x = static_cast<LONG>((sheet_w - draw_w) / 2 * dpi_x) - offset_x;
      LONG dest_y = static_cast<LONG>((sheet_h - draw_h) / 2 * dpi_y) - offset_y;
      double render_dpi = static_cast<double>(std::min(std::max(dpi_x, dpi_y), kMaxRenderDpi));
      double pixels = draw_w * render_dpi * draw_h * render_dpi;
      if (pixels > static_cast<double>(kMaxRenderPixels)) {
        render_dpi *= std::sqrt(static_cast<double>(kMaxRenderPixels) / pixels);
      }
      UINT render_w = std::max<UINT>(1, static_cast<UINT>(draw_w * render_dpi));
      UINT render_h = std::max<UINT>(1, static_cast<UINT>(draw_h * render_dpi));
      Bitmap bitmap;
      hr = RenderPage(document.get(), order[i], render_w, render_h, &bitmap);
      if (FAILED(hr)) break;
      if (StartPage(dc) <= 0) {
        hr = LastError();
        break;
      }
      hr = DrawBitmap(dc, bitmap, dest_x, dest_y, dest_w, dest_h);
      if (EndPage(dc) <= 0 && SUCCEEDED(hr)) hr = LastError();
      if (SUCCEEDED(hr) && printed_pages) ++*printed_pages;
    }
  }
  if (started) {
    if (SUCCEEDED(hr)) {
      if (EndDoc(dc) <= 0) hr = LastError();
    } else {
      AbortDoc(dc);
    }
  }
  if (dc) DeleteDC(dc);
  document.reset();
  if (SUCCEEDED(com)) CoUninitialize();
  return hr;
}

}  // namespace gesher::print
