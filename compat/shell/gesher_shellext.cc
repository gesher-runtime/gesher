// gesher_shellext.dll: the icon handler of Gesher's .apk file type
// (Gesher.Apk\shellex\IconHandler, docs/installer.md). Explorer asks it for each
// .apk file's icon: an APK Gesher installed shows its app's icon (the .ico
// com.gesher.app.shell.AppShell made for the app's Start menu shortcut), any
// other APK Gesher's own (gesher.ico, next to this DLL).
//
// It parses no APK: Explorer loads it in its own process, for every .apk it
// shows. Gesher records the APK files it installs in
// %LOCALAPPDATA%\Gesher\data\system\apk-icons.list (AppShell.rememberApkQuietly), a
// line per file: its size, its last write time (milliseconds since 1970), the
// app's .ico and the package, tab-separated, UTF-8. A file is the one recorded
// when its size and time are; a copy or a move keeps both.
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
    if (name == nullptr || lstrlenW(name) >= MAX_PATH * 4) return E_INVALIDARG;
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
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (path_[0] != L'\0' && GetFileAttributesExW(path_, GetFileExInfoStandard, &data)) {
      unsigned long long size =
          (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      unsigned long long ft = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime)
                               << 32) | data.ftLastWriteTime.dwLowDateTime;
      // FILETIME (100 ns since 1601) as java.io.File.lastModified's milliseconds.
      unsigned long long millis = ft >= 116444736000000000ULL
                                      ? (ft - 116444736000000000ULL) / 10000ULL
                                      : 0;
      if (RecordedIcon(size, millis, file, cch)) return S_OK;
    }
    *flags = 0;  // Gesher's icon is the same for every other APK
    return DefaultIcon(file, cch) ? S_OK : S_FALSE;
  }
  STDMETHODIMP Extract(PCWSTR, UINT, HICON*, HICON*, UINT) override {
    return S_FALSE;  // the shell extracts the icon from the location
  }

 private:
  LONG refs_ = 1;
  wchar_t path_[MAX_PATH * 4] = {};
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
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  if (!IsEqualCLSID(clsid, kClsidIconHandler)) return CLASS_E_CLASSNOTAVAILABLE;
  return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

}  // extern "C"
