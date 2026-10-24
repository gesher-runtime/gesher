// Gesher: the WebView2 probe (docs/webview.md): compat/webview's host without
// Java. A top-level window (red) with a WebView2 in a child window at
// (0, 100, 400x300):
//   1. the loader and the installed runtime's version;
//   2. HTML (green) through NavigateToString whose script calls the bridge
//      host object (gesherBridge.call) synchronously; ExecuteScript reads the
//      result and the title back;
//   3. CapturePreview (PNG, decoded with WIC) is green; PrintWindow with
//      PW_RENDERFULLCONTENT shows the green WebView at its bounds over the
//      red window (reported; DirectComposition content may not be captured);
//   4. a request to https://gesher-file.example/ answered by the listener
//      (WebResourceRequested with a deferral): a blue page.
//
//   webview2_probe.exe [user-data-dir]
// Exit code 0 when 1, 2, 3 (preview) and 4 pass; 2 when no runtime is
// installed.

#include <stdio.h>
#include <string.h>
#include <wincodec.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "webview_host.h"

using gesher::webview::Host;
using gesher::webview::HostOptions;

namespace {

std::mutex g_mutex;
std::vector<std::wstring> g_log;
std::atomic<int> g_finished{0};
std::wstring g_script_result;
std::atomic<bool> g_script_done{false};
std::vector<uint8_t> g_png;
std::atomic<bool> g_png_done{false};
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<int> g_bridge_calls{0};
Host* g_host = nullptr;

void Log(const std::wstring& s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  wprintf(L"probe: %ls\n", s.c_str());
  fflush(stdout);
}

class ProbeListener final : public gesher::webview::Listener {
 public:
  void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& a, const std::wstring& b,
               const std::wstring&, const std::vector<uint8_t>* bytes) override {
    switch (type) {
      case gesher::webview::kReady:
        Log(L"ready, runtime " + a);
        g_ready = true;
        break;
      case gesher::webview::kFailed:
        Log(L"failed: " + a);
        g_failed = true;
        break;
      case gesher::webview::kPageStarted:
        Log(L"page started: " + a);
        break;
      case gesher::webview::kPageFinished:
        Log(L"page finished: " + a + L" (status " + std::to_wstring(arg) + L", http " +
            std::to_wstring(id) + L")");
        ++g_finished;
        break;
      case gesher::webview::kTitle:
        Log(L"title: " + a);
        break;
      case gesher::webview::kConsole:
        Log(L"console " + a + L": " + b.substr(0, 200));
        break;
      case gesher::webview::kScriptResult:
        g_script_result = a;
        g_script_done = true;
        break;
      case gesher::webview::kCapture:
        if (bytes != nullptr) g_png = *bytes;
        Log(L"capture: " + std::to_wstring(g_png.size()) + L" bytes, hr " + std::to_wstring(arg));
        g_png_done = true;
        break;
      default:
        break;
    }
  }
  bool OnNavigationStarting(const std::wstring& url, bool, bool) override {
    Log(L"navigation starting (page): " + url);
    return false;
  }
  void OnResourceRequest(int64_t id, const std::wstring& url, const std::wstring& method,
                         const std::wstring&, int32_t context, bool main_frame) override {
    Log(L"request " + method + L" " + url + L" context " + std::to_wstring(context) +
        (main_frame ? L" (main frame)" : L""));
    if (url.rfind(gesher::webview::kFileOrigin, 0) == 0) {
      std::string html =
          "<!doctype html><html><body style='margin:0;background:#0000ff'>"
          "<script>document.title='served'</script></body></html>";
      g_host->RespondToRequest(id, 200, L"OK", L"Content-Type: text/html\r\n",
                               std::vector<uint8_t>(html.begin(), html.end()));
    } else {
      g_host->RespondToRequest(id, 0, L"", L"", {});
    }
  }
  std::wstring OnBridgeCall(const std::wstring& object, const std::wstring& method,
                            const std::wstring& args) override {
    ++g_bridge_calls;
    Log(L"bridge call " + object + L"." + method + L"(" + args + L")");
    if (method == L"add") return L"{\"v\":5}";
    return L"{\"e\":\"no such method\"}";
  }
  void OnDestroyed() override {}
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
    FillRect(dc, &ps.rcPaint, red);
    DeleteObject(red);
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Pumps this thread's messages until done() or the timeout.
bool Wait(int ms, bool (*done)()) {
  DWORD end = GetTickCount() + ms;
  while (!done()) {
    if (static_cast<int>(end - GetTickCount()) <= 0) return false;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return true;
}

void Pump(int ms) {
  Wait(ms, [] { return false; });
}

// The PNG's pixel at (x, y) as 0xRRGGBB, or -1.
long PngPixel(const std::vector<uint8_t>& png, int fx, int fy, int* width, int* height) {
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory, reinterpret_cast<void**>(&factory)))) {
    return -1;
  }
  long out = -1;
  IStream* stream = SHCreateMemStream(png.data(), static_cast<UINT>(png.size()));
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;
  if (stream != nullptr &&
      SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                 &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame)) &&
      SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom))) {
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    int x = fx < 0 ? static_cast<int>(w) / 2 : fx, y = fy < 0 ? static_cast<int>(h) / 2 : fy;
    uint32_t px = 0;
    WICRect r = {x, y, 1, 1};
    if (x < static_cast<int>(w) && y < static_cast<int>(h) &&
        SUCCEEDED(converter->CopyPixels(&r, 4, 4, reinterpret_cast<BYTE*>(&px)))) {
      out = static_cast<long>(px & 0xffffff);
    }
  }
  if (converter) converter->Release();
  if (frame) frame->Release();
  if (decoder) decoder->Release();
  if (stream) stream->Release();
  factory->Release();
  return out;
}

// PrintWindow(PW_RENDERFULLCONTENT) of the window's client area at (x, y).
long PrintPixel(HWND hwnd, int x, int y) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  RECT wr;
  GetWindowRect(hwnd, &wr);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, bmp);
  long out = -1;
  if (PrintWindow(hwnd, dc, 2 /* PW_RENDERFULLCONTENT */)) {
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    int cx = origin.x - wr.left + x, cy = origin.y - wr.top + y;
    if (cx >= 0 && cy >= 0 && cx < w && cy < h) {
      out = static_cast<long>(static_cast<uint32_t*>(bits)[cy * w + cx] & 0xffffff);
    }
  }
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return out;
}

bool IsColor(long px, long want) {
  if (px < 0) return false;
  for (int shift = 0; shift < 24; shift += 8) {
    int a = (px >> shift) & 0xff, b = (want >> shift) & 0xff;
    if (a - b > 24 || b - a > 24) return false;
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  _putenv("GESHER_WEBVIEW_DEBUG=1");  // the bridge reports how it is called
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int failures = 0;
  Log(std::wstring(L"loader: ") + std::wstring(gesher::webview::LoaderKind(),
                                              gesher::webview::LoaderKind() +
                                                  strlen(gesher::webview::LoaderKind())));
  HRESULT hr = S_OK;
  std::wstring version = Host::RuntimeVersion(&hr);
  if (version.empty()) {
    wprintf(L"FAIL  no WebView2 Runtime (hr 0x%08lx)\n", static_cast<unsigned long>(hr));
    return 2;
  }
  wprintf(L"PASS  webview2: the loader finds the WebView2 Runtime %ls\n", version.c_str());

  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  std::wstring user_data = argc > 1 ? argv[1] : std::wstring(temp) + L"gesher-webview2-probe";
  Host::Configure(user_data, L"");

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherWebViewProbe";
  RegisterClassExW(&wc);
  RECT r = {0, 0, 400, 500};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Gesher WebView2 probe",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 50, 50, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);

  ProbeListener listener;
  HostOptions options;
  options.parent = hwnd;
  g_host = Host::Create(&listener, options);
  Host::Settings settings;
  settings.javascript = true;
  g_host->SetSettings(settings);
  g_host->SetBounds(hwnd, 0, 100, 400, 300, 0, 100, 400, 300, true);
  g_host->SetDocumentScript(
      L"window.bridgeResult = (function(){ try {"
      L" return chrome.webview.hostObjects.sync.gesherBridge.call('probe','add','[2,3]');"
      L" } catch (e) { return 'error: ' + e; } })();");
  g_host->NavigateToString(
      L"<!doctype html><html><head><title>probe</title></head>"
      L"<body style='margin:0;background:#00ff00'>"
      L"<script>console.log('bridge said ' + window.bridgeResult)</script></body></html>");

  if (!Wait(60000, [] { return g_finished.load() >= 1 || g_failed.load(); }) || g_failed) {
    wprintf(L"FAIL  webview2: the page did not load\n");
    return 1;
  }
  Pump(1000);
  g_host->ExecuteScript(L"document.title + '|' + window.bridgeResult", 1);
  Wait(20000, [] { return g_script_done.load(); });
  std::wstring want = L"\"probe|{\\\"v\\\":5}\"";
  if (g_script_result == want && g_bridge_calls > 0) {
    wprintf(L"PASS  webview2: HTML with a script calling the bridge host object synchronously, "
            L"ExecuteScript returns %ls\n", g_script_result.c_str());
  } else {
    wprintf(L"FAIL  webview2: ExecuteScript returned %ls (wanted %ls), %d bridge calls\n",
            g_script_result.c_str(), want.c_str(), g_bridge_calls.load());
    ++failures;
  }

  g_host->CapturePreview(2);
  Wait(20000, [] { return g_png_done.load(); });
  int pw = 0, ph = 0;
  long center = g_png.empty() ? -1 : PngPixel(g_png, -1, -1, &pw, &ph);
  if (IsColor(center, 0x00ff00)) {
    wprintf(L"PASS  webview2: CapturePreview %dx%d, green (0x%06lx)\n", pw, ph, center);
  } else {
    wprintf(L"FAIL  webview2: CapturePreview %dx%d center 0x%06lx\n", pw, ph, center);
    ++failures;
  }
  long in_view = PrintPixel(hwnd, 200, 250), above = PrintPixel(hwnd, 200, 50);
  wprintf(L"%ls  webview2: PrintWindow(PW_RENDERFULLCONTENT): 0x%06lx in the WebView's bounds, "
          L"0x%06lx above them\n",
          IsColor(in_view, 0x00ff00) && IsColor(above, 0xff0000) ? L"PASS" : L"INFO", in_view,
          above);

  g_host->Navigate(std::wstring(gesher::webview::kFileOrigin) + L"/probe/index.html", L"");
  if (!Wait(30000, [] { return g_finished.load() >= 2; })) {
    wprintf(L"FAIL  webview2: the served page did not load\n");
    ++failures;
  } else {
    Pump(1000);
    g_png_done = false;
    g_host->CapturePreview(3);
    Wait(20000, [] { return g_png_done.load(); });
    long blue = g_png.empty() ? -1 : PngPixel(g_png, -1, -1, &pw, &ph);
    if (IsColor(blue, 0x0000ff)) {
      wprintf(L"PASS  webview2: a page served by WebResourceRequested (deferred), blue\n");
    } else {
      wprintf(L"FAIL  webview2: the served page shows 0x%06lx\n", blue);
      ++failures;
    }
  }

  g_host->Destroy();
  Pump(500);
  DestroyWindow(hwnd);
  return failures == 0 ? 0 : 1;
}
