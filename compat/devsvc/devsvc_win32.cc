// Gesher: the Windows side of the device-state system services (devsvc track);
// see devsvc_win32.h. Toasts (WinRT) are devsvc_toast.c's.

#include "devsvc_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netlistmgr.h>

#include "devsvc_toast.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <utility>

namespace gesher {
namespace devsvc {
namespace {

const wchar_t* W(const std::u16string& s) { return reinterpret_cast<const wchar_t*>(s.c_str()); }

std::u16string U16(const wchar_t* s, size_t n) {
  return std::u16string(reinterpret_cast<const char16_t*>(s), n);
}

std::u16string U16(const wchar_t* s) { return s ? U16(s, wcslen(s)) : std::u16string(); }

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

// --- the event window ---------------------------------------------------------------

struct Activation {
  std::u16string app_id, tag, group, arguments;
};

SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_cond = CONDITION_VARIABLE_INIT;
unsigned g_pending = 0;                // guarded by g_lock
std::deque<Activation> g_activations;  // guarded by g_lock
HWND g_window = nullptr;
INIT_ONCE g_events_once = INIT_ONCE_STATIC_INIT;

void Post(unsigned events) {
  AcquireSRWLockExclusive(&g_lock);
  g_pending |= events;
  ReleaseSRWLockExclusive(&g_lock);
  WakeAllConditionVariable(&g_cond);
}

LRESULT CALLBACK EventProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLIPBOARDUPDATE:
      Post(kClipboard);
      return 0;
    case WM_TIMECHANGE:
      Post(kTimeChanged);
      return 0;
    case WM_SETTINGCHANGE:
      Post(kSettingChanged);
      return 0;
    case WM_POWERBROADCAST:
      switch (wp) {
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
          Post(kResumed);
          break;
        case PBT_APMSUSPEND:
          Post(kSuspending);
          break;
        case PBT_APMPOWERSTATUSCHANGE:
          Post(kPowerStatus);
          break;
        default:
          break;
      }
      return TRUE;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

DWORD WINAPI EventThread(void* ready) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = EventProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GesherDeviceServices";
  RegisterClassExW(&wc);
  // A top-level window that is never shown: message-only windows get no
  // broadcasts (WM_TIMECHANGE, WM_POWERBROADCAST, WM_SETTINGCHANGE).
  HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                              L"Gesher device services", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, wc.hInstance, nullptr);
  if (hwnd != nullptr) AddClipboardFormatListener(hwnd);
  g_window = hwnd;
  SetEvent(static_cast<HANDLE>(ready));
  if (hwnd == nullptr) return 1;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

BOOL CALLBACK StartOnce(PINIT_ONCE, void*, void**) {
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE thread = CreateThread(nullptr, 0, EventThread, ready, 0, nullptr);
  if (thread != nullptr) {
    WaitForSingleObject(ready, 10000);
    CloseHandle(thread);
  }
  CloseHandle(ready);
  return TRUE;
}

// --- COM ---------------------------------------------------------------------------

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
  void reset() {
    if (p_ != nullptr) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

// COM in the multithreaded apartment on every thread that calls in (the system
// server's binder threads); never torn down.
HRESULT EnsureCom() {
  thread_local HRESULT hr = [] {
    HRESULT r = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // A thread already in an apartment (RPC_E_CHANGED_MODE) can still call.
    return r == RPC_E_CHANGED_MODE ? S_OK : r;
  }();
  return FAILED(hr) ? hr : S_OK;
}

// --- networks -----------------------------------------------------------------------

// {DCB00C01-570F-4A9B-8D69-199FDBA5723B}, {DCB00000-...}, {DCB00008-...}
const GUID kClsidNetworkListManager = {0xDCB00C01, 0x570F, 0x4A9B,
                                       {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const GUID kIidNetworkListManager = {0xDCB00000, 0x570F, 0x4A9B,
                                     {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
const GUID kIidNetworkCostManager = {0xDCB00008, 0x570F, 0x4A9B,
                                     {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

// --- alarms ---------------------------------------------------------------------------

HANDLE AlarmTimer() {
  static HANDLE h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  return h;
}

HANDLE AlarmKickEvent() {
  static HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return h;
}

SRWLOCK g_power_lock = SRWLOCK_INIT;
HANDLE g_power_request = nullptr;  // guarded by g_power_lock

bool OpenClipboardRetrying() {
  for (int i = 0; i < 20; i++) {
    if (OpenClipboard(g_window)) return true;
    Sleep(10);  // another process has it open
  }
  return false;
}

}  // namespace

// --- events ---------------------------------------------------------------------------

unsigned long long StartEvents() {
  InitOnceExecuteOnce(&g_events_once, StartOnce, nullptr, nullptr);
  return reinterpret_cast<unsigned long long>(g_window);
}

unsigned WaitEvents(int timeout_ms) {
  AcquireSRWLockExclusive(&g_lock);
  if (g_pending == 0) {
    ULONGLONG start = GetTickCount64();
    while (g_pending == 0) {
      DWORD wait = INFINITE;
      if (timeout_ms >= 0) {
        ULONGLONG spent = GetTickCount64() - start;
        if (spent >= static_cast<ULONGLONG>(timeout_ms)) break;
        wait = static_cast<DWORD>(timeout_ms - spent);
      }
      if (!SleepConditionVariableSRW(&g_cond, &g_lock, wait, 0) &&
          GetLastError() == ERROR_TIMEOUT) {
        break;
      }
    }
  }
  unsigned events = g_pending;
  g_pending = 0;
  ReleaseSRWLockExclusive(&g_lock);
  return events;
}

bool PopActivation(std::u16string* app_id, std::u16string* tag, std::u16string* group,
                   std::u16string* arguments) {
  AcquireSRWLockExclusive(&g_lock);
  bool any = !g_activations.empty();
  if (any) {
    Activation a = std::move(g_activations.front());
    g_activations.pop_front();
    *app_id = std::move(a.app_id);
    *tag = std::move(a.tag);
    *group = std::move(a.group);
    *arguments = std::move(a.arguments);
  }
  ReleaseSRWLockExclusive(&g_lock);
  return any;
}

}  // namespace devsvc
}  // namespace gesher

// devsvc_toast.c's Activated handler: a click, queued for WaitEvents' callers.
extern "C" void gesher_toast_activated(const wchar_t* app_id, const wchar_t* tag,
                                       const wchar_t* group, const wchar_t* arguments) {
  using namespace gesher::devsvc;
  Activation a;
  a.app_id = U16(app_id);
  a.tag = U16(tag);
  a.group = U16(group);
  a.arguments = U16(arguments);
  AcquireSRWLockExclusive(&g_lock);
  g_activations.push_back(std::move(a));
  g_pending |= kToastActivated;
  ReleaseSRWLockExclusive(&g_lock);
  WakeAllConditionVariable(&g_cond);
}

namespace gesher {
namespace devsvc {

// --- clipboard ------------------------------------------------------------------------

bool ClipboardText(std::u16string* text) {
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboardRetrying()) return false;
  bool ok = false;
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
  CloseClipboard();
  return ok;
}

namespace {

UINT HtmlFormat() {
  static UINT f = RegisterClipboardFormatW(L"HTML Format");
  return f;
}

long HeaderNumber(const std::string& data, const char* name) {
  size_t at = data.find(name);
  if (at == std::string::npos) return -1;
  return std::strtol(data.c_str() + at + std::strlen(name), nullptr, 10);
}

// CF_HTML around a fragment (UTF-8; the offsets count its bytes).
std::string CfHtml(const std::string& fragment) {
  const char* kHeader =
      "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
      "StartFragment:%010zu\r\nEndFragment:%010zu\r\n";
  const std::string kBefore = "<html><body>\r\n<!--StartFragment-->";
  const std::string kAfter = "<!--EndFragment-->\r\n</body>\r\n</html>";
  char probe[256];
  size_t header = static_cast<size_t>(std::snprintf(probe, sizeof(probe), kHeader,
                                                    size_t{0}, size_t{0}, size_t{0},
                                                    size_t{0}));
  size_t start_fragment = header + kBefore.size();
  size_t end_fragment = start_fragment + fragment.size();
  size_t end_html = end_fragment + kAfter.size();
  char head[256];
  std::snprintf(head, sizeof(head), kHeader, header, end_html, start_fragment, end_fragment);
  return std::string(head) + kBefore + fragment + kAfter;
}

}  // namespace

bool ClipboardHtml(std::u16string* html) {
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

bool SetClipboard(const std::u16string& text, const std::u16string& html) {
  if (!OpenClipboardRetrying()) return false;
  bool ok = EmptyClipboard() != 0;
  if (ok && !text.empty()) {
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g != nullptr) {
      void* p = GlobalLock(g);
      std::memcpy(p, text.c_str(), bytes);
      GlobalUnlock(g);
      if (SetClipboardData(CF_UNICODETEXT, g) == nullptr) {
        GlobalFree(g);
        ok = false;
      }
    } else {
      ok = false;
    }
  }
  if (ok && !html.empty() && HtmlFormat() != 0) {
    std::string data = CfHtml(Utf8(html));
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, data.size() + 1);
    if (g != nullptr) {
      void* p = GlobalLock(g);
      std::memcpy(p, data.c_str(), data.size() + 1);
      GlobalUnlock(g);
      if (SetClipboardData(HtmlFormat(), g) == nullptr) GlobalFree(g);
    }
  }
  CloseClipboard();
  return ok;
}

uint32_t ClipboardSequence() { return GetClipboardSequenceNumber(); }

// --- alarms ---------------------------------------------------------------------------

int AlarmWait(int64_t utc_ms, bool wakeup, int timeout_ms) {
  HANDLE timer = AlarmTimer();
  HANDLE kick = AlarmKickEvent();
  if (timer == nullptr || kick == nullptr) {
    Sleep(timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0);
    return 2;
  }
  // FILETIME: 100 ns units since 1601; positive: an absolute (wall clock) time.
  const int64_t kEpochDeltaMs = 11644473600000LL;
  LARGE_INTEGER due;
  int64_t ms = utc_ms < 0 ? 0 : utc_ms;
  due.QuadPart = (ms + kEpochDeltaMs) * 10000LL;
  SetWaitableTimer(timer, &due, 0, nullptr, nullptr, wakeup ? TRUE : FALSE);
  HANDLE handles[2] = {timer, kick};
  DWORD r = WaitForMultipleObjects(2, handles, FALSE,
                                   timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  CancelWaitableTimer(timer);
  if (r == WAIT_OBJECT_0) return 0;
  if (r == WAIT_OBJECT_0 + 1) return 1;
  return 2;
}

void AlarmKick() {
  HANDLE kick = AlarmKickEvent();
  if (kick != nullptr) SetEvent(kick);
}

// --- power ----------------------------------------------------------------------------

bool PowerStatus(int out[4]) {
  SYSTEM_POWER_STATUS s;
  if (!GetSystemPowerStatus(&s)) return false;
  out[0] = s.ACLineStatus;
  out[1] = s.BatteryFlag;
  out[2] = s.BatteryLifePercent;
  out[3] = s.SystemStatusFlag;
  return true;
}

bool KeepAwake(const std::u16string& reason, bool system, bool display) {
  AcquireSRWLockExclusive(&g_power_lock);
  if (g_power_request != nullptr) {
    PowerClearRequest(g_power_request, PowerRequestSystemRequired);
    PowerClearRequest(g_power_request, PowerRequestDisplayRequired);
    CloseHandle(g_power_request);
    g_power_request = nullptr;
  }
  bool ok = true;
  if (system || display) {
    std::wstring text(W(reason));
    REASON_CONTEXT context = {};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = text.data();
    HANDLE h = PowerCreateRequest(&context);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
      ok = false;
    } else {
      if (system) ok &= PowerSetRequest(h, PowerRequestSystemRequired) != 0;
      if (display) ok &= PowerSetRequest(h, PowerRequestDisplayRequired) != 0;
      g_power_request = h;
    }
  }
  ReleaseSRWLockExclusive(&g_power_lock);
  return ok;
}

uint32_t IdleMillis() {
  LASTINPUTINFO info = {};
  info.cbSize = sizeof(info);
  if (!GetLastInputInfo(&info)) return 0;
  return GetTickCount() - info.dwTime;
}

int NetworkState() {
  if (FAILED(EnsureCom())) return -1;
  Ref<INetworkListManager> nlm;
  if (FAILED(CoCreateInstance(kClsidNetworkListManager, nullptr, CLSCTX_ALL,
                              kIidNetworkListManager, nlm.put_void()))) {
    return -1;
  }
  int state = 0;
  VARIANT_BOOL b = VARIANT_FALSE;
  if (SUCCEEDED(nlm->IsConnected(&b)) && b != VARIANT_FALSE) state |= 1;
  b = VARIANT_FALSE;
  if (SUCCEEDED(nlm->IsConnectedToInternet(&b)) && b != VARIANT_FALSE) state |= 2;
  Ref<INetworkCostManager> cost;
  if (SUCCEEDED(nlm->QueryInterface(kIidNetworkCostManager, cost.put_void()))) {
    DWORD c = 0;
    if (SUCCEEDED(cost->GetCost(&c, nullptr)) &&
        c != NLM_CONNECTION_COST_UNKNOWN &&
        (c & NLM_CONNECTION_COST_UNRESTRICTED) == 0) {
      state |= 4;
    }
  }
  return state;
}

// --- time zone --------------------------------------------------------------------------

std::u16string TimeZoneKeyName() {
  DYNAMIC_TIME_ZONE_INFORMATION tz = {};
  if (GetDynamicTimeZoneInformation(&tz) == TIME_ZONE_ID_INVALID) return std::u16string();
  return U16(tz.TimeZoneKeyName);
}

// --- toasts -----------------------------------------------------------------------------

long ShowToast(const std::u16string& app_id, const std::u16string& tag,
               const std::u16string& group, const std::u16string& xml, bool suppress_popup,
               bool high) {
  HRESULT hr = EnsureCom();
  if (FAILED(hr)) return hr;
  return gesher_toast_show(W(app_id), W(tag), W(group), W(xml), suppress_popup ? 1 : 0,
                           high ? 1 : 0);
}

long RemoveToast(const std::u16string& app_id, const std::u16string& tag,
                 const std::u16string& group) {
  HRESULT hr = EnsureCom();
  if (FAILED(hr)) return hr;
  return gesher_toast_remove(W(app_id), W(tag), W(group));
}

long ClearToasts(const std::u16string& app_id) {
  HRESULT hr = EnsureCom();
  if (FAILED(hr)) return hr;
  return gesher_toast_clear(W(app_id));
}

long RegisterAppId(const std::u16string& app_id, const std::u16string& display_name,
                   const std::u16string& icon) {
  std::wstring key = L"Software\\Classes\\AppUserModelId\\";
  key += W(app_id);
  HKEY h = nullptr;
  LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                           nullptr, &h, nullptr);
  if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
  r = RegSetValueExW(h, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(W(display_name)),
                     static_cast<DWORD>((display_name.size() + 1) * sizeof(wchar_t)));
  if (r == ERROR_SUCCESS && !icon.empty()) {
    r = RegSetValueExW(h, L"IconUri", 0, REG_SZ, reinterpret_cast<const BYTE*>(W(icon)),
                       static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(h);
  return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

long UnregisterAppId(const std::u16string& app_id) {
  std::wstring key = L"Software\\Classes\\AppUserModelId\\";
  key += W(app_id);
  LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) return S_OK;
  return HRESULT_FROM_WIN32(r);
}

std::u16string FullPath(const std::u16string& path) {
  std::wstring in(W(path));
  for (wchar_t& c : in) {
    if (c == L'/') c = L'\\';
  }
  DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
  if (n == 0) return path;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(in.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return U16(out.c_str(), out.size());
}

}  // namespace devsvc
}  // namespace gesher
