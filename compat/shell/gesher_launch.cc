// gesher-launch.exe: what Explorer starts for Gesher (docs/installer.md): an
// app's Start menu shortcut (`gesher-launch launch <package>`), a double-clicked
// .apk (`gesher-launch <file.apk>`) and a click on a toast (`gesher-launch
// toast-activated`). It runs gesher-app.ps1 with the same arguments.
//
// It is a Windows-subsystem program, so that nothing flashes: powershell.exe is
// a console program, and Explorer starting it opens a console window before its
// -WindowStyle Hidden can hide it. Here powershell runs with CREATE_NO_WINDOW,
// its console never shown, and the runtime it starts shares that console.
//
// It also hands on the foreground: Explorer lets the program it starts for a
// click take the foreground, but the app's window belongs to a process the
// Gesher device's system server starts, which Windows keeps in the background
// (its window opens behind Explorer). AllowSetForegroundWindow(ASFW_ANY) lets
// that window come to the front, until the user's next input.
//
// A launch (`launch <package>`, `launch <file.apk>`, `<file.apk>`) that will
// show a window shows a splash window (class GesherSplash) at once, while the
// device starts, the APK is installed and the app's process starts: the app's
// label and icon, with its AppUserModelID (it groups with the app on the
// taskbar). It closes once a visible window with the app's AppUserModelID is up
// (the app's: compat/hwui/window sets it on each top-level window), when
// gesher-app.ps1 ends, or after two minutes. Closing it does not cancel the
// launch. GESHER_SPLASH=0 turns it off.
//
// Its exit code is gesher-app.ps1's. Standard handles it was given (a redirected
// output, as CI reads it) are passed on.

// The project defines NOGDI for AOSP code; the splash paints with GDI.
// CommandLineToArgvW and the property store need what WIN32_LEAN_AND_MEAN
// leaves out.
#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

// The arguments after the program name, as written (quotes kept).
const wchar_t* Arguments(const wchar_t* line) {
  bool quoted = false;
  for (; *line != L'\0'; ++line) {
    if (*line == L'"') quoted = !quoted;
    if (!quoted && (*line == L' ' || *line == L'\t')) break;
  }
  while (*line == L' ' || *line == L'\t') ++line;
  return line;
}

std::wstring Directory(const wchar_t* path) {
  std::wstring dir(path);
  size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
}

// --- the splash window -----------------------------------------------------------

constexpr wchar_t kSplashClass[] = L"GesherSplash";
// How long the splash waits for the app's window at most, and how often it looks.
constexpr ULONGLONG kSplashTimeoutMs = 120000;
constexpr ULONGLONG kSplashPollMs = 200;
// An app window's size when GESHER_DISPLAY gives none (WindowMode.DEFAULT_DISPLAY).
constexpr int kDefaultWidth = 400, kDefaultHeight = 800;
constexpr int kIconSize = 128;

// IID_IPropertyStore and PKEY_AppUserModel_ID (propkey.h), spelled out: not
// every GUID is in llvm-mingw's libuuid.
const IID kIidPropertyStore = {0x886D8EEB, 0x8CF2, 0x4446,
                               {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
const PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

struct Splash {
  std::wstring app_id;  // Gesher.App.<package>, or empty: the package is not known
  std::wstring label;
  std::wstring icon;  // an .ico file
  HICON picture = nullptr, big = nullptr, small = nullptr;
  HFONT title_font = nullptr, text_font = nullptr;
};

Splash* g_splash = nullptr;

std::wstring Env(const wchar_t* name) {
  wchar_t value[1024];
  DWORD n = GetEnvironmentVariableW(name, value, 1024);
  return n > 0 && n < 1024 ? std::wstring(value, n) : std::wstring();
}

std::wstring Lower(std::wstring s) {
  if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
  return s;
}

bool IsFile(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// A small file's bytes (at most 4 MB).
bool ReadBytes(const std::wstring& path, std::string* out) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length;
  bool ok = GetFileSizeEx(f, &length) && length.QuadPart >= 0 && length.QuadPart < (4 << 20);
  if (ok) {
    out->resize(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    ok = out->empty() ||
         ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != 0;
    out->resize(ok ? read : 0);
  }
  CloseHandle(f);
  return ok;
}

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

int HexDigit(wchar_t c) {
  if (c >= L'0' && c <= L'9') return c - L'0';
  if (c >= L'a' && c <= L'f') return c - L'a' + 10;
  if (c >= L'A' && c <= L'F') return c - L'A' + 10;
  return -1;
}

// Reads a java.util.Properties key (stops at an unescaped separator) or value
// (to the end of the line) from line[i], undoing its escapes (\uXXXX, \t, ...).
size_t Unescape(const std::wstring& line, size_t i, bool key, std::wstring* out) {
  while (i < line.size()) {
    wchar_t c = line[i];
    if (key && (c == L'=' || c == L':' || c == L' ' || c == L'\t' || c == L'\f')) break;
    ++i;
    if (c != L'\\') {
      out->push_back(c);
      continue;
    }
    if (i >= line.size()) break;
    c = line[i++];
    if (c == L'u') {
      int v = 0, digits = 0;
      for (; digits < 4 && i < line.size() && HexDigit(line[i]) >= 0; ++digits) {
        v = v * 16 + HexDigit(line[i++]);
      }
      out->push_back(static_cast<wchar_t>(v));
    } else {
      out->push_back(c == L't' ? L'\t' : c == L'n' ? L'\n' : c == L'r' ? L'\r' : c == L'f' ? L'\f' : c);
    }
  }
  return i;
}

// The value of `key` in a java.util.Properties file's text (ISO-8859-1, as
// Properties.store writes it), or "".
std::wstring PropertiesValue(const std::string& text, const std::wstring& key) {
  size_t p = 0;
  while (p < text.size()) {
    size_t end = text.find('\n', p);
    if (end == std::string::npos) end = text.size();
    std::wstring line;
    for (size_t i = p; i < end; ++i) {
      if (text[i] != '\r') line.push_back(static_cast<wchar_t>(static_cast<unsigned char>(text[i])));
    }
    p = end + 1;
    size_t i = 0;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t' || line[i] == L'\f')) ++i;
    if (i >= line.size() || line[i] == L'#' || line[i] == L'!') continue;
    std::wstring k, v;
    i = Unescape(line, i, true, &k);
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t' || line[i] == L'\f')) ++i;
    if (i < line.size() && (line[i] == L'=' || line[i] == L':')) ++i;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t' || line[i] == L'\f')) ++i;
    if (k != key) continue;
    Unescape(line, i, false, &v);
    return v;
  }
  return std::wstring();
}

// The package's AppUserModelID, as com.gesher.app.shell.AppShell.appUserModelId
// makes it: Gesher.App.<package>, a longer one than 128 characters cut and
// ended with the package name's String.hashCode.
std::wstring AppUserModelId(const std::wstring& package) {
  std::wstring id = L"Gesher.App." + package;
  if (id.size() > 128) {
    unsigned int h = 0;
    for (wchar_t c : package) h = 31 * h + static_cast<unsigned short>(c);
    wchar_t hex[9];
    for (int i = 7; i >= 0; --i, h >>= 4) hex[i] = L"0123456789abcdef"[h & 15];
    hex[8] = L'\0';
    id = id.substr(0, 119) + L"." + hex;
  }
  return id;
}

// The .ico and package apk-icons.list records for the APK file (its size and
// last write time, as gesher_shellext.cc looks it up); package "-" is unknown.
bool ApkRecord(const std::wstring& data, const std::wstring& apk, std::wstring* icon,
               std::wstring* package) {
  WIN32_FILE_ATTRIBUTE_DATA attributes;
  if (!GetFileAttributesExW(apk.c_str(), GetFileExInfoStandard, &attributes)) return false;
  unsigned long long size =
      (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  unsigned long long ft = (static_cast<unsigned long long>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
                          attributes.ftLastWriteTime.dwLowDateTime;
  // FILETIME (100 ns since 1601) as java.io.File.lastModified's milliseconds.
  unsigned long long millis = ft >= 116444736000000000ULL ? (ft - 116444736000000000ULL) / 10000ULL : 0;
  std::string text;
  if (!ReadBytes(data + L"\\system\\apk-icons.list", &text)) return false;
  const std::string want = std::to_string(size) + "\t" + std::to_string(millis) + "\t";
  size_t p = 0;
  while (p < text.size()) {
    size_t end = text.find('\n', p);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(p, end - p);
    p = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, want.size(), want) != 0) continue;
    std::string rest = line.substr(want.size());
    size_t tab = rest.find('\t');
    *icon = Widen(rest.substr(0, tab));
    std::wstring name = tab == std::string::npos ? std::wstring() : Widen(rest.substr(tab + 1));
    *package = name == L"-" ? std::wstring() : name;
    return true;
  }
  return false;
}

// "WxH[@dpi]" (GESHER_DISPLAY, as the app's window gets it), else the default size.
void DisplaySize(const std::wstring& display, int* width, int* height) {
  *width = kDefaultWidth;
  *height = kDefaultHeight;
  size_t x = display.find(L'x');
  if (x == std::wstring::npos) return;
  int w = 0, h = 0;
  size_t i = 0;
  for (; i < x && display[i] >= L'0' && display[i] <= L'9' && w < 100000; ++i) w = w * 10 + (display[i] - L'0');
  if (i != x) return;
  for (i = x + 1; i < display.size() && display[i] >= L'0' && display[i] <= L'9' && h < 100000; ++i) {
    h = h * 10 + (display[i] - L'0');
  }
  if (w >= 100 && h >= 100) {
    *width = w;
    *height = h;
  }
}

// Whether these arguments launch an app in a window it shows (gesher-app.ps1's
// and Launcher's options), and if so, what the splash shows.
bool SplashFor(const std::wstring& here, Splash* splash) {
  if (Env(L"GESHER_SPLASH") == L"0") return false;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return false;
  std::vector<std::wstring> args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  LocalFree(argv);
  if (args.empty()) return false;
  std::wstring target;
  size_t first = 1;
  if (Lower(args[0]) == L"launch") {
    if (args.size() < 2) return false;
    target = args[1];
    first = 2;
  } else {
    // gesher-app.ps1's other commands show no window of an app; anything else is an APK.
    static const wchar_t* const kCommands[] = {L"install", L"uninstall", L"view", L"list",
                                               L"status", L"start", L"shutdown",
                                               L"toast-activated", L"apk-icon"};
    for (const wchar_t* command : kCommands) {
      if (Lower(args[0]) == command) return false;
    }
    target = args[0];
  }
  std::wstring mode = Lower(Env(L"GESHER_APP_WINDOW"));
  bool shown = !(mode == L"hidden" || mode == L"hidden-window" || mode == L"headless" ||
                 mode == L"none");
  for (size_t i = first; i < args.size(); ++i) {
    const std::wstring& a = args[i];
    if (a == L"--window") shown = true;
    if (a == L"--hidden-window" || a == L"--headless") shown = false;
    if (a == L"--single-process" || a == L"--system-server") return false;  // no AppUserModelID
    if (a == L"--activity") ++i;
    if (a == L"--ez") i += 2;
  }
  if (!shown) return false;

  const std::wstring data = Env(L"LOCALAPPDATA") + L"\\Gesher\\data";
  std::wstring package, apk_icon;
  if (IsFile(target)) {
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(target.c_str(), MAX_PATH * 4, full, nullptr);
    std::wstring apk = n > 0 && n < MAX_PATH * 4 ? std::wstring(full, n) : target;
    ApkRecord(data, apk, &apk_icon, &package);
    size_t slash = apk.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? apk : apk.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    splash->label = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
  } else if (target.find_first_of(L"\\/:") == std::wstring::npos) {
    package = target;
    splash->label = package;
  }
  if (!package.empty()) {
    splash->app_id = AppUserModelId(package);
    const std::wstring shell = data + L"\\data\\" + package + L"\\gesher-shell";
    std::string record;
    if (ReadBytes(shell + L"\\shortcut.properties", &record)) {
      std::wstring label = PropertiesValue(record, L"label");
      if (!label.empty()) splash->label = label;
    }
    if (IsFile(shell + L"\\icon.ico")) splash->icon = shell + L"\\icon.ico";
  }
  if (splash->icon.empty() && !apk_icon.empty() && IsFile(apk_icon)) splash->icon = apk_icon;
  if (splash->icon.empty()) splash->icon = here + L"\\gesher.ico";
  if (splash->label.empty()) splash->label = L"Gesher";
  return true;
}

// shell32's SHGetPropertyStoreForWindow, looked up when first used.
HRESULT WindowPropertyStore(HWND hwnd, IPropertyStore** store) {
  using GetStoreFn = HRESULT(WINAPI*)(HWND, REFIID, void**);
  static HMODULE module = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  void* get = module == nullptr
                  ? nullptr
                  : reinterpret_cast<void*>(GetProcAddress(module, "SHGetPropertyStoreForWindow"));
  if (get == nullptr) return E_NOTIMPL;
  return reinterpret_cast<GetStoreFn>(get)(hwnd, kIidPropertyStore, reinterpret_cast<void**>(store));
}

// The window's System.AppUserModel.ID (what the taskbar groups it by), or "".
std::wstring WindowAppUserModelId(HWND hwnd) {
  IPropertyStore* store = nullptr;
  if (FAILED(WindowPropertyStore(hwnd, &store))) return std::wstring();
  PROPVARIANT value;
  std::memset(&value, 0, sizeof(value));
  std::wstring id;
  if (SUCCEEDED(store->GetValue(kAppUserModelId, &value)) && value.vt == VT_LPWSTR &&
      value.pwszVal != nullptr) {
    id = value.pwszVal;
  }
  PropVariantClear(&value);
  store->Release();
  return id;
}

void SetWindowAppUserModelId(HWND hwnd, const std::wstring& id) {
  IPropertyStore* store = nullptr;
  if (FAILED(WindowPropertyStore(hwnd, &store))) return;
  PROPVARIANT value;
  std::memset(&value, 0, sizeof(value));
  size_t bytes = (id.size() + 1) * sizeof(wchar_t);
  value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
  if (value.pwszVal != nullptr) {
    std::memcpy(value.pwszVal, id.c_str(), bytes);
    value.vt = VT_LPWSTR;
    if (SUCCEEDED(store->SetValue(kAppUserModelId, value))) store->Commit();
  }
  PropVariantClear(&value);
  store->Release();
}

struct WindowSearch {
  HWND splash;
  const std::wstring* app_id;
  const std::vector<HWND>* before;  // the package not known: windows already shown
  HWND found;
};

// The app's window: visible, top-level, not a tool window, with the app's
// AppUserModelID (the package not known: any Gesher app's, of a window not
// shown before the launch). Never a splash.
BOOL CALLBACK FindAppWindow(HWND hwnd, LPARAM lparam) {
  auto* search = reinterpret_cast<WindowSearch*>(lparam);
  if (hwnd == search->splash || !IsWindowVisible(hwnd)) return TRUE;
  if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
  wchar_t name[64];
  if (GetClassNameW(hwnd, name, 64) > 0 && std::wcscmp(name, kSplashClass) == 0) return TRUE;
  const bool any = search->app_id->empty();
  if (any && std::find(search->before->begin(), search->before->end(), hwnd) != search->before->end()) {
    return TRUE;
  }
  std::wstring id = WindowAppUserModelId(hwnd);
  if (any ? id.compare(0, 11, L"Gesher.App.") != 0 : id != *search->app_id) return TRUE;
  search->found = hwnd;
  return FALSE;
}

HWND AppWindow(HWND splash, const std::wstring& app_id, const std::vector<HWND>& before) {
  WindowSearch search = {splash, &app_id, &before, nullptr};
  EnumWindows(FindAppWindow, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

BOOL CALLBACK CollectVisible(HWND hwnd, LPARAM lparam) {
  if (IsWindowVisible(hwnd)) reinterpret_cast<std::vector<HWND>*>(lparam)->push_back(hwnd);
  return TRUE;
}

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_PAINT && g_splash != nullptr) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    const int w = client.right, h = client.bottom;
    const int size = std::min(kIconSize, w / 2);
    const int top = std::max(16, h * 2 / 5 - size / 2);
    if (g_splash->picture != nullptr) {
      DrawIconEx(dc, (w - size) / 2, top, g_splash->picture, size, size, 0, nullptr, DI_NORMAL);
    }
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g_splash->title_font);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT label = {16, top + size + 24, w - 16, top + size + 24 + 64};
    DrawTextW(dc, g_splash->label.c_str(), -1, &label,
              DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, g_splash->text_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    RECT status = {16, label.bottom + 8, w - 16, label.bottom + 40};
    DrawTextW(dc, L"Starting\u2026", -1, &status, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    EndPaint(hwnd, &ps);
    return 0;
  }
  // WM_CLOSE (its X) destroys the splash only: the launch goes on.
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND ShowSplash(Splash* splash) {
  // Pixels are pixels, as in the app's window (compat/hwui/window).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = SplashProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32650));  // IDC_APPSTARTING
  wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
  wc.lpszClassName = kSplashClass;
  if (RegisterClassExW(&wc) == 0) return nullptr;

  int width = 0, height = 0;
  DisplaySize(Env(L"GESHER_DISPLAY"), &width, &height);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT frame = {0, 0, width, height};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  // Centered in the work area of the monitor the pointer is on, inside it.
  POINT cursor = {0, 0};
  GetCursorPos(&cursor);
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof monitor;
  RECT work = {0, 0, 1024, 768};
  if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
    work = monitor.rcWork;
  }
  const int ww = std::min<int>(frame.right - frame.left, work.right - work.left);
  const int wh = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
  const int x = work.left + (work.right - work.left - ww) / 2;
  const int y = work.top + (work.bottom - work.top - wh) / 2;

  splash->picture = static_cast<HICON>(
      LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON, kIconSize, kIconSize, LR_LOADFROMFILE));
  splash->big = static_cast<HICON>(LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
  splash->small = static_cast<HICON>(LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
  splash->title_font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  splash->text_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  g_splash = splash;

  HWND hwnd = CreateWindowExW(0, kSplashClass, splash->label.c_str(), style, x, y, ww, wh, nullptr,
                              nullptr, instance, nullptr);
  if (hwnd == nullptr) return nullptr;
  // Before it is shown: its taskbar button is the app's (the shortcut's icon).
  if (!splash->app_id.empty()) SetWindowAppUserModelId(hwnd, splash->app_id);
  if (splash->big != nullptr) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(splash->big));
  if (splash->small != nullptr) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(splash->small));
  }
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  return hwnd;
}

void FreeSplash(Splash* splash) {
  g_splash = nullptr;
  for (HICON icon : {splash->picture, splash->big, splash->small}) {
    if (icon != nullptr) DestroyIcon(icon);
  }
  if (splash->title_font != nullptr) DeleteObject(splash->title_font);
  if (splash->text_font != nullptr) DeleteObject(splash->text_font);
}

// Shows the splash until the app's window is up, `process` ends, the user
// closes it or the timeout; then waits for `process` as before.
void SplashWhileStarting(const std::wstring& here, HANDLE process) {
  Splash splash;
  if (!SplashFor(here, &splash)) return;
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::vector<HWND> before;
  if (splash.app_id.empty()) EnumWindows(CollectVisible, reinterpret_cast<LPARAM>(&before));
  // The app already has its window (it runs): no splash.
  HWND hwnd = AppWindow(nullptr, splash.app_id, before) == nullptr ? ShowSplash(&splash) : nullptr;
  if (hwnd != nullptr) {
    const ULONGLONG start = GetTickCount64();
    ULONGLONG next = start + kSplashPollMs;
    for (;;) {
      ULONGLONG now = GetTickCount64();
      DWORD wait = now >= next ? 0 : static_cast<DWORD>(next - now);
      DWORD r = MsgWaitForMultipleObjects(1, &process, FALSE, wait, QS_ALLINPUT);
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (r == WAIT_OBJECT_0 || !IsWindow(hwnd)) break;
      now = GetTickCount64();
      if (now < next) continue;
      next = now + kSplashPollMs;
      HWND app = AppWindow(hwnd, splash.app_id, before);
      if (app != nullptr) {
        // Never in the app's way: if the splash still has the foreground (the
        // app's window did not take it), it hands it on before it goes.
        if (GetForegroundWindow() == hwnd) SetForegroundWindow(app);
        break;
      }
      if (now - start >= kSplashTimeoutMs) break;
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
  }
  FreeSplash(&splash);
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t self[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
  if (n == 0 || n >= sizeof(self) / sizeof(self[0])) return 1;
  const std::wstring here = Directory(self);

  wchar_t system[MAX_PATH];
  UINT s = GetSystemDirectoryW(system, MAX_PATH);
  std::wstring powershell = (s > 0 && s < MAX_PATH ? std::wstring(system) : L"C:\\Windows\\System32") +
                            L"\\WindowsPowerShell\\v1.0\\powershell.exe";
  std::wstring command = L"\"" + powershell +
                         L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                         here + L"\\gesher-app.ps1\" " + Arguments(GetCommandLineW());

  AllowSetForegroundWindow(ASFW_ANY);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE),
         err = GetStdHandle(STD_ERROR_HANDLE);
  bool inherit = (out != nullptr && out != INVALID_HANDLE_VALUE) ||
                 (err != nullptr && err != INVALID_HANDLE_VALUE);
  if (inherit) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
  }
  PROCESS_INFORMATION pi = {};
  std::wstring mutable_command = command;
  if (!CreateProcessW(powershell.c_str(), mutable_command.data(), nullptr, nullptr,
                      inherit ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return 1;
  }
  CloseHandle(pi.hThread);
  // The splash comes up while powershell starts (the launch is never delayed by it).
  SplashWhileStarting(here, pi.hProcess);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}
