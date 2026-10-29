// Fault-injected startup lifecycle probe. Compiled only into this separate executable.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <atomic>
#include "webview_host.h"

class StartupListener final : public gesher::webview::Listener {
 public:
  std::atomic<int> events{0};
  std::atomic<int> destroyed{0};
  std::atomic<bool> ready{false};
  void OnEvent(int32_t type, int64_t, int32_t, const std::wstring&, const std::wstring&,
               const std::wstring&, const std::vector<uint8_t>*) override {
    ++events;
    if (type == gesher::webview::kReady) ready = true;
  }
  bool OnNavigationStarting(const std::wstring&, bool, bool) override { return false; }
  void OnResourceRequest(int64_t, const std::wstring&, const std::wstring&, const std::wstring&,
                         int32_t, bool) override { ++events; }
  std::wstring OnBridgeCall(const std::wstring&, const std::wstring&,
                            const std::wstring&) override { ++events; return L"{}"; }
  void OnDestroyed() override { ++destroyed; }
};

bool OwnWindow(HWND parent, const wchar_t* window_class) {
  HWND window = nullptr;
  while ((window = FindWindowExW(parent, window, window_class, nullptr)) != nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return true;
  }
  return false;
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || !SetEnvironmentVariableW(L"GESHER_WEB4_STARTUP_FAULT", argv[1])) return 2;
  StartupListener listener;
  ULONGLONG begin = GetTickCount64();
  auto* host = gesher::webview::Host::Create(&listener, {});
  ULONGLONG elapsed = GetTickCount64() - begin;
  if (wcscmp(argv[1], L"normal") == 0) {
    if (host == nullptr) return 7;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready || !host->Destroy()) return 8;
    deadline = GetTickCount64() + 10000;
    while (listener.destroyed != 1 && GetTickCount64() < deadline) Sleep(20);
    if (listener.destroyed != 1) return 9;
    wprintf(L"PASS startup fault=normal ready=1 destroyed=1\n");
    return 0;
  }
  if (host != nullptr) { host->Destroy(); return 3; }
  if (elapsed > 15000 || listener.destroyed != 1 || listener.events != 0) return 4;
  Sleep(3000); // observe late startup after the 10-second timeout
  host = gesher::webview::Host::Create(&listener, {});
  if (host != nullptr) { host->Destroy(); return 5; }
  if (listener.destroyed != 2 || listener.events != 0 ||
      OwnWindow(HWND_MESSAGE, L"GesherWebViewTasks") ||
      OwnWindow(nullptr, L"GesherWebViewParking")) return 6;
  wprintf(L"PASS startup fault=%ls elapsed=%llu destroyed=2 events=0\n", argv[1], elapsed);
  return 0;
}
