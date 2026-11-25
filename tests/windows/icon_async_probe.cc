// Focused production-handler contract test; fixture/profile supplied by caller.
// No registry registration, runtime launch or package installation is needed.
#include "../../compat/shell/gesher_shellext.cc"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) return 2;  // APK, cache list, existing ICO
  g_module = GetModuleHandleW(nullptr);
  InitializeCriticalSection(&g_queue_lock);
  IconHandler handler;
  if (handler.Load(argv[1], 0) != S_OK) return 3;
  wchar_t location[kPathChars];
  int index = -1;
  UINT flags = 0;
  HRESULT hr = handler.GetIconLocation(GIL_ASYNC, location, kPathChars, &index, &flags);
  if (hr != E_PENDING) {
    std::printf("FAIL: cold async request returned 0x%08lx, expected E_PENDING\n",
                static_cast<unsigned long>(hr));
    return 1;
  }
  unsigned long long size = 0, millis = 0;
  if (!FileKey(argv[1], &size, &millis)) return 4;
  char path[kPathChars * 4];
  if (!WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, path, sizeof(path), nullptr, nullptr)) return 5;
  char row[kPathChars * 4 + 100];
  int count = std::snprintf(row, sizeof(row), "%llu\t%llu\t%s\tcom.example.fixture\n",
                            size, millis, path);
  HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 6;
  DWORD written = 0;
  bool ok = WriteFile(file, row, static_cast<DWORD>(count), &written, nullptr) &&
            written == static_cast<DWORD>(count);
  CloseHandle(file);
  if (!ok) return 7;
  hr = handler.GetIconLocation(0, location, kPathChars, &index, &flags);
  if (hr != S_OK || lstrcmpW(location, argv[3]) != 0 || index != 0 ||
      (flags & GIL_PERINSTANCE) == 0) return 8;
  hr = handler.GetIconLocation(GIL_ASYNC, location, kPathChars, &index, &flags);
  if (hr != S_OK || lstrcmpW(location, argv[3]) != 0) return 9;
  std::puts("PASS: cold async request is pending; published icon resolves on the same instance");
  return 0;
}
