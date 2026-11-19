// Gesher: the Windows side of the device-state system services (devsvc track);
// see devsvc_win32.h. Toasts (WinRT) are devsvc_toast.c's.

#include "devsvc_win32.h"
#include "toast_identity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>

#include "devsvc_toast.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <new>
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
// GUID_SESSION_DISPLAY_STATUS: this process runs in the Android apps' Windows
// session. A still image / idle keyboard with the display on is not device idle.
constexpr GUID kSessionDisplayStatus = {
    0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}};
int g_session_display_state = -1;      // guarded by g_lock; unknown remains distinct
uint64_t g_display_off_since = 0;      // GetTickCount64 (elapsed, including suspend)

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
        case PBT_POWERSETTINGCHANGE: {
          const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
          if (setting != nullptr && setting->DataLength == sizeof(DWORD)
              && IsEqualGUID(setting->PowerSetting, kSessionDisplayStatus)) {
            DWORD state = 0;
            std::memcpy(&state, setting->Data, sizeof(state));
            AcquireSRWLockExclusive(&g_lock);
            int observed = state <= 2 ? static_cast<int>(state) : -1;
            if (observed == 0 && g_session_display_state != 0) g_display_off_since = GetTickCount64();
            g_session_display_state = observed; // Dimmed (2) remains interactive.
            ReleaseSRWLockExclusive(&g_lock);
            Post(kDisplayState);
          }
          break;
        }
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
  HPOWERNOTIFY display_notification = nullptr;
  if (hwnd != nullptr) {
    AddClipboardFormatListener(hwnd);
    display_notification = RegisterPowerSettingNotification(
        hwnd, &kSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
  }
  g_window = hwnd;
  SetEvent(static_cast<HANDLE>(ready));
  if (hwnd == nullptr) return 1;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (display_notification != nullptr) UnregisterPowerSettingNotification(display_notification);
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

bool IsLocalDocument(const std::u16string& path) {
  if (path.empty()) return false;
  // OPEN_EXISTING cannot create anything. Do not use OPEN_REPARSE_POINT: the
  // actual target must be local, including mapped drives and directory junctions.
  HANDLE file = CreateFileW(W(path), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_NO_RECALL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  bool available = GetFileInformationByHandleEx(file, FileAttributeTagInfo,
      &attributes, sizeof(attributes))
      && !(attributes.FileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS));
  std::wstring final_path;
  if (available) {
    // Network shares have no volume GUID. Windows resolves reparse targets before
    // returning this path; an input such as C:\\link or X:\\file is not evidence of locality.
    DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, VOLUME_NAME_GUID | FILE_NAME_NORMALIZED);
    available = size != 0;
    if (available) {
      final_path.resize(size);
      DWORD length = GetFinalPathNameByHandleW(file, final_path.data(), size,
          VOLUME_NAME_GUID | FILE_NAME_NORMALIZED);
      available = length != 0 && length < size;
      if (available) final_path.resize(length);
    }
  }
  CloseHandle(file);
  if (!available || final_path.rfind(L"\\\\?\\Volume{", 0) != 0) return false;
  size_t end = final_path.find(L"}\\", 11);
  if (end == std::wstring::npos) return false;
  std::wstring volume = final_path.substr(0, end + 2);
  switch (GetDriveTypeW(volume.c_str())) {
    case DRIVE_FIXED: case DRIVE_REMOVABLE: case DRIVE_CDROM: case DRIVE_RAMDISK:
      return true;
    default: return false;
  }
}

namespace {

struct DocumentLifetime {
  IFileDialog* dialog;
  bool (*active)(void*);
  void* context;
};

LRESULT CALLBACK DocumentLifetimeProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
  } else if (message == WM_TIMER) {
    auto* state = reinterpret_cast<DocumentLifetime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state && !state->active(state->context)) {
      state->dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
  }
  return DefWindowProcW(window, message, wp, lp);
}

BOOL CALLBACK RegisterDocumentLifetime(PINIT_ONCE, PVOID, PVOID*) {
  WNDCLASSW cls{};
  cls.lpfnWndProc = DocumentLifetimeProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"GesherDocumentPickerLifetime";
  return RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

struct DocumentOwner { DWORD pid; HWND window; };
BOOL CALLBACK FindDocumentOwner(HWND window, LPARAM parameter) {
  auto* owner = reinterpret_cast<DocumentOwner*>(parameter);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == owner->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
    owner->window = window;
    return FALSE;
  }
  return TRUE;
}

HRESULT DocumentPath(IShellItem* item, std::vector<std::u16string>* paths) {
  PWSTR value = nullptr;
  HRESULT status = item->GetDisplayName(SIGDN_FILESYSPATH, &value);
  if (SUCCEEDED(status)) {
    if (!value || !*value || wcslen(value) > 32768) status = E_INVALIDARG;
    else paths->push_back(U16(value));
  }
  CoTaskMemFree(value);
  return status;
}

HRESULT DocumentPaths(IFileDialog* dialog, int mode, bool multiple,
    std::vector<std::u16string>* paths) {
  paths->clear();
  if (mode == 1) {
    IShellItem* item = nullptr;
    HRESULT status = dialog->GetResult(&item);
    if (SUCCEEDED(status)) { status = DocumentPath(item, paths); item->Release(); }
    return status;
  }
  IFileOpenDialog* open = nullptr;
  HRESULT status = dialog->QueryInterface(IID_PPV_ARGS(&open));
  if (SUCCEEDED(status)) {
    IShellItemArray* items = nullptr;
    status = open->GetResults(&items);
    if (SUCCEEDED(status)) {
      DWORD count = 0;
      status = items->GetCount(&count);
      if (SUCCEEDED(status) && (count == 0 || (!multiple && count != 1))) status = E_INVALIDARG;
      // JNI arrays use signed 32-bit sizes. There is no Android selection-count
      // policy below that representation limit (nor a provider-specific 256 cap).
      if (SUCCEEDED(status) && (count > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
          || count > paths->max_size())) status = E_OUTOFMEMORY;
      for (DWORD i = 0; SUCCEEDED(status) && i < count; ++i) {
        IShellItem* item = nullptr;
        status = items->GetItemAt(i, &item);
        if (SUCCEEDED(status)) { status = DocumentPath(item, paths); item->Release(); }
      }
      items->Release();
    }
    open->Release();
  }
  return status;
}

bool DocumentPathsAreLocal(int mode, const std::vector<std::u16string>& paths) {
  if (paths.empty()) return false;
  for (const auto& path : paths) {
    if (mode == 1) {
      // A save target need not exist. Query its existing parent without creating
      // even a test file; the provider alone performs atomic document creation.
      size_t slash = path.find_last_of(u"\\/");
      if (slash == std::u16string::npos || !IsLocalDocument(path.substr(0, slash + 1))) return false;
    } else if (!IsLocalDocument(path)) return false;
  }
  return true;
}

// OnFileOk returns S_FALSE to keep an invalid selection in the real dialog, so
// the user can choose a local target or cancel. No alternate document is returned.
class DocumentLocalEvents final : public IFileDialogEvents {
 public:
  DocumentLocalEvents(int mode, bool multiple, IFileDialogCustomize* custom,
      DocumentLifetime* lifetime) : mode_(mode), multiple_(multiple), custom_(custom), lifetime_(lifetime) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) {
      *result = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG result = InterlockedDecrement(&references_);
    if (!result) delete this;
    return result;
  }
  HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
    if (!lifetime_->active(lifetime_->context)) {
      dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)); return S_FALSE;
    }
    std::vector<std::u16string> paths;
    HRESULT status = DocumentPaths(dialog, mode_, multiple_, &paths);
    if (SUCCEEDED(status) && DocumentPathsAreLocal(mode_, paths)) return S_OK;
    custom_->SetControlLabel(1, L"This selection is not available locally. Choose a local file or folder.");
    return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
    *response = FDESVR_DEFAULT; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
    *response = FDEOR_DEFAULT; return S_OK;
  }
 private:
  LONG references_ = 1;
  int mode_;
  bool multiple_;
  // Both outlive the Advise registration and this object on the same STA thread.
  IFileDialogCustomize* custom_;
  DocumentLifetime* lifetime_;
};

}  // namespace

DocumentSelection PickDocuments(int mode, const std::u16string& title,
    const std::u16string& suggested_name, const std::u16string& initial_directory,
    const std::u16string& filter, const std::u16string& extension, bool multiple,
    bool local_only, uint32_t owner_pid, bool (*active)(void*), void* context) {
  DocumentSelection selection;
  if (mode < 0 || mode > 2 || !active || (mode != 0 && multiple)) {
    selection.error = E_INVALIDARG; return selection;
  }
  if (!active(context)) return selection;
  HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(status)) { selection.error = status; return selection; }
  IFileDialog* dialog = nullptr;
  status = CoCreateInstance(mode == 1 ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
      nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  HWND lifetime_window = nullptr;
  DocumentLifetime lifetime{dialog, active, context};
  IFileDialogCustomize* custom = nullptr;
  DocumentLocalEvents* events = nullptr;
  DWORD cookie = 0;
  bool advised = false;
  if (SUCCEEDED(status)) {
    FILEOPENDIALOGOPTIONS options = 0;
    status = dialog->GetOptions(&options);
    if (SUCCEEDED(status)) {
      options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
      // ACTION_CREATE_DOCUMENT creates a new document, including when a chosen
      // display name already exists. Uniqueness and atomic creation are in Java.
      options &= ~(FOS_FILEMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_ALLOWMULTISELECT | FOS_PICKFOLDERS);
      if (mode != 1) options |= FOS_FILEMUSTEXIST;
      if (mode == 2) options |= FOS_PICKFOLDERS;
      if (multiple) options |= FOS_ALLOWMULTISELECT;
      if (mode == 1 && local_only) options |= FOS_NOTESTFILECREATE;
      status = dialog->SetOptions(options);
    }
    if (SUCCEEDED(status) && !title.empty()) status = dialog->SetTitle(W(title));
    if (SUCCEEDED(status) && mode == 1 && !suggested_name.empty()) {
      status = dialog->SetFileName(W(suggested_name));
    }
    if (SUCCEEDED(status) && mode != 2 && !filter.empty()) {
      COMDLG_FILTERSPEC spec{L"Requested document types", W(filter)};
      status = dialog->SetFileTypes(1, &spec);
    }
    if (SUCCEEDED(status) && mode == 1 && !extension.empty()) {
      status = dialog->SetDefaultExtension(W(extension));
    }
    if (SUCCEEDED(status) && !initial_directory.empty()) {
      IShellItem* initial = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(W(initial_directory), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial); // EXTRA_INITIAL_URI is an advisory hint.
        initial->Release();
      }
    }
    if (SUCCEEDED(status) && local_only) {
      status = dialog->QueryInterface(IID_PPV_ARGS(&custom));
      if (SUCCEEDED(status)) status = custom->AddText(1, L"Choose files or folders available on this computer.");
      if (SUCCEEDED(status)) {
        events = new (std::nothrow) DocumentLocalEvents(mode, multiple, custom, &lifetime);
        if (!events) status = E_OUTOFMEMORY;
        else { status = dialog->Advise(events, &cookie); advised = SUCCEEDED(status); }
      }
    }
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (SUCCEEDED(status) && !InitOnceExecuteOnce(&once, RegisterDocumentLifetime, nullptr, nullptr)) {
      status = HRESULT_FROM_WIN32(GetLastError());
      if (SUCCEEDED(status)) status = E_FAIL;
    }
    if (SUCCEEDED(status)) {
      lifetime_window = CreateWindowExW(0, L"GesherDocumentPickerLifetime", L"", 0,
          0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), &lifetime);
      if (!lifetime_window || !SetTimer(lifetime_window, 1, 100, nullptr)) {
        status = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(status)) status = E_FAIL;
      }
    }
    if (SUCCEEDED(status) && active(context)) {
      DocumentOwner owner{owner_pid, nullptr};
      if (owner_pid) EnumWindows(FindDocumentOwner, reinterpret_cast<LPARAM>(&owner));
      status = dialog->Show(owner.window);
      if (SUCCEEDED(status) && active(context)) {
        status = DocumentPaths(dialog, mode, multiple, &selection.paths);
        if (SUCCEEDED(status) && local_only && !DocumentPathsAreLocal(mode, selection.paths)) {
          status = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        }
      }
    }
  }
  if (lifetime_window) { KillTimer(lifetime_window, 1); DestroyWindow(lifetime_window); }
  if (advised) dialog->Unadvise(cookie);
  if (events) events->Release();
  if (custom) custom->Release();
  if (dialog) dialog->Release();
  CoUninitialize();
  if (!active(context)) selection.paths.clear();
  if (FAILED(status)) {
    selection.paths.clear();
    if (status != HRESULT_FROM_WIN32(ERROR_CANCELLED)) selection.error = status;
  }
  return selection;
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

int SessionDisplayState() {
  AcquireSRWLockShared(&g_lock);
  int state = g_session_display_state;
  ReleaseSRWLockShared(&g_lock);
  return state;
}

uint64_t JobIdleMillis() {
  AcquireSRWLockShared(&g_lock);
  bool off = g_session_display_state == 0;
  uint64_t since = g_display_off_since;
  ReleaseSRWLockShared(&g_lock);
  if (!off) return 0;
  uint64_t display_idle = GetTickCount64() - since;
  uint64_t input_idle = IdleMillis();
  return display_idle < input_idle ? display_idle : input_idle;
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

std::u16string IanaZone(const std::u16string& windows_id) {
  // Windows' own ICU (icu.dll, Windows 10 1903 and later): CLDR's windowsZones, for the
  // "001" territory. Looked up at run time; empty without it.
  typedef int32_t(__cdecl * IdForWindowsId)(const char16_t* winid, int32_t len,
                                               const char* region, char16_t* id,
                                               int32_t capacity, int* status);
  static IdForWindowsId fn = [] {
    HMODULE m = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m != nullptr ? reinterpret_cast<IdForWindowsId>(
                              reinterpret_cast<void*>(
                                  GetProcAddress(m, "ucal_getTimeZoneIDForWindowsID")))
                        : nullptr;
  }();
  if (fn == nullptr || windows_id.empty()) return std::u16string();
  char16_t id[128];
  int status = 0;  // U_ZERO_ERROR; failures are > 0
  int32_t n = fn(windows_id.c_str(), static_cast<int32_t>(windows_id.size()), nullptr, id,
                 static_cast<int32_t>(sizeof(id) / sizeof(id[0])), &status);
  if (status > 0 || n <= 0 || n >= 128) return std::u16string();
  return std::u16string(id, static_cast<size_t>(n));
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

long RemoveToastIfLaunch(const std::u16string& app_id, const std::u16string& tag,
                         const std::u16string& group, const std::u16string& launch) {
  if (!ValidToastIdentityArguments(app_id, tag, group, launch)) return E_INVALIDARG;
  HRESULT hr = EnsureCom();
  if (FAILED(hr)) return hr;
  return gesher_toast_remove_if_launch(W(app_id), W(tag), W(group), W(launch));
}

const char16_t kToastActivatorClsid[] = u"{C18D9445-A7F0-472E-985F-AC354EBEB660}";

namespace {

// Whether the activator is registered as a local server (the installer's key).
bool ActivatorInstalled() {
  std::wstring key = L"Software\\Classes\\CLSID\\";
  key += W(kToastActivatorClsid);
  key += L"\\LocalServer32";
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &h) == ERROR_SUCCESS) {
      RegCloseKey(h);
      return true;
    }
  }
  return false;
}

}  // namespace

long RegisterAppId(const std::u16string& app_id, const std::u16string& display_name,
                   const std::u16string& icon) {
  std::wstring key = L"Software\\Classes\\AppUserModelId\\";
  key += W(app_id);
  HKEY h = nullptr;
  LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                           KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &h, nullptr);
  if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
  r = RegSetValueExW(h, L"DisplayName", 0, REG_SZ, reinterpret_cast<const BYTE*>(W(display_name)),
                     static_cast<DWORD>((display_name.size() + 1) * sizeof(wchar_t)));
  if (r == ERROR_SUCCESS && !icon.empty()) {
    r = RegSetValueExW(h, L"IconUri", 0, REG_SZ, reinterpret_cast<const BYTE*>(W(icon)),
                       static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
  }
  // Clicks go to Gesher's activator (also when no Gesher process runs) only where it
  // is registered; without it they reach the toast's Activated event while the system
  // server runs.
  if (r == ERROR_SUCCESS) {
    if (ActivatorInstalled()) {
      std::u16string clsid(kToastActivatorClsid);
      r = RegSetValueExW(h, L"CustomActivator", 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(W(clsid)),
                         static_cast<DWORD>((clsid.size() + 1) * sizeof(wchar_t)));
    } else {
      RegDeleteValueW(h, L"CustomActivator");
    }
  }
  RegCloseKey(h);
  return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

long RegisterActivator(bool single_use) {
  HRESULT hr = EnsureCom();
  if (FAILED(hr)) return hr;
  return gesher_activator_register(single_use ? 1 : 0);
}

void RevokeActivator() { gesher_activator_revoke(); }

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

bool OpenUrl(const std::u16string& url) {
  if (url.empty()) return false;
  AllowSetForegroundWindow(ASFW_ANY);
  HWND fg = GetForegroundWindow();
  std::wstring wurl = W(url);
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.hwnd = fg;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
  sei.lpVerb = L"open";
  sei.lpFile = wurl.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) {
    if (sei.hProcess != nullptr) {
      AllowSetForegroundWindow(GetProcessId(sei.hProcess));
      CloseHandle(sei.hProcess);
    }
    return true;
  }
  return false;
}

std::u16string PickFolder(const std::u16string& title) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool need_uninit = (hr == S_OK || hr == S_FALSE);

  IFileOpenDialog* pfd = nullptr;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&pfd));
  if (FAILED(hr) || !pfd) {
    if (need_uninit) CoUninitialize();
    return std::u16string();
  }

  if (!title.empty()) {
    pfd->SetTitle(W(title));
  }
  FILEOPENDIALOGOPTIONS opt = 0;
  if (SUCCEEDED(pfd->GetOptions(&opt))) {
    pfd->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  }

  HWND owner = GetForegroundWindow();
  hr = pfd->Show(owner);
  std::u16string result;
  if (SUCCEEDED(hr)) {
    IShellItem* psi = nullptr;
    if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
      PWSTR path = nullptr;
      if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = U16(path);
        CoTaskMemFree(path);
      }
      psi->Release();
    }
  }
  pfd->Release();
  if (need_uninit) CoUninitialize();
  return result;
}

std::u16string PickFile(const std::u16string& title) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool need_uninit = (hr == S_OK || hr == S_FALSE);

  IFileOpenDialog* pfd = nullptr;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&pfd));
  if (FAILED(hr) || !pfd) {
    if (need_uninit) CoUninitialize();
    return std::u16string();
  }

  if (!title.empty()) {
    pfd->SetTitle(W(title));
  }
  FILEOPENDIALOGOPTIONS opt = 0;
  if (SUCCEEDED(pfd->GetOptions(&opt))) {
    pfd->SetOptions(opt | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
  }

  HWND owner = GetForegroundWindow();
  hr = pfd->Show(owner);
  std::u16string result;
  if (SUCCEEDED(hr)) {
    IShellItem* psi = nullptr;
    if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
      PWSTR path = nullptr;
      if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = U16(path);
        CoTaskMemFree(path);
      }
      psi->Release();
    }
  }
  pfd->Release();
  if (need_uninit) CoUninitialize();
  return result;
}

}  // namespace devsvc
}  // namespace gesher
