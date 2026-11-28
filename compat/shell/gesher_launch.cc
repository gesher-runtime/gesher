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
// gesher-app.ps1 ends (with an error), or after two minutes. Closing it (its X)
// cancels the launch and ends its processes. GESHER_SPLASH=0 turns it off.
//
// Its exit code is gesher-app.ps1's. Standard handles it was given (a redirected
// output, as CI reads it) are passed on, and then the launch waits, as on a
// console, until the app's process ends (its output streamed). Without them (a
// Start menu shortcut, a double-clicked .apk: nobody reads the output or the
// exit code) it sets GESHER_LAUNCH_DETACH=1: com.gesher.app.Launcher returns
// once the activity has started, so that gesher-launch, powershell, its console
// and the launcher's dalvikvm all end instead of staying for the app's lifetime.
// The app's process belongs to the device. The splash then stays until the
// app's window is up (at most kSplashAfterDetachMs more). --detach and --wait
// in the arguments (gesher-app's) override the variable, for the splash too.
// A control panel opened without output handles owns its own lifetime. Release
// this executable after starting it, so its updater can replace the launcher
// while the panel remains open to monitor Setup. Redirected invocations still
// wait and return the PowerShell script's output and exit code.
//
// An APK or bundle opened from Explorer (the bare form `gesher-launch <file>`:
// exactly one argument, no gesher-app command, an existing file; also the
// portable Gesher.exe given a file) is never installed without the user's
// consent (issue 519):
//  1. A small progress window (the splash's painting: the file's name, "Reading...")
//     while `gesher-app.ps1 apk-info <file> <metadata file> <snapshot dir>` runs (apk_info_file.h
//     reads its key=value output; the file is deleted). Its X cancels (exit 1223).
//  2. The same versionCode is installed: `launch <package>`, as its Start menu
//     shortcut does (the splash, until the app's window is up).
//  3. Otherwise the confirmation dialog, title install_confirm_title: the app's
//     icon (64 px, the cached .ico or gesher.ico), label, version and question,
//     the button Install, or Update when the package is installed (IDOK = 1),
//     Cancel (IDCANCEL = 2, the default button and the initial focus), and the
//     check box "Create a desktop shortcut" (1001, unchecked; absent in portable
//     mode, with GESHER_SHORTCUTS=0 and when the recorded desktop shortcut
//     exists). Cancel, Esc and X install nothing: exit 1223 (ERROR_CANCELLED).
//  4. Install: the progress window ("Installing <label>...", no X) while
//     `gesher-app.ps1 install <snapshot> --confirmed <package> <version> <digest>` runs.
//  5. After a matching installation result, the installed dialog, title install_done_title:
//     Open (1002) runs `launch <package>` with the splash; Done (IDCANCEL = 2, the
//     default and the initial focus), Esc and X end it (exit 0).
// Both dialogs are top-level #32770 dialogs of this process, themed by
// compat/windows/host_dialog_theme.h (right-to-left text for Hebrew). A failing
// step shows its error file's text (ShowLaunchFailure) and returns its exit code
// (never 0); with redirected handles each step's output goes to them instead.

// The project defines NOGDI for AOSP code; the splash paints with GDI.
// CommandLineToArgvW and the property store need what WIN32_LEAN_AND_MEAN
// leaves out.
#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include "../locale/ui_locale.h"
#include "../windows/host_dialog_theme.h"
#include "apk_info_file.h"
#include "launch_failure_text.h"
#include "app_crash_text.h"

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
// A detached launch's gesher-app.ps1 ends once the activity has started: how much
// longer the splash waits for its window, and, if the user closed the splash after
// that, how long it looks for the app's windows to close them.
// (Generous: a trampoline activity, or an app's first run, can take long to show
// its window after the first activity is idle.)
constexpr ULONGLONG kSplashAfterDetachMs = 60000;
constexpr ULONGLONG kCancelAfterDetachMs = 10000;
// The environment variable that asks com.gesher.app.Launcher not to wait (Launcher.DETACH_ENV).
constexpr wchar_t kDetachEnv[] = L"GESHER_LAUNCH_DETACH";
// Where com.gesher.app.Launcher writes a failure for the user (Launcher.ERROR_FILE_ENV).
constexpr wchar_t kErrorFileEnv[] = L"GESHER_LAUNCH_ERROR_FILE";
// An app window's size when GESHER_DISPLAY gives none (WindowMode.DEFAULT_DISPLAY).
constexpr int kDefaultWidth = 400, kDefaultHeight = 800;
constexpr int kIconSize = 128;

// IID_IPropertyStore and PKEY_AppUserModel_ID (propkey.h), spelled out: not
// every GUID is in llvm-mingw's libuuid.
const IID kIidPropertyStore = {0x886D8EEB, 0x8CF2, 0x4446,
                               {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
const PROPERTYKEY kAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

// The bare-file flow's results (see the top): the user cancelled, and the dialogs' controls.
constexpr int kExitCancelled = 1223;  // ERROR_CANCELLED
constexpr int kDesktopShortcutId = 1001;
constexpr int kOpenId = 1002;
constexpr int kAppIconId = 1010, kVersionTextId = 1011, kQuestionTextId = 1012;
constexpr int kDialogIconSize = 64;
// The progress window's size (96-DPI pixels; scaled to its monitor).
constexpr int kProgressWidth = 380, kProgressHeight = 320, kProgressIconSize = 64;

struct Splash {
  std::wstring app_id;  // Gesher.App.<package>, or empty: the package is not known
  std::wstring label;
  std::wstring icon;  // an .ico file
  std::wstring status;  // the line under the label: "Starting..." when empty
  bool closable = true;  // its X cancels; otherwise the X is disabled
  int dpi = 96;  // what its fonts and layout are scaled for (the splash: pixels are pixels)
  int icon_size = kIconSize;
  HICON picture = nullptr, big = nullptr, small = nullptr;
  HFONT title_font = nullptr, text_font = nullptr;
};

// A progress window still up when the next window of the flow comes: that window
// destroys it once it is shown, so that the foreground stays with this process.
HWND g_handover = nullptr;

void EndHandover() {
  if (g_handover != nullptr && IsWindow(g_handover)) DestroyWindow(g_handover);
  g_handover = nullptr;
}

// gesher-app.ps1's commands and gesher-launch's own: the first argument names
// one, so the arguments are not the bare-file form, and they show no app window
// (except launch).
const wchar_t* const kCommands[] = {
    L"launch", L"install", L"uninstall", L"view", L"list", L"status", L"start", L"shutdown",
    L"toast-activated", L"apk-icon", L"apk-info", L"proxy", L"windows", L"special-access",
    L"settings-closed", L"settings-shown", L"sync-shortcuts", L"cameras", L"spell-checker",
    L"mobile-data", L"control-panel", L"settings", L"dashboard", L"gui", L"--app-crash"};

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

bool IsCommand(const std::wstring& argument) {
  const std::wstring lower = Lower(argument);
  for (const wchar_t* command : kCommands) {
    if (lower == command) return true;
  }
  return false;
}

std::wstring GetDataDirectory(const std::wstring& here) {
  // Keep the splash on the runtime scripts' profile. They derive ANDROID_DATA
  // from this profile, so an inherited Android path is not authoritative here.
  std::wstring env_profile = Env(L"GESHER_PROFILE");
  if (!env_profile.empty()) {
    DWORD required = GetFullPathNameW(env_profile.c_str(), 0, nullptr, nullptr);
    if (required == 0) return std::wstring();
    std::vector<wchar_t> full(required);
    DWORD count = GetFullPathNameW(env_profile.c_str(), required, full.data(), nullptr);
    return count > 0 && count < required ? std::wstring(full.data(), count) + L"\\data"
                                        : std::wstring();
  }
  if (IsFile(here + L"\\portable.marker")) {
    return here + L"\\Data\\data";
  }
  size_t slash = here.find_last_of(L"\\/");
  std::wstring leaf = slash == std::wstring::npos ? here : here.substr(slash + 1);
  std::wstring parent = Directory(here.c_str());
  if (Lower(leaf) == L"app" && IsFile(parent + L"\\portable.marker")) {
    return parent + L"\\Data\\data";
  }
  return Env(L"LOCALAPPDATA") + L"\\Gesher\\data";
}

// A new empty file in the user's temp directory for the launcher's failure
// (kErrorFileEnv), or empty.
std::wstring NewErrorFile() {
  wchar_t dir[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH + 1, dir);
  if (n == 0 || n > MAX_PATH) return std::wstring();
  wchar_t file[MAX_PATH];
  if (GetTempFileNameW(dir, L"gsh", 0, file) == 0) return std::wstring();
  return file;
}

// The UTF-8 text of `path` (at most 64 KiB), deleting the file.
std::wstring TakeErrorText(const std::wstring& path) {
  std::wstring text;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    std::string bytes(65536, '\0');
    DWORD got = 0;
    if (ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got > 0) {
      int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(got), nullptr, 0);
      if (chars > 0) {
        text.resize(static_cast<size_t>(chars));
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(got), text.data(), chars);
      }
    }
    CloseHandle(f);
  }
  DeleteFileW(path.c_str());
  return text;
}

// Shows the launcher's failure: nobody reads its output when Explorer started the
// launch, and without this the splash would only close, as if the app had crashed.
void ShowLaunchFailure(const std::wstring& reason, HWND owner = nullptr) {
  if (reason.empty()) return;
  const auto language = GesherUiCurrentLanguage();
  const std::wstring formatted = GesherFormatLaunchFailureReason(reason, GesherUiRtl(language));
  std::wstring text = GesherUiText(language, "launch_failed");
  const size_t at = text.find(L"{0}");
  if (at != std::wstring::npos) text.replace(at, 3, formatted);
  else text = formatted;
  GesherHostPrompt(GesherUiText(language, "brand"), text.c_str(),
                   GesherUiText(language, "cp_dialog_ok"), nullptr,
                   GesherUiRtl(language), owner, MB_ICONERROR);
}

// A failing app starts this short-lived host process rather than waiting inside
// Android's dying main thread. No PowerShell, APK launch, splash or retry here.
int ShowAppCrash(int argc, LPWSTR* argv) {
  if (argc != 6 || wcslen(argv[2]) == 0 || wcslen(argv[2]) > 255 ||
      wcslen(argv[4]) == 0 || wcslen(argv[4]) > 8192 ||
      wcslen(argv[5]) == 0 || wcslen(argv[5]) > 64) return 1;
  const char* key = nullptr;
  if (wcscmp(argv[3], L"generic") == 0) key = "app_crash_generic";
  else if (wcscmp(argv[3], L"bluetooth") == 0) key = "app_crash_bluetooth";
  else if (wcscmp(argv[3], L"security") == 0) key = "app_crash_security";
  else if (wcscmp(argv[3], L"compatibility") == 0) key = "app_crash_compatibility";
  if (key == nullptr) return 1;
  const auto language = GesherUiCurrentLanguage();
  const std::wstring message = GesherFormatAppCrashText(
      GesherUiText(language, key), argv[2], argv[4], argv[5]);
  GesherHostPrompt(GesherUiText(language, "brand"), message.c_str(),
                   GesherUiText(language, "cp_dialog_ok"), nullptr,
                   GesherUiRtl(language), nullptr, MB_ICONERROR);
  return 0;
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

// The program's arguments (after its name).
std::vector<std::wstring> CommandArguments() {
  std::vector<std::wstring> args;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  LocalFree(argv);
  return args;
}

// Gesher's icon beside the scripts (installed, portable root or App folder), or "".
std::wstring GesherIcon(const std::wstring& here) {
  for (const std::wstring& candidate : {here + L"\\gesher.ico", Directory(here.c_str()) + L"\\gesher.ico",
                                        here + L"\\App\\gesher.ico"}) {
    if (IsFile(candidate)) return candidate;
  }
  return std::wstring();
}

// Whether these arguments (gesher-app.ps1's) launch an app in a window it shows
// (gesher-app.ps1's and Launcher's options), and if so, what the splash shows.
bool SplashFor(const std::wstring& here, const std::vector<std::wstring>& args, Splash* splash) {
  if (Env(L"GESHER_SPLASH") == L"0") return false;
  if (args.empty()) return false;
  std::wstring target;
  size_t first = 1;
  if (Lower(args[0]) == L"launch") {
    if (args.size() < 2) return false;
    target = args[1];
    first = 2;
  } else {
    // gesher-app.ps1's other commands show no window of an app; anything else is an APK.
    if (IsCommand(args[0])) return false;
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
    if (a == L"--activity" || a == L"--proxy") ++i;
    if (a == L"--ez") i += 2;
  }
  if (!shown) return false;

  const std::wstring data = GetDataDirectory(here);
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
  if (splash->icon.empty()) splash->icon = GesherIcon(here);
  if (splash->label.empty()) splash->label = GesherUiText(GesherUiCurrentLanguage(), "brand");
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

void TerminateProcessTree(DWORD pid) {
  if (pid == 0) return;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ParentProcessID == pid) {
          TerminateProcessTree(pe.th32ProcessID);
        }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }
  HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (proc != nullptr) {
    TerminateProcess(proc, 1);
    CloseHandle(proc);
  }
}

bool g_cancelled = false;

// The direction of a text's first strong character: C2_LEFTTORIGHT,
// C2_RIGHTTOLEFT, or 0 (none: digits, punctuation). An app's label has its own
// direction, whatever the UI's language.
WORD StrongDirection(const std::wstring& text) {
  if (text.empty() || text.size() > 0x7fffffff) return 0;
  std::vector<WORD> types(text.size());
  if (!GetStringTypeW(CT_CTYPE2, text.data(), static_cast<int>(text.size()), types.data())) {
    return 0;
  }
  for (WORD type : types) {
    if (type == C2_LEFTTORIGHT || type == C2_RIGHTTOLEFT) return type;
  }
  return 0;
}

bool StartsRtl(const std::wstring& text) { return StrongDirection(text) == C2_RIGHTTOLEFT; }

// `value` (an app's label or version) inside a sentence of the UI's direction,
// kept in its own direction (an embedding: GDI's text output has no isolates).
std::wstring Embed(const std::wstring& value, bool ui_rtl) {
  const WORD direction = StrongDirection(value);
  if (direction == 0 || (direction == C2_RIGHTTOLEFT) == ui_rtl) return value;
  return std::wstring(1, direction == C2_RIGHTTOLEFT ? L'\u202B' : L'\u202A') + value + L'\u202C';
}

// GDI presentation only; launch/cancellation policy stays in SplashProc and its caller.
void PaintSplash(HWND hwnd, const Splash* splash) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client;
  GetClientRect(hwnd, &client);
  // Match the Control Center's light surface and semantic text hierarchy.
  const bool high_contrast = GesherHostHighContrast() != FALSE;
  FillRect(dc, &client, high_contrast ? GetSysColorBrush(COLOR_WINDOW)
      : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  auto px = [splash](int value) { return MulDiv(value, splash->dpi, 96); };
  const int w = client.right, h = client.bottom;
  const int size = std::min(splash->icon_size, w / 2);
  const int top = std::max(px(16), h * 2 / 5 - size / 2);
  if (splash->picture != nullptr) {
    DrawIconEx(dc, (w - size) / 2, top, splash->picture, size, size, 0, nullptr, DI_NORMAL);
  }
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ old = SelectObject(dc, splash->title_font);
  SetTextColor(dc, high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(16, 24, 40));
  RECT label = {px(16), top + size + px(24), w - px(16), top + size + px(24 + 64)};
  DrawTextW(dc, splash->label.c_str(), -1, &label,
            DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX |
                (StartsRtl(splash->label) ? DT_RTLREADING : 0));
  SelectObject(dc, splash->text_font);
  SetTextColor(dc, high_contrast ? GetSysColor(COLOR_GRAYTEXT) : RGB(102, 112, 133));
  RECT status = {px(16), label.bottom + px(8), w - px(16), label.bottom + px(40)};
  const auto language = GesherUiCurrentLanguage();
  DrawTextW(dc, splash->status.empty() ? GesherUiText(language, "starting") : splash->status.c_str(),
            -1, &status,
            DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX |
                (GesherUiRtl(language) ? DT_RTLREADING : 0));
  SelectObject(dc, old);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* splash = reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && splash != nullptr) {
    PaintSplash(hwnd, splash);
    return 0;
  }
  if (msg == WM_CLOSE) {
    // A progress window whose step must not be interrupted (an installation) stays.
    if (splash != nullptr && !splash->closable) return 0;
    g_cancelled = true;
    DestroyWindow(hwnd);
    return 0;
  }
  if (msg == WM_DESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The window (hidden) of a splash or progress window: its class, icons and fonts
// (for splash->dpi).
HWND CreateSplashWindow(Splash* splash, int width, int height, int x, int y) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = SplashProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32650));  // IDC_APPSTARTING
  wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
  wc.lpszClassName = kSplashClass;
  // The bare-file flow can show more than one (its progress, then the splash).
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  splash->picture = static_cast<HICON>(LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON,
                                                  splash->icon_size, splash->icon_size,
                                                  LR_LOADFROMFILE));
  splash->big = static_cast<HICON>(LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXICON),
                                              GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE));
  splash->small = static_cast<HICON>(LoadImageW(nullptr, splash->icon.c_str(), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
  splash->title_font = CreateFontW(-MulDiv(24, splash->dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE,
                                   FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  splash->text_font = CreateFontW(-MulDiv(16, splash->dpi, 96), 0, 0, 0, FW_NORMAL, FALSE,
                                  FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  const DWORD ex_style = GesherUiRtl(GesherUiCurrentLanguage()) ? WS_EX_RTLREADING : 0;
  HWND hwnd = CreateWindowExW(ex_style, kSplashClass, splash->label.c_str(), style, x, y, width,
                              height, nullptr, nullptr, instance, splash);
  if (hwnd == nullptr) return nullptr;
  GesherHostApplyCaption(hwnd);
  if (!splash->closable) EnableMenuItem(GetSystemMenu(hwnd, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
  // Before it is shown: its taskbar button is the app's (the shortcut's icon).
  if (!splash->app_id.empty()) SetWindowAppUserModelId(hwnd, splash->app_id);
  if (splash->big != nullptr) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(splash->big));
  if (splash->small != nullptr) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(splash->small));
  }
  return hwnd;
}

// The work area of the monitor the pointer is on.
RECT PointerWorkArea(HMONITOR* which) {
  POINT cursor = {0, 0};
  GetCursorPos(&cursor);
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof monitor;
  RECT work = {0, 0, 1024, 768};
  HMONITOR handle = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  if (GetMonitorInfoW(handle, &monitor)) work = monitor.rcWork;
  if (which != nullptr) *which = handle;
  return work;
}

HWND ShowSplash(Splash* splash) {
  // Pixels are pixels, as in the app's window (compat/hwui/window).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int width = 0, height = 0;
  DisplaySize(Env(L"GESHER_DISPLAY"), &width, &height);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT frame = {0, 0, width, height};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  // Centered in the work area of the monitor the pointer is on, inside it.
  const RECT work = PointerWorkArea(nullptr);
  const int ww = std::min<int>(frame.right - frame.left, work.right - work.left);
  const int wh = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
  const int x = work.left + (work.right - work.left - ww) / 2;
  const int y = work.top + (work.bottom - work.top - wh) / 2;
  HWND hwnd = CreateSplashWindow(splash, ww, wh, x, y);
  if (hwnd == nullptr) return nullptr;
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  EndHandover();
  return hwnd;
}

// The monitor's DPI (shcore's GetDpiForMonitor, looked up when first used), else 96.
int MonitorDpi(HMONITOR monitor) {
  using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static HMODULE module = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  void* get = module == nullptr ? nullptr
                                : reinterpret_cast<void*>(GetProcAddress(module, "GetDpiForMonitor"));
  UINT x = 96, y = 96;
  if (get == nullptr || FAILED(reinterpret_cast<GetDpiFn>(get)(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) {
    return 96;
  }
  return x >= 48 ? static_cast<int>(x) : 96;
}

// The bare-file flow's small progress window: the splash's painting, scaled to
// its monitor (unlike the splash, it is no stand-in for the app's window).
HWND ShowProgress(Splash* splash) {
  if (Env(L"GESHER_SPLASH") == L"0") return nullptr;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HMONITOR monitor = nullptr;
  const RECT work = PointerWorkArea(&monitor);
  splash->dpi = MonitorDpi(monitor);
  splash->icon_size = MulDiv(kProgressIconSize, splash->dpi, 96);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT frame = {0, 0, MulDiv(kProgressWidth, splash->dpi, 96), MulDiv(kProgressHeight, splash->dpi, 96)};
  AdjustWindowRectEx(&frame, style, FALSE, 0);
  const int ww = std::min<int>(frame.right - frame.left, work.right - work.left);
  const int wh = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
  HWND hwnd = CreateSplashWindow(splash, ww, wh, work.left + (work.right - work.left - ww) / 2,
                                 work.top + (work.bottom - work.top - wh) / 2);
  if (hwnd == nullptr) return nullptr;
  ShowWindow(hwnd, SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  EndHandover();
  return hwnd;
}

void FreeSplash(Splash* splash) {
  for (HICON* icon : {&splash->picture, &splash->big, &splash->small}) {
    if (*icon != nullptr) DestroyIcon(*icon);
    *icon = nullptr;
  }
  if (splash->title_font != nullptr) DeleteObject(splash->title_font);
  if (splash->text_font != nullptr) DeleteObject(splash->text_font);
  splash->title_font = splash->text_font = nullptr;
}

// Posts WM_CLOSE to the app's visible top-level windows (this AppUserModelID; never
// a splash, which another launch of the app may show) not in *done, and adds them
// to it (closing an app's window finishes its activity).
void CloseAppWindows(const std::wstring& app_id, std::vector<HWND>* done) {
  HWND app = nullptr;
  while ((app = FindWindowExW(nullptr, app, nullptr, nullptr)) != nullptr) {
    if (!IsWindowVisible(app)) continue;
    if (std::find(done->begin(), done->end(), app) != done->end()) continue;
    wchar_t name[64];
    if (GetClassNameW(app, name, 64) > 0 && std::wcscmp(name, kSplashClass) == 0) continue;
    if (WindowAppUserModelId(app) != app_id) continue;
    PostMessageW(app, WM_CLOSE, 0, 0);
    done->push_back(app);
  }
}

// Shows the splash until the app's window is up, `process` ends, the user
// closes it or the timeout. With `detached` (the launcher returns once the
// activity has started), `process` ending with exit code 0 does not end it: it
// waits for the window kSplashAfterDetachMs more. Returns true if the user
// cancelled the launch; *started then says whether the launcher had already
// started the activity and ended (its processes are not to be terminated: the
// app's windows are closed).
bool SplashWhileStarting(const std::wstring& here, const std::vector<std::wstring>& args,
                         HANDLE process, DWORD pid, bool detached, bool* started) {
  *started = false;
  g_cancelled = false;
  Splash splash;
  if (!SplashFor(here, args, &splash)) return false;
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::vector<HWND> before;
  if (splash.app_id.empty()) EnumWindows(CollectVisible, reinterpret_cast<LPARAM>(&before));
  // The app already has its window (it runs): no splash.
  HWND hwnd = AppWindow(nullptr, splash.app_id, before) == nullptr ? ShowSplash(&splash) : nullptr;
  // The launcher ended, having started the activity (detached): its pid is no
  // longer the launch's.
  bool exited = false;
  if (hwnd != nullptr) {
    const ULONGLONG start = GetTickCount64();
    ULONGLONG next = start + kSplashPollMs;
    ULONGLONG exited_at = 0;
    for (;;) {
      ULONGLONG now = GetTickCount64();
      DWORD wait = now >= next ? 0 : static_cast<DWORD>(next - now);
      DWORD r = exited ? MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT)
                       : MsgWaitForMultipleObjects(1, &process, FALSE, wait, QS_ALLINPUT);
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!exited && r == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (!detached || !GetExitCodeProcess(process, &code) || code != 0) break;
        exited = true;
        exited_at = GetTickCount64();
        next = exited_at;  // look for the window now
      }
      if (!IsWindow(hwnd) || g_cancelled) break;
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
      if (exited && now - exited_at >= kSplashAfterDetachMs) break;
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
  }
  FreeSplash(&splash);

  if (g_cancelled) {
    // A detached launcher may have ended (the activity started) after the loop last
    // looked: its pid is then no longer the launch's, and its job may hold the device.
    DWORD code = 1;
    if (!exited && detached && WaitForSingleObject(process, 0) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process, &code) && code == 0) {
      exited = true;
    }
    if (exited && splash.app_id.empty()) {
      // A first installation may not have had an APK record when the splash
      // opened. Successful Launcher.install records its actual package first.
      Splash installed;
      if (SplashFor(here, args, &installed)) splash.app_id = installed.app_id;
    }
    std::vector<HWND> closed;
    if (!exited) {
      TerminateProcessTree(pid);
      if (!splash.app_id.empty()) CloseAppWindows(splash.app_id, &closed);
    } else if (!splash.app_id.empty()) {
      // The activity started and the launcher is gone: its windows are on their way
      // (or up); each is closed when it comes, for kCancelAfterDetachMs.
      const ULONGLONG deadline = GetTickCount64() + kCancelAfterDetachMs;
      for (;;) {
        CloseAppWindows(splash.app_id, &closed);
        if (GetTickCount64() >= deadline) break;
        Sleep(static_cast<DWORD>(kSplashPollMs));
      }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    *started = exited;
    return true;
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return false;
}

// --- running gesher-app.ps1 ------------------------------------------------------

struct Runner {
  std::wstring powershell;
  std::wstring script;  // gesher-app.ps1, or the control panel's script
  STARTUPINFOW si = {};
  bool inherit = false;  // standard handles are passed on: somebody reads the output
};

struct Child {
  PROCESS_INFORMATION pi = {};
  HANDLE job = nullptr;
  std::wstring error_file;  // where the launcher writes a failure for the user, or ""
};

// Starts the script with `arguments` (as written: quoted where needed), its
// console never shown. With `error_file` and nobody reading the output, a new
// GESHER_LAUNCH_ERROR_FILE for it; with `job`, its processes in a new job.
bool StartChild(const Runner& runner, const std::wstring& arguments, bool error_file, bool job,
                Child* child) {
  static bool error_env_set = false;
  std::wstring command = L"\"" + runner.powershell +
                         L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
                         runner.script + L"\"";
  if (!arguments.empty()) command += L" " + arguments;
  if (error_file && !runner.inherit) {
    child->error_file = NewErrorFile();
    if (!child->error_file.empty()) {
      SetEnvironmentVariableW(kErrorFileEnv, child->error_file.c_str());
      error_env_set = true;
    } else if (error_env_set) {
      SetEnvironmentVariableW(kErrorFileEnv, nullptr);  // not an earlier step's (deleted) file
    }
  }
  STARTUPINFOW si = runner.si;
  if (!CreateProcessW(runner.powershell.c_str(), command.data(), nullptr, nullptr,
                      runner.inherit ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                      &child->pi)) {
    if (!child->error_file.empty()) DeleteFileW(child->error_file.c_str());
    child->error_file.clear();
    return false;
  }
  CloseHandle(child->pi.hThread);
  child->pi.hThread = nullptr;
  if (job) {
    child->job = CreateJobObjectW(nullptr, nullptr);
    if (child->job != nullptr) AssignProcessToJobObject(child->job, child->pi.hProcess);
  }
  return true;
}

// Runs `launch ...` or another command of gesher-app.ps1 as Explorer starts it:
// the splash while the app starts (`args` are the arguments as the splash reads
// them), then waits for the script and shows its failure. Returns its exit code.
int RunLaunch(const Runner& runner, const std::wstring& arguments,
              const std::vector<std::wstring>& args, bool detached, bool error_file) {
  Child child;
  if (!StartChild(runner, arguments, error_file, true, &child)) {
    EndHandover();
    return 1;
  }
  // The splash comes up while powershell starts (the launch is never delayed by it).
  bool started = false;
  bool cancelled = SplashWhileStarting(Directory(runner.script.c_str()), args,
                                       child.pi.hProcess, child.pi.dwProcessId, detached,
                                       &started);
  EndHandover();
  if (cancelled) {
    // The job also holds a device this launch started (its processes cannot break
    // away): once the activity has started, the device and the app stay; the
    // app's windows were closed.
    if (child.job != nullptr) {
      if (!started) TerminateJobObject(child.job, 1);
      CloseHandle(child.job);
    }
    WaitForSingleObject(child.pi.hProcess, 1000);
    CloseHandle(child.pi.hProcess);
    // The user cancelled: nothing to tell them.
    if (!child.error_file.empty()) DeleteFileW(child.error_file.c_str());
    return 1;
  }
  if (child.job != nullptr) {
    CloseHandle(child.job);
  }
  WaitForSingleObject(child.pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(child.pi.hProcess, &code);
  CloseHandle(child.pi.hProcess);
  if (!child.error_file.empty()) {
    std::wstring failure = TakeErrorText(child.error_file);
    if (code != 0) ShowLaunchFailure(failure);
  }
  return static_cast<int>(code);
}

// --- the bare-file flow: confirmation before installing (see the top) -------------

struct StepResult {
  DWORD code = 1;
  bool cancelled = false;   // the user closed the progress window: the step was ended
  std::wstring failure;     // the launcher's text for the user, when it failed
};

// Runs a step (apk-info, install) to its end, `progress` (if any) up meanwhile.
StepResult RunStep(const Runner& runner, const std::wstring& arguments, HWND progress) {
  StepResult result;
  Child child;
  if (!StartChild(runner, arguments, true, true, &child)) return result;
  g_cancelled = false;
  if (progress != nullptr) {
    for (;;) {
      DWORD r = MsgWaitForMultipleObjects(1, &child.pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
      if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (g_cancelled) break;
    }
  }
  if (g_cancelled) {
    if (child.job != nullptr) TerminateJobObject(child.job, 1);
    else TerminateProcessTree(child.pi.dwProcessId);
    WaitForSingleObject(child.pi.hProcess, 1000);
    result.cancelled = true;
  } else {
    WaitForSingleObject(child.pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(child.pi.hProcess, &result.code)) result.code = 1;
  }
  g_cancelled = false;
  CloseHandle(child.pi.hProcess);
  if (child.job != nullptr) CloseHandle(child.job);
  if (!child.error_file.empty()) {
    std::wstring text = TakeErrorText(child.error_file);
    if (!result.cancelled && result.code != 0) result.failure = text;
  }
  return result;
}

// Writes a line to the standard error (else output) handle that was passed on.
void WriteOutput(const Runner& runner, const std::wstring& text) {
  HANDLE h = runner.si.hStdError;
  if (h == nullptr || h == INVALID_HANDLE_VALUE) h = runner.si.hStdOutput;
  if (h == nullptr || h == INVALID_HANDLE_VALUE || text.empty()) return;
  int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
  if (n <= 0) return;
  std::string bytes(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n,
                      nullptr, nullptr);
  DWORD written = 0;
  WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

// A failed step: its reason for the user (or, when somebody reads the output,
// there), the progress window gone. Never 0.
int FailStep(const Runner& runner, const std::wstring& reason, HWND progress, DWORD code) {
  const bool owner = progress != nullptr && IsWindow(progress);
  if (runner.inherit) WriteOutput(runner, L"gesher-launch: " + reason + L"\n");
  else ShowLaunchFailure(reason, owner ? progress : nullptr);
  if (owner) DestroyWindow(progress);
  return code != 0 ? static_cast<int>(code) : 1;
}

std::wstring Quote(const std::wstring& path) { return L"\"" + path + L"\""; }

std::wstring FullPath(const std::wstring& path) {
  DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0) return path;
  std::vector<wchar_t> full(required);
  DWORD count = GetFullPathNameW(path.c_str(), required, full.data(), nullptr);
  return count > 0 && count < required ? std::wstring(full.data(), count) : path;
}

// A fresh staging directory in the selected profile, or in the user's temporary directory
// when no profile has been created yet.
std::wstring NewInstallDirectory(const std::wstring& data) {
  std::wstring dir;
  if (!data.empty()) {
    const std::wstring system = data + L"\\system";
    const DWORD attributes = GetFileAttributesW(system.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
      dir = system + L"\\";
    }
  }
  if (dir.empty()) {
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    if (n == 0 || n > MAX_PATH) return std::wstring();
    dir = temp;
  }
  GUID guid;
  wchar_t text[64];
  if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, text, 64) == 0) return std::wstring();
  dir += L"gesher-install-" + std::wstring(text + 1, wcslen(text) - 2);
  // The temporary directory can be shared. Scope the staging tree's inherited
  // access to its owner and SYSTEM even when the parent has a more open ACL.
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
    return std::wstring();
  }
  SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
  const bool created = CreateDirectoryW(dir.c_str(), &security) != 0;
  LocalFree(descriptor);
  return created ? dir : std::wstring();
}

// Keep the launcher-owned directory and its copied files open through the dialog and
// install. The host service also verifies their digest as it captures its own inputs.
class InstallSnapshot {
 public:
  InstallSnapshot() = default;
  InstallSnapshot(const InstallSnapshot&) = delete;
  InstallSnapshot& operator=(const InstallSnapshot&) = delete;
  ~InstallSnapshot() { Reset(); }

  bool Create(const std::wstring& name, const std::wstring& data) {
    directory_ = NewInstallDirectory(data);
    if (directory_.empty()) return false;
    directory_lock_ = PinDirectory(directory_);
    if (directory_lock_ == INVALID_HANDLE_VALUE) return false;
    const std::wstring payload = directory_ + L"\\payload";
    if (!CreateDirectoryW(payload.c_str(), nullptr)) return false;
    payload_ = payload;
    payload_lock_ = PinDirectory(payload_);
    if (payload_lock_ == INVALID_HANDLE_VALUE) return false;
    apk_ = payload_ + L"\\" + name;
    return true;
  }

  bool PinInputs() {
    bool found_apk = false;
    WIN32_FIND_DATAW entry = {};
    HANDLE entries = FindFirstFileW((payload_ + L"\\*").c_str(), &entry);
    if (entries == INVALID_HANDLE_VALUE) return false;
    bool valid = true;
    do {
      if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0) {
        const std::wstring path = payload_ + L"\\" + entry.cFileName;
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
          valid = false;
          break;
        }
        files_.push_back(handle);
        FILE_ATTRIBUTE_TAG_INFO attributes = {};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
          valid = false;
          break;
        }
        if (CompareStringOrdinal(path.c_str(), -1, apk_.c_str(), -1, TRUE) == CSTR_EQUAL) {
          apk_ = path;
          found_apk = true;
        }
      }
    } while (FindNextFileW(entries, &entry));
    const bool complete = valid && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(entries);
    return complete && found_apk;
  }

  const std::wstring& apk() const { return apk_; }
  const std::wstring& payload() const { return payload_; }
  std::wstring info() const { return directory_ + L"\\info.txt"; }

  void Reset() {
    for (HANDLE file : files_) CloseHandle(file);
    files_.clear();
    if (!payload_.empty() && payload_lock_ != INVALID_HANDLE_VALUE) {
      WIN32_FIND_DATAW entry = {};
      HANDLE entries = FindFirstFileW((payload_ + L"\\*").c_str(), &entry);
      if (entries != INVALID_HANDLE_VALUE) {
        do {
          if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            DeleteFileW((payload_ + L"\\" + entry.cFileName).c_str());
          }
        } while (FindNextFileW(entries, &entry));
        FindClose(entries);
      }
      CloseHandle(payload_lock_);
    }
    if (!payload_.empty()) RemoveDirectoryW(payload_.c_str());
    if (!directory_.empty() && directory_lock_ != INVALID_HANDLE_VALUE) {
      DeleteFileW(info().c_str());
      DeleteFileW((directory_ + L"\\info.txt.ico").c_str());
      CloseHandle(directory_lock_);
    }
    if (!directory_.empty()) RemoveDirectoryW(directory_.c_str());
    payload_lock_ = directory_lock_ = INVALID_HANDLE_VALUE;
    apk_.clear();
    payload_.clear();
    directory_.clear();
  }

 private:
  static HANDLE PinDirectory(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    FILE_ATTRIBUTE_TAG_INFO attributes = {};
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
            FILE_ATTRIBUTE_DIRECTORY) {
      return handle;
    }
    CloseHandle(handle);
    return INVALID_HANDLE_VALUE;
  }

  std::wstring directory_;
  std::wstring payload_;
  std::wstring apk_;
  HANDLE directory_lock_ = INVALID_HANDLE_VALUE;
  HANDLE payload_lock_ = INVALID_HANDLE_VALUE;
  std::vector<HANDLE> files_;
};

// GetDataDirectory's portable layout: the marker beside `dir`, or beside the App folder it is.
bool IsPortable(const std::wstring& dir) {
  if (IsFile(dir + L"\\portable.marker")) return true;
  size_t slash = dir.find_last_of(L"\\/");
  std::wstring leaf = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
  return Lower(leaf) == L"app" && IsFile(Directory(dir.c_str()) + L"\\portable.marker");
}

// A pattern's {0} replaced once (an app's label containing "{0}" is no template).
std::wstring FormatText(const wchar_t* pattern, const std::wstring& value) {
  std::wstring text(pattern);
  const size_t at = text.find(L"{0}");
  if (at != std::wstring::npos) text.replace(at, 3, value);
  return text;
}

struct InstallDialog {
  bool confirm = true;  // the confirmation; else the installed dialog
  bool update = false;  // the package is installed (another version)
  bool offer_shortcut = false;
  bool shortcut = false;  // the result: the check box was checked
  bool rtl = false;
  std::wstring label, version, question, accept, cancel;
  std::wstring icon_path;
  HICON icon = nullptr;
  int icon_px = 0;
  HFONT heading_font = nullptr;
  GesherHostTheme theme = {};
  GesherHostIcons icons = {};
};

// The dialogs' layout, in dialog units: a 300-unit wide dialog, the icon in the
// reading direction's leading corner, the buttons as GesherHostPrompt places them.
constexpr short kDialogWidth = 300;

short InstallDialogHeight(const InstallDialog& state) {
  if (!state.confirm) return 118;
  return state.offer_shortcut ? 144 : 126;
}

void DrawAppIcon(const DRAWITEMSTRUCT* item, InstallDialog* state) {
  FillRect(item->hDC, &item->rcItem, GesherHostSurface(&state->theme));
  const int dpi = GesherHostDpi(item->hwndItem, item->hDC);
  const int w = item->rcItem.right - item->rcItem.left, h = item->rcItem.bottom - item->rcItem.top;
  const int size = std::min(MulDiv(kDialogIconSize, dpi, 96), std::min(w, h));
  if (size <= 0) return;
  if (state->icon == nullptr || state->icon_px != size) {
    if (state->icon != nullptr) DestroyIcon(state->icon);
    state->icon = static_cast<HICON>(
        LoadImageW(nullptr, state->icon_path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
    state->icon_px = size;
  }
  if (state->icon == nullptr) return;
  const int x = state->rtl ? item->rcItem.right - size : item->rcItem.left;
  DrawIconEx(item->hDC, x, item->rcItem.top, state->icon, size, size, 0, nullptr, DI_NORMAL);
}

bool CreateInstallControls(HWND dialog, InstallDialog* state) {
  const DWORD reading = state->rtl ? WS_EX_RTLREADING : 0;
  const DWORD align = (state->rtl ? SS_RIGHT : SS_LEFT) | SS_NOPREFIX;
  const int icon_x = state->rtl ? kDialogWidth - 16 - 40 : 16;
  const int text_x = state->rtl ? 16 : 64;
  HWND icon = GesherHostControl(dialog, 0, L"STATIC", L"", SS_OWNERDRAW, kAppIconId, icon_x, 14,
                                40, 36);
  // The label and the confirmation's version line are one line each: a long one ends in
  // an ellipsis rather than being cut off.
  HWND heading = GesherHostControl(dialog, StartsRtl(state->label) ? WS_EX_RTLREADING : 0,
                                   L"STATIC", state->label.c_str(), align | SS_ENDELLIPSIS,
                                   GESHER_HOST_MESSAGE_HEADING, text_x, 14, 220, 24);
  HWND version = GesherHostControl(dialog, reading, L"STATIC", state->version.c_str(),
                                   align | (state->confirm ? SS_ENDELLIPSIS : 0), kVersionTextId,
                                   text_x, 40, 220, state->confirm ? 12 : 34);
  if (!icon || !heading || !version) return false;
  int buttons_y = 80;
  if (state->confirm) {
    if (!GesherHostControl(dialog, reading, L"STATIC", state->question.c_str(), align,
                           kQuestionTextId, 16, 60, kDialogWidth - 32, 20)) {
      return false;
    }
    buttons_y = 88;
    if (state->offer_shortcut) {
      if (!GesherHostControl(dialog, reading, L"BUTTON",
                             GesherUiText(GesherUiCurrentLanguage(), "install_desktop_shortcut"),
                             WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE |
                                 (state->rtl ? BS_LEFTTEXT | BS_RIGHT : 0),
                             kDesktopShortcutId, 16, 84, kDialogWidth - 32, 14)) {
        return false;
      }
      buttons_y = 106;
    }
  }
  // Cancel (Done) first: Tab order follows the on-screen order.
  HWND cancel = GesherHostControl(dialog, 0, L"BUTTON", state->cancel.c_str(),
                                  WS_TABSTOP | BS_OWNERDRAW, IDCANCEL, 124, buttons_y, 76, 24);
  HWND accept = GesherHostControl(dialog, 0, L"BUTTON", state->accept.c_str(),
                                  WS_TABSTOP | BS_OWNERDRAW, state->confirm ? IDOK : kOpenId, 208,
                                  buttons_y, 76, 24);
  if (!cancel || !accept) return false;
  GesherHostSetFont(heading, &state->heading_font, 145, FW_SEMIBOLD);
  return true;
}

INT_PTR CALLBACK InstallDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<InstallDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<InstallDialog*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER, lparam);
    GesherHostThemeInit(&state->theme, dialog);
    if (!CreateInstallControls(dialog, state)) {
      EndDialog(dialog, IDCANCEL);
      return FALSE;
    }
    // Cancel (Done) is the default and has the focus: installing takes a click
    // on Install, or Tab and Enter.
    SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
    GesherHostSetIcons(dialog, &state->icons);
    if (!SetForegroundWindow(dialog)) {
      FLASHWINFO flash = {sizeof(flash), dialog, FLASHW_TRAY, 3, 0};
      FlashWindowEx(&flash);
    }
    EndHandover();
    SetFocus(GetDlgItem(dialog, IDCANCEL));
    return FALSE;
  }
  if (state == nullptr) return FALSE;
  if (message == WM_DESTROY) {
    if (state->heading_font != nullptr) DeleteObject(state->heading_font);
    state->heading_font = nullptr;
    if (state->icon != nullptr) DestroyIcon(state->icon);
    state->icon = nullptr;
    GesherHostThemeDispose(&state->theme);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, GESHER_HOST_MESSAGE_HEADING), &state->heading_font, 145,
                      FW_SEMIBOLD);
    InvalidateRect(GetDlgItem(dialog, kAppIconId), nullptr, TRUE);
    return TRUE;
  }
  if (message == WM_CLOSE) {
    EndDialog(dialog, IDCANCEL);
    return TRUE;
  }
  if (message == WM_COMMAND) {
    const int command = LOWORD(wparam);
    if (command == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
    if (command == IDOK && state->confirm) {
      state->shortcut = state->offer_shortcut &&
                        IsDlgButtonChecked(dialog, kDesktopShortcutId) == BST_CHECKED;
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (command == kOpenId && !state->confirm) {
      EndDialog(dialog, kOpenId);
      return TRUE;
    }
  }
  if (message == WM_DRAWITEM && lparam != 0) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (item->CtlType == ODT_STATIC && item->CtlID == static_cast<UINT>(kAppIconId)) {
      DrawAppIcon(item, state);
      return TRUE;
    }
  }
  return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam,
                              GESHER_HOST_MESSAGE_HEADING);
}

// Shows the dialog (modal, top-level, owned by nobody); IDCANCEL when it could not.
INT_PTR RunInstallDialog(InstallDialog* state, const wchar_t* title) {
  union {
    DWORD align;
    WORD words[512];
  } buffer = {};
  auto* layout = reinterpret_cast<DLGTEMPLATE*>(buffer.words);
  layout->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  layout->dwExtendedStyle = state->rtl ? WS_EX_RTLREADING : 0;
  layout->cx = kDialogWidth;
  layout->cy = InstallDialogHeight(*state);
  WORD* at = reinterpret_cast<WORD*>(layout + 1);
  *at++ = 0;  // no menu
  *at++ = 0;  // the dialog class
  const size_t chars = wcslen(title) + 1;
  const wchar_t font[] = L"Segoe UI";
  const size_t font_chars = sizeof(font) / sizeof(font[0]);
  if (static_cast<size_t>(at - buffer.words) + chars + 1 + font_chars >
      sizeof(buffer.words) / sizeof(buffer.words[0])) {
    EndHandover();
    return IDCANCEL;
  }
  for (size_t i = 0; i < chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (size_t i = 0; i < font_chars; ++i) *at++ = font[i];
  GesherHostLoadIcons(&state->icons);
  INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), layout, nullptr,
                                           InstallDialogProc, reinterpret_cast<LPARAM>(state));
  GesherHostFreeIcons(&state->icons);
  EndHandover();
  return result <= 0 ? IDCANCEL : result;
}

// Whether the package's shortcut record names a desktop shortcut that exists.
bool DesktopLinkRecorded(const std::wstring& data, const std::wstring& package) {
  if (data.empty() || package.empty()) return false;
  std::string record;
  if (!ReadBytes(data + L"\\data\\" + package + L"\\gesher-shell\\shortcut.properties", &record)) {
    return false;
  }
  const std::wstring link = PropertiesValue(record, L"desktop_link");
  return !link.empty() && IsFile(link);
}

// `gesher-launch <file>`: reads the APK, asks before installing it, offers to open it.
int RunBareFile(const Runner& runner, const std::wstring& here, const std::wstring& file,
                bool detached) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const auto language = GesherUiCurrentLanguage();
  const std::wstring scripts = Directory(runner.script.c_str());
  const std::wstring apk = FullPath(file);
  const size_t slash = apk.find_last_of(L"\\/");
  const std::wstring name = slash == std::wstring::npos ? apk : apk.substr(slash + 1);
  const std::wstring data = GetDataDirectory(here);
  // 1. What the file is, and whether its package is installed.
  Splash reading;
  reading.label = name;
  reading.icon = GesherIcon(scripts);
  reading.status = GesherUiText(language, "install_reading");
  g_cancelled = false;
  HWND progress = ShowProgress(&reading);
  InstallSnapshot snapshot;
  if (!snapshot.Create(name, data)) {
    const int failure = g_cancelled
                            ? kExitCancelled
                            : FailStep(runner, L"Could not copy " + name + L" for installation.",
                                       progress, 1);
    FreeSplash(&reading);
    return failure;
  }
  const std::wstring info_file = snapshot.info();
  StepResult step = RunStep(runner, L"apk-info " + Quote(apk) + L" " + Quote(info_file) +
                                       L" " + Quote(snapshot.payload()), progress);
  std::string bytes;
  const bool read = !step.cancelled && step.code == 0 && ReadBytes(info_file, &bytes);
  DeleteFileW(info_file.c_str());
  GesherApkInfo info;
  int result = 0;
  if (step.cancelled) {
    result = kExitCancelled;  // the user closed the progress window
  } else if (step.code != 0) {
    result = FailStep(runner,
                      step.failure.empty()
                          ? L"gesher-app apk-info " + name + L" failed (exit code " +
                                std::to_wstring(step.code) + L")."
                          : step.failure,
                      progress, step.code);
  } else if (!read || !GesherParseApkInfo(bytes, &info) || info.inputs_sha256.empty() ||
             !snapshot.PinInputs()) {
    result = FailStep(runner, L"gesher-app apk-info gave no package information for " + name + L".",
                      progress, 1);
  }
  if (step.cancelled || result != 0) {
    if (IsWindow(progress)) DestroyWindow(progress);
    FreeSplash(&reading);
    return result;
  }

  // 2. This version is installed: opening the file opens the app.
  const std::vector<std::wstring> launch = {L"launch", info.package};
  if (GesherApkInfoSameVersion(info)) {
    snapshot.Reset();
    g_handover = progress;
    result = RunLaunch(runner, L"launch " + info.package, launch, detached, true);
    FreeSplash(&reading);
    return result;
  }

  // 3. Install or update it?
  const std::wstring icon = !info.icon.empty() && IsFile(info.icon) ? info.icon : GesherIcon(scripts);
  InstallDialog confirm;
  confirm.update = info.installed;
  confirm.rtl = GesherUiRtl(language) != 0;
  confirm.offer_shortcut = !IsPortable(here) && !IsPortable(scripts) &&
                           Env(L"GESHER_SHORTCUTS") != L"0" && !info.desktop_shortcut;
  confirm.label = info.label;
  confirm.version = FormatText(GesherUiText(language, "install_version"),
                               Embed(info.version_name.empty() ? std::to_wstring(info.version_code)
                                                               : info.version_name,
                                     confirm.rtl));
  confirm.question =
      GesherUiText(language, confirm.update ? "update_confirm_question" : "install_confirm_question");
  confirm.accept = GesherUiText(language, confirm.update ? "update_button" : "install_button");
  confirm.cancel = GesherUiText(language, "chooser_cancel");
  confirm.icon_path = icon;
  g_handover = progress;
  const INT_PTR answer = RunInstallDialog(&confirm, GesherUiText(language, "install_confirm_title"));
  FreeSplash(&reading);
  if (answer != IDOK) return kExitCancelled;

  // 4. Install it.
  Splash installing;
  installing.label = info.label;
  installing.icon = icon;
  installing.status =
      FormatText(GesherUiText(language, "install_installing"), Embed(info.label, confirm.rtl));
  installing.closable = false;
  progress = ShowProgress(&installing);
  step = RunStep(runner,
                 L"install " + Quote(snapshot.apk()) + L" --confirmed " + Quote(info.package) +
                     L" " + std::to_wstring(info.version_code) + L" " + info.inputs_sha256 +
                     (confirm.shortcut ? L" --desktop-shortcut" : L""),
                 progress);
  if (step.cancelled || step.code != 0) {
    result = step.cancelled
                 ? kExitCancelled
                 : FailStep(runner,
                            step.failure.empty()
                                ? L"gesher-app install " + name + L" failed (exit code " +
                                      std::to_wstring(step.code) + L")."
                                : step.failure,
                            progress, step.code);
    if (IsWindow(progress)) DestroyWindow(progress);
    FreeSplash(&installing);
    return result;
  }

  // 5. Installed: open it?
  InstallDialog done;
  done.confirm = false;
  done.rtl = confirm.rtl;
  done.label = info.label;
  done.version = FormatText(
      GesherUiText(language, confirm.update ? "update_done_text" : "install_done_text"),
      Embed(info.label, confirm.rtl));
  if (confirm.shortcut && !DesktopLinkRecorded(data, info.package)) {
    // Asked for, but not made (install printed why): say so, never imply it is there.
    done.version += L"\r\n";
    done.version += GesherUiText(language, "install_desktop_failed");
  }
  done.accept = GesherUiText(language, "install_open_button");
  done.cancel = GesherUiText(language, "install_done_button");
  done.icon_path = icon;
  g_handover = progress;
  const INT_PTR open = RunInstallDialog(&done, GesherUiText(language, "install_done_title"));
  FreeSplash(&installing);
  if (open != kOpenId) return 0;
  snapshot.Reset();
  return RunLaunch(runner, L"launch " + info.package, launch, detached, true);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  bool default_to_control_panel = false;
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
      if (argc <= 1) {
        default_to_control_panel = true;
      } else if (argc > 1 && wcscmp(argv[1], L"--app-crash") == 0) {
        int result = ShowAppCrash(argc, argv);
        LocalFree(argv);
        return result;
      }
      LocalFree(argv);
    }
  }
  wchar_t self[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(sizeof(self) / sizeof(self[0])));
  if (n == 0 || n >= sizeof(self) / sizeof(self[0])) return 1;
  const std::wstring here = Directory(self);

  wchar_t system[MAX_PATH];
  UINT s = GetSystemDirectoryW(system, MAX_PATH);
  Runner runner;
  runner.powershell = (s > 0 && s < MAX_PATH ? std::wstring(system) : L"C:\\Windows\\System32") +
                      L"\\WindowsPowerShell\\v1.0\\powershell.exe";

  AllowSetForegroundWindow(ASFW_ANY);

  runner.si.cb = sizeof(runner.si);
  runner.si.dwFlags = STARTF_USESHOWWINDOW;
  runner.si.wShowWindow = SW_HIDE;
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE),
         err = GetStdHandle(STD_ERROR_HANDLE);
  runner.inherit = (out != nullptr && out != INVALID_HANDLE_VALUE) ||
                   (err != nullptr && err != INVALID_HANDLE_VALUE);
  if (runner.inherit) {
    runner.si.dwFlags |= STARTF_USESTDHANDLES;
    runner.si.hStdInput = in;
    runner.si.hStdOutput = out;
    runner.si.hStdError = err;
  }
  // Nobody reads the output: the launcher does not wait for the app (powershell
  // inherits this environment). A caller's own setting is left as it is.
  if (!runner.inherit && Env(kDetachEnv).empty()) SetEnvironmentVariableW(kDetachEnv, L"1");
  // The splash's view of it: gesher-app's own --detach / --wait (the launcher's
  // last one wins) over the environment.
  bool detached = Env(kDetachEnv) == L"1";
  const bool environment_detached = detached;
  bool control_panel = default_to_control_panel;
  const std::vector<std::wstring> args = CommandArguments();
  if (!args.empty()) {
    const std::wstring command_name = Lower(args[0]);
    control_panel = command_name == L"control-panel" || command_name == L"settings" ||
                    command_name == L"dashboard" || command_name == L"gui";
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == L"--detach") detached = true;
    if (args[i] == L"--wait") detached = false;
    // Launcher consumes option values before looking for the next option.
    // An extra/shortcut named "--wait" must not change splash ownership.
    if (args[i] == L"--ez") i += 2;
    else if (args[i] == L"--activity" || args[i] == L"--proxy" || args[i] == L"--shortcut") ++i;
  }

  auto find_script = [&](const wchar_t* name) -> std::wstring {
    std::wstring candidate = here + L"\\" + name;
    if (IsFile(candidate)) return candidate;
    std::wstring app_candidate = here + L"\\App\\" + name;
    if (IsFile(app_candidate)) return app_candidate;
    return L"";
  };

  runner.script = find_script(L"gesher-app.ps1");
  if (runner.script.empty()) runner.script = here + L"\\gesher-app.ps1";

  // An APK or bundle opened from Explorer: installed only once the user agrees.
  if (!default_to_control_panel && args.size() == 1 && !IsCommand(args[0]) && IsFile(args[0])) {
    return RunBareFile(runner, here, args[0], environment_detached);
  }

  bool direct_control_panel = false;
  if (control_panel) {
    std::wstring cp_script = find_script(L"gesher-control-panel.ps1");
    if (!cp_script.empty()) {
      runner.script = cp_script;
      direct_control_panel = true;
    }
  }

  std::wstring command_args;
  if (!direct_control_panel) {
    command_args = default_to_control_panel ? L"control-panel" : Arguments(GetCommandLineW());
  }
  if (control_panel && !runner.inherit) {
    // The panel owns its own lifetime (see the top).
    Child child;
    if (!StartChild(runner, command_args, false, false, &child)) return 1;
    CloseHandle(child.pi.hProcess);
    return 0;
  }
  // Nobody reads the output: a failure is shown in a message box (ShowLaunchFailure).
  return RunLaunch(runner, command_args, args, detached, !control_panel);
}
