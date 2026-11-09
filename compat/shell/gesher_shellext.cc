// gesher_shellext.dll: the icon handler of Gesher's .apk file type
// (Gesher.Apk\shellex\IconHandler, docs/installer.md). Explorer asks it for each
// .apk file's icon: its app's icon (the .ico com.gesher.app.shell.AppShell made
// for the app's Start menu shortcut, or drew for the file), Gesher's own
// (gesher.ico, next to this DLL) until that is drawn.
//
// It parses no APK: Explorer loads it in its own process, for every .apk it
// shows. Gesher records APK files and their icons in
// %LOCALAPPDATA%\Gesher\data\system\apk-icons.list (AppShell.rememberApkQuietly for
// the ones it installs, AppShell.apkIcon for any other), a line per file: its
// size, its last write time (milliseconds since 1970), the app's .ico and the
// package, tab-separated, UTF-8. A file is the one recorded when its size and
// time are; a copy or a move keeps both.
//
// An APK not recorded shows gesher.ico for now, and its icon is drawn in the
// background: a thread of this DLL runs `gesher-launch.exe apk-icon "<file>"`
// (next to this DLL; no window), which draws the app's icon without installing
// the app, records it and tells the shell the file changed, so Explorer asks
// again. Never on the caller's thread; once per file (path, size and time) in
// this process, at most kQueued waiting; at most two runs at once in the
// session (two named mutexes, shared by every process that loads this DLL);
// not for an empty file or one written in the last seconds (still being
// written).
//
// In-process COM server, apartment threaded: IPersistFile::Load gets the file,
// IExtractIconW::GetIconLocation names the icon, and the shell extracts it.
//
// The COM and shell headers need what WIN32_LEAN_AND_MEAN and NOGDI (the
// build's defaults) leave out.
#undef WIN32_LEAN_AND_MEAN
#undef NOGDI
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <new>

namespace {

// {6B0C3C9E-2D1B-4E57-9B7A-3F2C8E4D1A50}: Gesher.Apk's icon handler.
const CLSID kClsidIconHandler = {
    0x6b0c3c9e, 0x2d1b, 0x4e57, {0x9b, 0x7a, 0x3f, 0x2c, 0x8e, 0x4d, 0x1a, 0x50}};
// The interfaces' IIDs, spelled out: not every one is in llvm-mingw's libuuid
// (as in shell_win32.cc).
const IID kIidUnknown = {0x00000000, 0x0000, 0x0000,
                         {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidClassFactory = {0x00000001, 0x0000, 0x0000,
                              {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPersist = {0x0000010C, 0x0000, 0x0000,
                         {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPersistFile = {0x0000010B, 0x0000, 0x0000,
                             {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidExtractIconW = {0x000214FA, 0x0000, 0x0000,
                              {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

HMODULE g_module = nullptr;
volatile LONG g_objects = 0;
volatile LONG g_locks = 0;

// gesher.ico next to this DLL.
bool DefaultIcon(wchar_t* out, UINT cch) {
  DWORD n = GetModuleFileNameW(g_module, out, cch);
  if (n == 0 || n >= cch) return false;
  wchar_t* slash = nullptr;
  for (wchar_t* p = out; *p; ++p) {
    if (*p == L'\\' || *p == L'/') slash = p;
  }
  if (slash == nullptr || static_cast<UINT>(slash - out) + 12 >= cch) return false;
  lstrcpyW(slash + 1, L"gesher.ico");
  return true;
}

unsigned long long ParseUnsigned(const char*& p, const char* end) {
  unsigned long long v = 0;
  while (p < end && *p >= '0' && *p <= '9') v = v * 10 + static_cast<unsigned>(*p++ - '0');
  return v;
}

// The .ico recorded for a file of `size` bytes last written at `millis`.
bool RecordedIcon(unsigned long long size, unsigned long long millis, wchar_t* out, UINT cch) {
  if (cch < 2) return false;
  wchar_t list[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", list, MAX_PATH);
  if (n == 0 || n + 40 >= MAX_PATH) return false;
  lstrcatW(list, L"\\Gesher\\data\\system\\apk-icons.list");
  HANDLE f = CreateFileW(list, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length;
  bool found = false;
  if (GetFileSizeEx(f, &length) && length.QuadPart > 0 && length.QuadPart < (4 << 20)) {
    DWORD bytes = static_cast<DWORD>(length.QuadPart);
    char* text = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    DWORD read = 0;
    if (text != nullptr && ReadFile(f, text, bytes, &read, nullptr)) {
      const char* p = text;
      const char* end = text + read;
      while (p < end && !found) {
        const char* line_end = p;
        while (line_end < end && *line_end != '\n') ++line_end;
        const char* q = p;
        unsigned long long s = ParseUnsigned(q, line_end);
        if (q < line_end && *q == '\t') {
          ++q;
          unsigned long long m = ParseUnsigned(q, line_end);
          if (q < line_end && *q == '\t' && s == size && m == millis) {
            ++q;
            const char* icon_end = q;
            while (icon_end < line_end && *icon_end != '\t' && *icon_end != '\r') ++icon_end;
            int w = MultiByteToWideChar(CP_UTF8, 0, q, static_cast<int>(icon_end - q), out,
                                        static_cast<int>(cch) - 1);
            if (w > 0) {
              out[w] = L'\0';
              found = GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
            }
          }
        }
        p = line_end + 1;
      }
    }
    if (text != nullptr) HeapFree(GetProcessHeap(), 0, text);
  }
  CloseHandle(f);
  return found;
}

// --- drawing a missing icon in the background ----------------------------------

constexpr UINT kPathChars = MAX_PATH * 4;
// Files waiting to be drawn, and the files asked for in this process (a hash of
// path, size and time each); while either is full no more are asked for.
constexpr int kQueued = 256;
constexpr int kSeen = 1024;
// A file written this recently may still be being written (milliseconds).
constexpr unsigned long long kSettleMs = 3000;
// How long a run of `gesher-launch apk-icon` may take before the next one starts.
constexpr DWORD kRunTimeoutMs = 180000;
// How long a request waits for one of the two run slots.
constexpr DWORD kSlotTimeoutMs = 600000;

struct IconRequest {
  wchar_t path[kPathChars];
  unsigned long long size;
  unsigned long long millis;
};

CRITICAL_SECTION g_queue_lock;  // initialized in DllMain
IconRequest g_queue[kQueued];
int g_queue_head = 0;
int g_queue_count = 0;
unsigned long long g_seen[kSeen];
int g_seen_count = 0;
bool g_worker_running = false;

unsigned long long FileTimeMillis(unsigned long long ft) {
  // FILETIME (100 ns since 1601) as java.io.File.lastModified's milliseconds.
  return ft >= 116444736000000000ULL ? (ft - 116444736000000000ULL) / 10000ULL : 0;
}

// The file's size and last write time.
bool FileKey(const wchar_t* path, unsigned long long* size, unsigned long long* millis) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (path[0] == L'\0' || !GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
  *size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  *millis = FileTimeMillis((static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime)
                            << 32) | data.ftLastWriteTime.dwLowDateTime);
  return true;
}

unsigned long long NowMillis() {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  return FileTimeMillis((static_cast<unsigned long long>(now.dwHighDateTime) << 32) |
                        now.dwLowDateTime);
}

unsigned long long RequestHash(const wchar_t* path, unsigned long long size,
                               unsigned long long millis) {
  unsigned long long h = 14695981039346656037ULL;  // FNV-1a
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    unsigned c = static_cast<unsigned>(*p);
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c == '/') c = '\\';
    h = (h ^ c) * 1099511628211ULL;
  }
  h = (h ^ size) * 1099511628211ULL;
  h = (h ^ millis) * 1099511628211ULL;
  return h;
}

// The directory of this DLL, with its trailing backslash.
bool ModuleDirectory(wchar_t* out, UINT cch) {
  DWORD n = GetModuleFileNameW(g_module, out, cch);
  if (n == 0 || n >= cch) return false;
  wchar_t* slash = nullptr;
  for (wchar_t* p = out; *p != L'\0'; ++p) {
    if (*p == L'\\' || *p == L'/') slash = p;
  }
  if (slash == nullptr) return false;
  slash[1] = L'\0';
  return true;
}

// SHChangeNotify(SHCNE_UPDATEITEM): the shell reads the file's icon again.
void NotifyShell(const wchar_t* path) {
  using SHChangeNotifyFn = void(WINAPI*)(LONG, UINT, LPCVOID, LPCVOID);
  HMODULE shell32 = GetModuleHandleW(L"shell32.dll");  // loaded: the shell asked for an icon
  if (shell32 == nullptr) return;
  auto notify = reinterpret_cast<SHChangeNotifyFn>(
      reinterpret_cast<void*>(GetProcAddress(shell32, "SHChangeNotify")));
  if (notify != nullptr) notify(SHCNE_UPDATEITEM, SHCNF_PATHW, path, nullptr);
}

// Runs `gesher-launch.exe apk-icon "<file>"` for a request, in one of the two run
// slots, and waits for it.
void DrawIcon(const IconRequest& r) {
  // Not while the file is being written: its size or time still change. A file
  // that changed is asked for again with its new size and time.
  for (int i = 0;; ++i) {
    unsigned long long size = 0, millis = 0;
    if (!FileKey(r.path, &size, &millis) || size == 0) return;
    if (size != r.size || millis != r.millis) return;
    unsigned long long now = NowMillis();
    if (now < millis || now - millis >= kSettleMs) break;
    if (i >= 10) return;
    Sleep(static_cast<DWORD>(kSettleMs - (now - millis)) + 100);
  }
  wchar_t icon[kPathChars];
  if (RecordedIcon(r.size, r.millis, icon, kPathChars)) return;  // drawn meanwhile

  wchar_t dir[kPathChars];
  if (!ModuleDirectory(dir, kPathChars)) return;
  if (lstrlenW(dir) + 20 >= static_cast<int>(kPathChars)) return;
  wchar_t exe[kPathChars];
  lstrcpyW(exe, dir);
  lstrcatW(exe, L"gesher-launch.exe");
  if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
  // "<exe>" apk-icon "<file>" (a Windows path holds no '"').
  constexpr UINT kCommandChars = kPathChars * 2 + 32;
  wchar_t command[kCommandChars];
  if (lstrlenW(exe) + lstrlenW(r.path) + 20 >= static_cast<int>(kCommandChars)) return;
  lstrcpyW(command, L"\"");
  lstrcatW(command, exe);
  lstrcatW(command, L"\" apk-icon \"");
  lstrcatW(command, r.path);
  lstrcatW(command, L"\"");

  HANDLE slots[2] = {CreateMutexW(nullptr, FALSE, L"Local\\Gesher.ApkIcon.0"),
                     CreateMutexW(nullptr, FALSE, L"Local\\Gesher.ApkIcon.1")};
  if (slots[0] == nullptr || slots[1] == nullptr) {
    if (slots[0] != nullptr) CloseHandle(slots[0]);
    if (slots[1] != nullptr) CloseHandle(slots[1]);
    return;
  }
  DWORD w = WaitForMultipleObjects(2, slots, FALSE, kSlotTimeoutMs);
  HANDLE slot = nullptr;
  if (w == WAIT_OBJECT_0 || w == WAIT_OBJECT_0 + 1) {
    slot = slots[w - WAIT_OBJECT_0];
  } else if (w == WAIT_ABANDONED_0 || w == WAIT_ABANDONED_0 + 1) {
    slot = slots[w - WAIT_ABANDONED_0];  // its owner ended without releasing it: free
  }
  if (slot != nullptr && !RecordedIcon(r.size, r.millis, icon, kPathChars)) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    // No window, and no standard handles (gesher-launch passes on those it has).
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nullptr;
    si.hStdOutput = nullptr;
    si.hStdError = nullptr;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe, command, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, dir, &si, &pi)) {
      CloseHandle(pi.hThread);
      DWORD code = 1;
      if (WaitForSingleObject(pi.hProcess, kRunTimeoutMs) == WAIT_OBJECT_0 &&
          GetExitCodeProcess(pi.hProcess, &code) && code == 0) {
        // The command tells the shell too; this notice cannot be lost with the
        // process that sent it.
        NotifyShell(r.path);
      }
      CloseHandle(pi.hProcess);
    }
  }
  if (slot != nullptr) ReleaseMutex(slot);
  CloseHandle(slots[0]);
  CloseHandle(slots[1]);
}

// The background thread: draws the queued requests, then ends. The DLL is
// pinned (DllMain), so it is still mapped whenever this runs or returns.
DWORD WINAPI Worker(LPVOID) {
  for (;;) {
    IconRequest r;
    EnterCriticalSection(&g_queue_lock);
    if (g_queue_count == 0) {
      g_worker_running = false;
      LeaveCriticalSection(&g_queue_lock);
      break;
    }
    r = g_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % kQueued;
    --g_queue_count;
    LeaveCriticalSection(&g_queue_lock);
    DrawIcon(r);
  }
  return 0;
}

// Asks for the icon of `path`, not recorded, to be drawn in the background.
void RequestIcon(const wchar_t* path, unsigned long long size, unsigned long long millis) {
  if (size == 0) return;  // still being written
  if (lstrlenW(path) >= static_cast<int>(kPathChars)) return;
  unsigned long long hash = RequestHash(path, size, millis);
  EnterCriticalSection(&g_queue_lock);
  bool seen = false;
  for (int i = 0; i < g_seen_count && !seen; ++i) seen = g_seen[i] == hash;
  if (!seen && g_seen_count < kSeen && g_queue_count < kQueued) {
    bool running = g_worker_running;
    if (!running) {
      HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
      if (thread != nullptr) {
        CloseHandle(thread);
        running = true;
        g_worker_running = true;
      }
    }
    if (running) {
      // Queued under the lock the worker takes before it ends: it is drawn.
      IconRequest& r = g_queue[(g_queue_head + g_queue_count) % kQueued];
      lstrcpyW(r.path, path);
      r.size = size;
      r.millis = millis;
      ++g_queue_count;
      g_seen[g_seen_count++] = hash;
    }
  }
  LeaveCriticalSection(&g_queue_lock);
}

class IconHandler final : public IPersistFile, public IExtractIconW {
 public:
  IconHandler() { InterlockedIncrement(&g_objects); }
  ~IconHandler() { InterlockedDecrement(&g_objects); }

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidPersistFile) ||
        IsEqualIID(riid, kIidPersist)) {
      *ppv = static_cast<IPersistFile*>(this);
    } else if (IsEqualIID(riid, kIidExtractIconW)) {
      *ppv = static_cast<IExtractIconW*>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  // IPersistFile
  STDMETHODIMP GetClassID(CLSID* clsid) override {
    if (clsid == nullptr) return E_POINTER;
    *clsid = kClsidIconHandler;
    return S_OK;
  }
  STDMETHODIMP IsDirty() override { return S_FALSE; }
  STDMETHODIMP Load(LPCOLESTR name, DWORD) override {
    if (name == nullptr || lstrlenW(name) >= static_cast<int>(kPathChars)) return E_INVALIDARG;
    lstrcpyW(path_, name);
    return S_OK;
  }
  STDMETHODIMP Save(LPCOLESTR, BOOL) override { return E_NOTIMPL; }
  STDMETHODIMP SaveCompleted(LPCOLESTR) override { return E_NOTIMPL; }
  STDMETHODIMP GetCurFile(LPOLESTR*) override { return E_NOTIMPL; }

  // IExtractIconW
  STDMETHODIMP GetIconLocation(UINT, PWSTR file, UINT cch, int* index, UINT* flags) override {
    if (file == nullptr || index == nullptr || flags == nullptr || cch == 0) return E_INVALIDARG;
    *index = 0;
    *flags = GIL_PERINSTANCE | GIL_DONTCACHE;
    unsigned long long size = 0, millis = 0;
    if (FileKey(path_, &size, &millis)) {
      if (RecordedIcon(size, millis, file, cch)) return S_OK;
      if (size > 0) {
        // Gesher's icon until the app's is drawn; the shell asks again then.
        RequestIcon(path_, size, millis);
        return DefaultIcon(file, cch) ? S_OK : S_FALSE;
      }
    }
    *flags = 0;  // Gesher's icon is the same for every other APK
    return DefaultIcon(file, cch) ? S_OK : S_FALSE;
  }
  STDMETHODIMP Extract(PCWSTR, UINT, HICON*, HICON*, UINT) override {
    return S_FALSE;  // the shell extracts the icon from the location
  }

 private:
  LONG refs_ = 1;
  wchar_t path_[kPathChars] = {};
};

class Factory final : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidClassFactory)) {
      *ppv = static_cast<IClassFactory*>(this);
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    IconHandler* h = new (std::nothrow) IconHandler();
    if (h == nullptr) return E_OUTOFMEMORY;
    HRESULT hr = h->QueryInterface(riid, ppv);
    h->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_locks);
    } else {
      InterlockedDecrement(&g_locks);
    }
    return S_OK;
  }
};

Factory g_factory;

}  // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    InitializeCriticalSection(&g_queue_lock);
    DisableThreadLibraryCalls(instance);
    // Never unloaded from this process. Explorer calls this handler from many
    // threads, and an object's destructor lowers g_objects before its Release
    // has returned: a DllCanUnloadNow answering S_OK in between let COM unmap
    // the DLL under a thread still running in it, a call into unmapped code
    // that crashed Explorer (0xc0000005 in gesher_shellext.dll_unloaded, or a
    // Control Flow Guard fail-fast, 0xc0000409). A loaded DLL can still be
    // renamed, which is how the installer replaces it (MoveShellExtAside).
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  if (!IsEqualCLSID(clsid, kClsidIconHandler)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

// Never: see DllMain. (g_objects and g_locks are kept for debugging.)
STDAPI DllCanUnloadNow() { return S_FALSE; }

}  // extern "C"
