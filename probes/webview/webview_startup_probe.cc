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
  std::atomic<bool> session_ok{false};
  std::atomic<gesher::webview::Host*> host{nullptr};
  std::atomic<bool> close_ok{false};
  std::atomic<bool> in_callback{false};
  std::atomic<bool> destroyed_in_callback{false};
  void OnEvent(int32_t type, int64_t id, int32_t arg, const std::wstring& result,
               const std::wstring&, const std::wstring&, const std::vector<uint8_t>*) override {
    ++events;
    if (type == gesher::webview::kReady) ready = true;
    if (type == gesher::webview::kFailed) {
      fwprintf(stderr, L"STARTUP_FAILED hr=0x%08x detail=%ls\n", static_cast<unsigned>(arg),
               result.c_str());
      fflush(stderr);
    }
    if (type == gesher::webview::kSessionResult && id == 1 && arg == 0 &&
        result.find(L"\"value\":42") != std::wstring::npos) session_ok = true;
    wchar_t fault[32] = {};
    if (type == gesher::webview::kSessionResult &&
        GetEnvironmentVariableW(L"GESHER_WEB4_STARTUP_FAULT", fault, 32) != 0 &&
        wcscmp(fault, L"session-close") == 0) {
      in_callback = true;
      auto* current = host.exchange(nullptr);
      close_ok = current != nullptr && current->Destroy();
      in_callback = false;
    }
    if (type == gesher::webview::kSessionResult && wcscmp(fault, L"callback-stall") == 0) {
      fputs("WEB4_CALLBACK_BLOCKED\n", stdout); fflush(stdout);
      Sleep(12000);
    }
  }
  bool OnNavigationStarting(const std::wstring&, bool, bool) override { return false; }
  void OnResourceRequest(int64_t, const std::wstring&, const std::wstring&, const std::wstring&,
                         int32_t, bool) override { ++events; }
  std::wstring OnBridgeCall(const std::wstring&, const std::wstring&,
                            const std::wstring&) override { ++events; return L"{}"; }
  void OnDestroyed() override {
    if (in_callback) destroyed_in_callback = true;
    ++destroyed;
  }
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
  if (argc != 2) return 2;
  bool watchdog = wcscmp(argv[1], L"watchdog") == 0 || wcscmp(argv[1], L"callback-stall") == 0;
  const wchar_t* fault = wcscmp(argv[1], L"watchdog") == 0 ? L"stall" : argv[1];
  if (!SetEnvironmentVariableW(L"GESHER_WEB4_STARTUP_FAULT", fault)) return 2;
  StartupListener listener;
  ULONGLONG begin = GetTickCount64();
  auto* host = gesher::webview::Host::Create(&listener, {});
  ULONGLONG elapsed = GetTickCount64() - begin;
  if (watchdog) {
    if (host == nullptr) return 10;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready) return 11;
    int status = gesher::webview::Host::SubmitSession(host->SessionHandle(), 1, L"",
        L"Runtime.evaluate", L"{\"expression\":\"6*7\",\"returnByValue\":true}",
        wcscmp(argv[1], L"watchdog") == 0 ? 100 : 5000);
    if (status != 0) return 12;
    fputs("WEB4_WATCHDOG_ARMED\n", stdout); fflush(stdout);
    Sleep(15000);
    return 13;  // watchdog failed to terminate the stalled generation
  }
  if (wcscmp(argv[1], L"session-normal") == 0 || wcscmp(argv[1], L"session-close") == 0) {
    if (host == nullptr) return 14;
    bool reentrant = wcscmp(argv[1], L"session-close") == 0;
    listener.host = host;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready || gesher::webview::Host::SubmitSession(host->SessionHandle(), 1, L"",
        L"Runtime.evaluate", L"{\"expression\":\"6*7\",\"returnByValue\":true}", 5000) != 0)
      return 15;
    deadline = GetTickCount64() + 10000;
    while (!listener.session_ok && GetTickCount64() < deadline) Sleep(20);
    if (!listener.session_ok) return 16;
    Sleep(6000); // longer than dispatch watchdog grace; completion must remain nonfatal
    if (reentrant) {
      if (!listener.close_ok || listener.host != nullptr) return 17;
    } else if (!host->Destroy()) return 17;
    deadline = GetTickCount64() + 10000;
    while (listener.destroyed != 1 && GetTickCount64() < deadline) Sleep(20);
    if (listener.destroyed != 1 || listener.destroyed_in_callback) return 18;
    wprintf(L"PASS startup fault=%ls result=42 destroyed=1\n", argv[1]);
    return 0;
  }
  if (wcscmp(argv[1], L"normal") == 0) {
    if (host == nullptr) return 7;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (!listener.ready && GetTickCount64() < deadline) Sleep(20);
    if (!listener.ready) {
      fwprintf(stderr, L"STARTUP_READY_TIMEOUT events=%d destroyed=%d\n",
               listener.events.load(), listener.destroyed.load());
      fflush(stderr);
      return 8;
    }
    if (!host->Destroy()) {
      fwprintf(stderr, L"STARTUP_DESTROY_ENQUEUE_FAILED\n"); fflush(stderr); return 8;
    }
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
